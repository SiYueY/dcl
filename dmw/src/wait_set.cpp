// SPDX-License-Identifier: Apache-2.0

#include "dmw/wait_set.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fastdds/dds/core/condition/GuardCondition.hpp>
#include <fastdds/dds/core/condition/StatusCondition.hpp>
#include <fastdds/dds/core/condition/WaitSet.hpp>
#include <fastdds/rtps/common/Time_t.h>

#include "dmw/client.hpp"
#include "dmw/error.hpp"
#include "dmw/event.hpp"
#include "dmw/guard_condition.hpp"
#include "dmw/server.hpp"
#include "dmw/subscriber.hpp"
#include "impl/context_impl.hpp"
#include "impl/endpoint_impl.hpp"
#include "impl/event_impl.hpp"
#include "impl/fastdds/context_state.hpp"
#include "impl/guard_condition_impl.hpp"
#include "impl/reader_wait_state.hpp"
#include "impl/service_impl.hpp"

namespace dmw {

namespace {

std::atomic<std::uint64_t> next_wait_set_id{1};

struct WaitSetWake {
    void notify() noexcept {
        try {
            control_condition->set_trigger_value(true);
        } catch (...) {
            // A failed control wake only delays observation until the bounded
            // Fast DDS wait slice expires; it never rolls back logical state.
        }
    }

    std::shared_ptr<eprosima::fastdds::dds::GuardCondition> control_condition{
        std::make_shared<eprosima::fastdds::dds::GuardCondition>()};
};

enum class RegistrationPhase { Attached, Detaching, Detached };

enum class AttachResult { Attached, AlreadyRegistered, Closing, DdsError };

struct Registration {
    std::uint64_t id{0};
    WaitableKind kind{WaitableKind::Subscriber};
    std::shared_ptr<GuardConditionState> guard;
    std::shared_ptr<impl::ReaderWaitState> reader;
    eprosima::fastdds::dds::StatusCondition* reader_condition{nullptr};
    std::atomic<RegistrationPhase> phase{RegistrationPhase::Attached};

    bool is_closing() const noexcept {
        return guard ? guard->closing.load(std::memory_order_acquire)
                     : reader->closing.load(std::memory_order_acquire);
    }

    template <typename AttachCallback, typename DetachCallback>
    AttachResult claim(
        std::uint64_t wait_set_id, const std::shared_ptr<WaitSetWake>& wake,
        AttachCallback&& attach_callback, DetachCallback&& detach_callback) noexcept {
        if (guard) {
            std::lock_guard<std::mutex> lock(guard->callback_mutex);
            if (guard->closing.load(std::memory_order_acquire)) return AttachResult::Closing;
            if (guard->wait_set_id.load(std::memory_order_acquire) != 0)
                return AttachResult::AlreadyRegistered;
            const auto attached = attach_callback();
            if (attached != AttachResult::Attached) return attached;
            guard->wait_set_id.store(wait_set_id, std::memory_order_release);
            guard->registration_id.store(id, std::memory_order_release);
            guard->wake_callback = [wake] { wake->notify(); };
            guard->detach_callback = std::forward<DetachCallback>(detach_callback);
            return AttachResult::Attached;
        }

        std::lock_guard<std::mutex> lock(reader->callback_mutex);
        if (reader->closing.load(std::memory_order_acquire)) return AttachResult::Closing;
        if (reader->wait_set_id.load(std::memory_order_acquire) != 0)
            return AttachResult::AlreadyRegistered;
        const auto attached = attach_callback();
        if (attached != AttachResult::Attached) return attached;
        reader->wait_set_id.store(wait_set_id, std::memory_order_release);
        reader->registration_id.store(id, std::memory_order_release);
        reader->wake_callback = [wake] { wake->notify(); };
        reader->detach_callback = std::forward<DetachCallback>(detach_callback);
        return AttachResult::Attached;
    }

    template <typename DetachCallback>
    bool release(std::uint64_t wait_set_id, DetachCallback&& detach_callback) noexcept {
        if (guard) {
            std::lock_guard<std::mutex> lock(guard->callback_mutex);
            if (guard->wait_set_id.load(std::memory_order_acquire) != wait_set_id ||
                guard->registration_id.load(std::memory_order_acquire) != id) {
                return true;
            }
            guard->wake_callback = {};
            guard->detach_callback = {};
            guard->registration_id.store(0, std::memory_order_release);
            guard->wait_set_id.store(0, std::memory_order_release);
            return true;
        }

        std::lock_guard<std::mutex> lock(reader->callback_mutex);
        if (reader->wait_set_id.load(std::memory_order_acquire) != wait_set_id ||
            reader->registration_id.load(std::memory_order_acquire) != id) {
            return true;
        }
        if (reader_condition != nullptr && !detach_callback(*reader_condition)) return false;
        reader->wake_callback = {};
        reader->detach_callback = {};
        reader->registration_id.store(0, std::memory_order_release);
        reader->wait_set_id.store(0, std::memory_order_release);
        reader_condition = nullptr;
        return true;
    }

    bool ready() const noexcept {
        if (guard) {
            if (kind == WaitableKind::Event) return guard->pending.load(std::memory_order_acquire);
            return guard->pending.exchange(false, std::memory_order_acq_rel);
        }
        return reader->is_ready();
    }
};

class WaitSetState final : public std::enable_shared_from_this<WaitSetState> {
public:
    WaitSetState(
        std::shared_ptr<impl::fastdds::ContextState> context_state, std::uint64_t wait_set_id)
    : context_state_(std::move(context_state)), wait_set_id_(wait_set_id) {}

    ~WaitSetState() noexcept { close(); }

    Result<void> initialize_shutdown_callback() noexcept {
        {
            std::lock_guard<std::mutex> lock(native_mutex_);
            try {
                const auto result = native_wait_set_.attach_condition(*wake_->control_condition);
                if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
                    return Result<void>::failure(Error(
                        ErrorCode::DdsError, "Fast DDS failed to attach the control condition"));
                }
                control_condition_attached_ = true;
            } catch (...) {
                return Result<void>::failure(
                    Error(ErrorCode::DdsError, "Fast DDS control condition attachment failed"));
            }
        }
        const std::weak_ptr<WaitSetState> weak_state = weak_from_this();
        shutdown_callback_id_ = context_state_->register_shutdown_callback([weak_state] {
            if (const auto state = weak_state.lock()) state->wake_->notify();
        });
        if (shutdown_callback_id_ != 0) return Result<void>::success();

        {
            std::lock_guard<std::mutex> lock(native_mutex_);
            try {
                native_wait_set_.detach_condition(*wake_->control_condition);
            } catch (...) {
                // The Context is already shut down.  The WaitSet destructor
                // will retain its private control condition until its own
                // native object is destroyed.
            }
            control_condition_attached_ = false;
        }
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }

    Result<std::uint64_t> add(
        std::shared_ptr<GuardConditionState> guard, std::shared_ptr<impl::ReaderWaitState> reader,
        WaitableKind kind) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) {
            return Result<std::uint64_t>::failure(
                Error(ErrorCode::ParentDestroyed, "WaitSet is closing"));
        }
        if (next_registration_id_ == 0) {
            return Result<std::uint64_t>::failure(
                Error(ErrorCode::ResourceExhausted, "WaitSet registration IDs are exhausted"));
        }

        auto registration = std::make_shared<Registration>();
        registration->id = next_registration_id_;
        registration->kind = kind;
        registration->guard = std::move(guard);
        registration->reader = std::move(reader);

        const std::weak_ptr<WaitSetState> weak_state = weak_from_this();
        const std::weak_ptr<Registration> weak_registration = registration;
        const auto attach = registration->claim(
            wait_set_id_, wake_,
            [this, registration] { return attach_reader_condition(*registration); },
            [weak_state, weak_registration] {
                const auto state = weak_state.lock();
                const auto detached_registration = weak_registration.lock();
                return state && detached_registration && state->detach(detached_registration);
            });
        if (attach == AttachResult::AlreadyRegistered) {
            return Result<std::uint64_t>::failure(
                Error(ErrorCode::AlreadyRegistered, "Waitable is already registered"));
        }
        if (attach == AttachResult::Closing) {
            return Result<std::uint64_t>::failure(
                Error(ErrorCode::ParentDestroyed, "Waitable is closing"));
        }
        if (attach == AttachResult::DdsError) {
            return Result<std::uint64_t>::failure(
                Error(ErrorCode::DdsError, "Fast DDS failed to attach a reader condition"));
        }
        if (registration->phase.load(std::memory_order_acquire) != RegistrationPhase::Attached) {
            registration->release(
                wait_set_id_, [this](eprosima::fastdds::dds::Condition& condition) {
                    return detach_native_condition(condition);
                });
            return Result<std::uint64_t>::failure(
                Error(ErrorCode::ParentDestroyed, "Waitable is closing"));
        }

        const auto id = next_registration_id_++;
        registrations_.emplace(id, std::move(registration));
        wake_->notify();
        return Result<std::uint64_t>::success(id);
    }

    bool remove(std::uint64_t id, WaitableKind kind) noexcept {
        std::shared_ptr<Registration> registration;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = registrations_.find(id);
            if (found == registrations_.end() || found->second->kind != kind) return false;
            registration = found->second;
        }
        return detach(registration);
    }

    bool detach(const std::shared_ptr<Registration>& registration) noexcept {
        auto expected = RegistrationPhase::Attached;
        if (!registration->phase.compare_exchange_strong(
                expected, RegistrationPhase::Detaching, std::memory_order_acq_rel)) {
            return false;
        }

        if (!registration->release(
                wait_set_id_, [this](eprosima::fastdds::dds::Condition& condition) {
                    return detach_native_condition(condition);
                })) {
            registration->phase.store(RegistrationPhase::Attached, std::memory_order_release);
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = registrations_.find(registration->id);
            if (found != registrations_.end() && found->second == registration) {
                registrations_.erase(found);
            }
        }
        registration->phase.store(RegistrationPhase::Detached, std::memory_order_release);
        wake_->notify();
        return true;
    }

    void close() noexcept {
        std::uint64_t shutdown_callback_id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closing_) return;
            closing_ = true;
            shutdown_callback_id = shutdown_callback_id_;
            shutdown_callback_id_ = 0;
        }
        if (shutdown_callback_id != 0)
            context_state_->unregister_shutdown_callback(shutdown_callback_id);
        while (true) {
            std::shared_ptr<Registration> registration;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (registrations_.empty()) break;
                registration = registrations_.begin()->second;
            }
            if (!detach(registration)) {
                // A failed native detach leaves the registration in place so
                // its reader remains retained until this WaitSet is gone.
                break;
            }
        }
        {
            std::lock_guard<std::mutex> lock(native_mutex_);
            if (control_condition_attached_) {
                try {
                    native_wait_set_.detach_condition(*wake_->control_condition);
                } catch (...) {
                    // No public object owns this private control condition.
                    // Keep it alive with the WaitSet state on teardown.
                }
                control_condition_attached_ = false;
            }
        }
        wake_->notify();
    }

    Result<void> begin_wait() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) {
            return Result<void>::failure(Error(ErrorCode::ParentDestroyed, "WaitSet is closing"));
        }
        if (waiting_) {
            return Result<void>::failure(
                Error(ErrorCode::Busy, "WaitSet already has an active wait"));
        }
        waiting_ = true;
        return Result<void>::success();
    }

    void end_wait() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        waiting_ = false;
    }

    Result<void> wait_for_notification(std::chrono::nanoseconds timeout) {
        constexpr auto kNanosecondsPerSecond = std::chrono::nanoseconds::period::den;
        const auto count = timeout.count();
        const auto seconds = count / kNanosecondsPerSecond;
        const auto nanoseconds = count % kNanosecondsPerSecond;
        const eprosima::fastrtps::Duration_t duration(
            static_cast<std::int32_t>(seconds), static_cast<std::uint32_t>(nanoseconds));
        eprosima::fastdds::dds::ConditionSeq active_conditions;
        eprosima::fastrtps::types::ReturnCode_t result;
        {
            std::lock_guard<std::mutex> lock(native_mutex_);
            result = native_wait_set_.wait(active_conditions, duration);
            wake_->control_condition->set_trigger_value(false);
        }
        if (result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK ||
            result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_TIMEOUT) {
            return Result<void>::success();
        }
        return Result<void>::failure(Error(ErrorCode::DdsError, "Fast DDS WaitSet wait failed"));
    }

private:
    AttachResult attach_reader_condition(Registration& registration) noexcept {
        if (!registration.reader) return AttachResult::Attached;
        std::lock_guard<std::mutex> reader_lock(registration.reader->reader_mutex);
        if (registration.reader->closing.load(std::memory_order_acquire) ||
            registration.reader->reader == nullptr) {
            return AttachResult::Closing;
        }
        auto& condition = registration.reader->reader->get_statuscondition();
        std::lock_guard<std::mutex> lock(native_mutex_);
        try {
            if (condition.set_enabled_statuses(
                    eprosima::fastdds::dds::StatusMask::data_available()) !=
                eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
                return AttachResult::DdsError;
            }
            const auto result = native_wait_set_.attach_condition(condition);
            if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
                return AttachResult::DdsError;
            }
            registration.reader_condition = &condition;
            return AttachResult::Attached;
        } catch (...) {
            return AttachResult::DdsError;
        }
    }

    bool detach_native_condition(eprosima::fastdds::dds::Condition& condition) noexcept {
        std::lock_guard<std::mutex> lock(native_mutex_);
        try {
            return native_wait_set_.detach_condition(condition) ==
                   eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
        } catch (...) {
            return false;
        }
    }

public:
    std::vector<std::shared_ptr<Registration>> snapshot() const {
        std::vector<std::shared_ptr<Registration>> registrations;
        std::lock_guard<std::mutex> lock(mutex_);
        registrations.reserve(registrations_.size());
        for (const auto& entry : registrations_) {
            if (entry.second->phase.load(std::memory_order_acquire) == RegistrationPhase::Attached)
                registrations.push_back(entry.second);
        }
        return registrations;
    }

    bool is_closing() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return closing_;
    }

    std::shared_ptr<impl::fastdds::ContextState> context_state_;
    const std::uint64_t wait_set_id_;
    std::shared_ptr<WaitSetWake> wake_{std::make_shared<WaitSetWake>()};

    mutable std::mutex mutex_;
    std::mutex native_mutex_;
    eprosima::fastdds::dds::WaitSet native_wait_set_;
    std::uint64_t next_registration_id_{1};
    std::uint64_t shutdown_callback_id_{0};
    bool control_condition_attached_{false};
    bool closing_{false};
    bool waiting_{false};
    std::unordered_map<std::uint64_t, std::shared_ptr<Registration>> registrations_;
};

class WaitActivityGuard {
public:
    explicit WaitActivityGuard(std::shared_ptr<WaitSetState> state) noexcept
    : state_(std::move(state)) {}
    ~WaitActivityGuard() noexcept { state_->end_wait(); }

    WaitActivityGuard(const WaitActivityGuard&) = delete;
    WaitActivityGuard& operator=(const WaitActivityGuard&) = delete;

private:
    std::shared_ptr<WaitSetState> state_;
};

Result<std::uint64_t> add_guard(
    const std::shared_ptr<WaitSetState>& state, const std::shared_ptr<GuardConditionState>& guard,
    WaitableKind kind) {
    if (guard->context_state != state->context_state_) {
        return Result<std::uint64_t>::failure(
            Error(ErrorCode::InvalidArgument, "Waitable belongs to another Context"));
    }
    const auto operation = state->context_state_->try_acquire_operation();
    if (!operation) {
        return Result<std::uint64_t>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    auto registration = state->add(guard, nullptr, kind);
    if (!registration) return Result<std::uint64_t>::failure(std::move(registration.error()));
    return registration;
}

Result<std::uint64_t> add_reader(
    const std::shared_ptr<WaitSetState>& state,
    const std::shared_ptr<impl::ReaderWaitState>& reader, WaitableKind kind) {
    if (reader->context_state != state->context_state_) {
        return Result<std::uint64_t>::failure(
            Error(ErrorCode::InvalidArgument, "Waitable belongs to another Context"));
    }
    const auto operation = state->context_state_->try_acquire_operation();
    if (!operation) {
        return Result<std::uint64_t>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    auto registration = state->add(nullptr, reader, kind);
    if (!registration) return Result<std::uint64_t>::failure(std::move(registration.error()));
    return registration;
}

}  // namespace

class WaitSet::Impl {
public:
    explicit Impl(std::shared_ptr<WaitSetState> state) noexcept : state_(std::move(state)) {}

    std::shared_ptr<WaitSetState> state_;
};

WaitSet::WaitSet(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
WaitSet::~WaitSet() noexcept { impl_->state_->close(); }

Result<std::unique_ptr<WaitSet>> Context::create_wait_set(const WaitSetOptions&) {
    const auto operation = impl_->state_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<WaitSet>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    const auto wait_set_id = next_wait_set_id.fetch_add(1, std::memory_order_relaxed);
    if (wait_set_id == 0) {
        return Result<std::unique_ptr<WaitSet>>::failure(
            Error(ErrorCode::ResourceExhausted, "WaitSet IDs are exhausted"));
    }
    const auto state = std::make_shared<WaitSetState>(impl_->state_, wait_set_id);
    auto initialized = state->initialize_shutdown_callback();
    if (!initialized)
        return Result<std::unique_ptr<WaitSet>>::failure(std::move(initialized.error()));
    return Result<std::unique_ptr<WaitSet>>::success(
        std::unique_ptr<WaitSet>(new WaitSet(std::make_unique<WaitSet::Impl>(state))));
}

Result<WaitToken> WaitSet::add(GuardCondition& guard_condition) {
    auto registration =
        add_guard(impl_->state_, guard_condition.impl_->state_, WaitableKind::GuardCondition);
    if (!registration) return Result<WaitToken>::failure(std::move(registration.error()));
    return Result<WaitToken>::success(
        WaitToken(impl_->state_->wait_set_id_, registration.value(), WaitableKind::GuardCondition));
}

Result<WaitToken> WaitSet::add(Subscriber& subscriber) {
    auto registration =
        add_reader(impl_->state_, subscriber.impl_->wait_state_, WaitableKind::Subscriber);
    if (!registration) return Result<WaitToken>::failure(std::move(registration.error()));
    return Result<WaitToken>::success(
        WaitToken(impl_->state_->wait_set_id_, registration.value(), WaitableKind::Subscriber));
}

Result<WaitToken> WaitSet::add(Client& client) {
    auto registration =
        add_reader(impl_->state_, client.impl_->response_wait_state_, WaitableKind::Client);
    if (!registration) return Result<WaitToken>::failure(std::move(registration.error()));
    return Result<WaitToken>::success(
        WaitToken(impl_->state_->wait_set_id_, registration.value(), WaitableKind::Client));
}

Result<WaitToken> WaitSet::add(Server& server) {
    auto registration =
        add_reader(impl_->state_, server.impl_->request_wait_state_, WaitableKind::Server);
    if (!registration) return Result<WaitToken>::failure(std::move(registration.error()));
    return Result<WaitToken>::success(
        WaitToken(impl_->state_->wait_set_id_, registration.value(), WaitableKind::Server));
}

Result<WaitToken> WaitSet::add(Event& event) {
    auto registration = add_guard(impl_->state_, event.impl_->wait_state_, WaitableKind::Event);
    if (!registration) return Result<WaitToken>::failure(std::move(registration.error()));
    return Result<WaitToken>::success(
        WaitToken(impl_->state_->wait_set_id_, registration.value(), WaitableKind::Event));
}

Result<void> WaitSet::remove(WaitToken token) {
    const auto state = impl_->state_;
    if (!token.valid() || token.wait_set_id_ != state->wait_set_id_) {
        return Result<void>::failure(
            Error(ErrorCode::InvalidArgument, "Token belongs to another WaitSet"));
    }
    const auto operation = state->context_state_->try_acquire_operation();
    if (!operation) {
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    if (!state->remove(token.registration_id_, token.kind_)) {
        return Result<void>::failure(Error(ErrorCode::NotRegistered, "WaitSet token is stale"));
    }
    return Result<void>::success();
}

Result<WaitResult> WaitSet::wait(WaitTimeout timeout) {
    const auto state = impl_->state_;
    const auto operation = state->context_state_->try_acquire_operation();
    if (!operation) {
        return Result<WaitResult>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    auto begin = state->begin_wait();
    if (!begin) return Result<WaitResult>::failure(std::move(begin.error()));
    const WaitActivityGuard active_wait(state);
    const auto deadline = timeout.kind() == WaitTimeout::Kind::Finite
                              ? std::chrono::steady_clock::now() + timeout.duration()
                              : std::chrono::steady_clock::time_point::max();

    while (true) {
        if (state->context_state_->is_shutdown()) {
            return Result<WaitResult>::failure(
                Error(ErrorCode::ContextShutdown, "Context is shut down"));
        }
        if (state->is_closing()) {
            return Result<WaitResult>::failure(
                Error(ErrorCode::ParentDestroyed, "WaitSet is closing"));
        }

        std::vector<WaitToken> tokens;
        for (const auto& registration : state->snapshot()) {
            if (registration->is_closing()) {
                state->detach(registration);
                continue;
            }
            if (registration->ready()) {
                const WaitToken token(state->wait_set_id_, registration->id, registration->kind);
                tokens.push_back(token);
            }
        }
        if (!tokens.empty()) {
            return Result<WaitResult>::success(WaitResult::ready(std::move(tokens)));
        }
        if (timeout.kind() == WaitTimeout::Kind::Poll) {
            return Result<WaitResult>::success(WaitResult::timeout());
        }
        if (timeout.kind() == WaitTimeout::Kind::Finite &&
            std::chrono::steady_clock::now() >= deadline) {
            return Result<WaitResult>::success(WaitResult::timeout());
        }

        const auto slice = std::chrono::milliseconds(10);
        const auto wake_deadline =
            timeout.kind() == WaitTimeout::Kind::Finite
                ? std::min(deadline, std::chrono::steady_clock::now() + slice)
                : std::chrono::steady_clock::now() + slice;
        const auto remaining = wake_deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
            continue;
        }
        const auto wake = state->wait_for_notification(
            std::chrono::duration_cast<std::chrono::nanoseconds>(remaining));
        if (!wake) {
            return Result<WaitResult>::failure(std::move(wake.error()));
        }
    }
}

}  // namespace dmw
