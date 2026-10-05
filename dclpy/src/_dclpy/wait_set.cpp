#include "wait_set.hpp"


namespace dclpy::detail {

NativeWaitSet::~NativeWaitSet() noexcept {
    try { close(); } catch (...) {
        // If DDS refuses detach, preserve backing claims. DMW itself retains
        // the affected reader/WaitSet through its quarantine barrier.
        backing_->close();
        guard_->close();
    }
}

std::uint64_t NativeWaitSet::add(std::shared_ptr<NativeWaitable> value, std::uint64_t generation) {
    if (!value) throw std::invalid_argument("Missing waitable");
    std::lock_guard lock(registrations_mutex_);
    if (closed_) throw EntityClosed();
    if (next_registration_ == std::numeric_limits<std::uint64_t>::max())
        throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::ResourceExhausted, "Registration identities are exhausted"));
    const auto token = next_registration_++;
    // Prepare the map entry before native attach: committing a successful
    // attach must not allocate or lose its registration on bad_alloc.
    auto inserted = registrations_.emplace(token, Entry{std::move(value), generation, {}}).first;
    try {
        inserted->second.native = unwrap(inserted->second.entity->register_with(*control_.get()));
    } catch (...) {
        registrations_.erase(inserted);
        throw;
    }
    return token;
}

void NativeWaitSet::remove(std::uint64_t token) {
    std::lock_guard lock(registrations_mutex_);
    const auto found = registrations_.find(token);
    if (found == registrations_.end())
        throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::NotRegistered, "Registration is stale"));
    unwrap(control_->remove(found->second.native));
    found->second.entity->mark_unregistered();
    registrations_.erase(found);
}

void NativeWaitSet::set_interest(std::uint64_t token, std::uint32_t mask) {
    std::lock_guard lock(registrations_mutex_);
    const auto found = registrations_.find(token);
    if (found == registrations_.end())
        throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::NotRegistered, "Registration is stale"));
    unwrap(control_->set_interest(found->second.native, mask));
}

void NativeWaitSet::wake() {
    auto lease = [&] {
        std::lock_guard lock(registrations_mutex_);
        if (closed_) throw EntityClosed();
        return guard_control_.derive_cleanup();
    }();
    unwrap(lease->trigger());
}

ReadyBatch NativeWaitSet::wait(dmw::WaitTimeout timeout) {
    auto lease = [&] {
        std::lock_guard lock(registrations_mutex_);
        if (closed_) throw EntityClosed();
        if (waiting_) throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::Busy, "WaitSet already has an active wait"));
        auto lease = control_.derive_cleanup();
        waiting_ = true;
        return lease;
    }();
    struct EndWait {
        NativeWaitSet& owner;
        ~EndWait() noexcept {
            std::lock_guard lock(owner.registrations_mutex_);
            owner.waiting_ = false;
        }
    } end{*this};
    const auto result = unwrap(lease->wait(timeout));
    std::vector<NativeReady> ready;
    ready.reserve(result.ready().size());
    std::lock_guard lock(registrations_mutex_);
    for (const auto& item : result.ready()) {
        for (const auto& entry : registrations_) {
            if (entry.second.native != item.registration) continue;
            try {
                // This map lock also serializes remove/finalize, so a stale
                // batch cannot obtain a pin after its native resource closes.
                auto pin = entry.second.entity->pin_dispatch();
                ready.push_back({entry.first, entry.second.generation, item.detail_mask, std::move(pin)});
            } catch (const EntityClosed&) {
            } catch (const ContextShutdown&) {
            }
            break;
        }
    }
    return ReadyBatch(std::move(ready));
}

void NativeWaitSet::close() {
    std::lock_guard lock(registrations_mutex_);
    if (closed_) return;
    if (waiting_) throw MiddlewareFailure(dmw::Error(dmw::ErrorCode::Busy, "WaitSet still has an active wait"));
    // The owner joins its wait thread and retires every mailbox producer before
    // closing. No active wait may race these permanent control leases.
    for (auto entry = registrations_.begin(); entry != registrations_.end();) {
        unwrap(control_->remove(entry->second.native));
        entry->second.entity->mark_unregistered();
        entry = registrations_.erase(entry);
    }
    backing_->close();
    control_.reset();
    guard_->close();
    guard_control_.reset();
    closed_ = true;
}

}  // namespace dclpy::detail
