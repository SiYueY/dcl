#ifndef DMW_IMPL__WAIT_SET_IMPL_HPP_
#define DMW_IMPL__WAIT_SET_IMPL_HPP_

#include "dmw/wait_set.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <limits>
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
#include "impl/publisher_impl.hpp"
#include "impl/subscriber_impl.hpp"
#include "impl/event_impl.hpp"
#include "impl/fastdds/context.hpp"
#include "impl/fastdds/return_code.hpp"
#include "impl/guard_condition_impl.hpp"
#include "impl/lock_rank.hpp"
#include "impl/reader_wait_state.hpp"
#include "impl/client_impl.hpp"
#include "impl/server_impl.hpp"

namespace dmw {

namespace impl {

inline std::atomic<std::uint64_t> next_wait_set_id{1};

struct WaitSetWake {
    void notify() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        try {
            if (control_condition->set_trigger_value(true) !=
                eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
                broken.store(true, std::memory_order_release);
            }
        } catch (...) {
            broken.store(true, std::memory_order_release);
        }
    }

    bool clear() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        try {
            if (control_condition->set_trigger_value(false) ==
                eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK)
                return true;
        } catch (...) {
        }
        broken.store(true, std::memory_order_release);
        return false;
    }

    bool replace(eprosima::fastdds::dds::WaitSet& wait_set) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        try {
            if (wait_set.detach_condition(*control_condition) !=
                eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK)
                return false;
            auto replacement = std::make_shared<eprosima::fastdds::dds::GuardCondition>();
            if (wait_set.attach_condition(*replacement) !=
                eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK)
                return false;
            control_condition = std::move(replacement);
            broken.store(false, std::memory_order_release);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool detach(eprosima::fastdds::dds::WaitSet& wait_set) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        try {
            return wait_set.detach_condition(*control_condition) ==
                   eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
        } catch (...) {
            return false;
        }
    }

    std::mutex mutex;
    std::atomic<bool> broken{false};
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
            std::lock_guard lock(guard->callback_mutex);
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

        {
            std::lock_guard lock(reader->callback_mutex);
            if (reader->closing.load(std::memory_order_acquire)) return AttachResult::Closing;
            if (reader->wait_set_id.load(std::memory_order_acquire) != 0 ||
                reader->claim_in_progress) {
                return AttachResult::AlreadyRegistered;
            }
            reader->claim_in_progress = true;
        }
        const auto attached = attach_callback();
        std::function<bool()> rollback;
        bool closed_during_claim = false;
        {
            std::lock_guard lock(reader->callback_mutex);
            reader->claim_in_progress = false;
            reader->callback_cv.notify_all();
            if (attached != AttachResult::Attached) return attached;
            reader->wait_set_id.store(wait_set_id, std::memory_order_release);
            reader->registration_id.store(id, std::memory_order_release);
            reader->wake_callback = [wake] { wake->notify(); };
            reader->detach_callback = std::forward<DetachCallback>(detach_callback);
            closed_during_claim = reader->closing.load(std::memory_order_acquire);
            if (closed_during_claim) rollback = reader->detach_callback;
        }
        if (closed_during_claim) {
            if (rollback) (void)rollback();
            return AttachResult::Closing;
        }
        return AttachResult::Attached;
    }

    template <typename DetachCallback>
    bool release(std::uint64_t wait_set_id, DetachCallback&& detach_callback) noexcept {
        if (guard) {
            std::lock_guard lock(guard->callback_mutex);
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

        eprosima::fastdds::dds::StatusCondition* condition = nullptr;
        {
            std::lock_guard lock(reader->callback_mutex);
            if (reader->wait_set_id.load(std::memory_order_acquire) != wait_set_id ||
                reader->registration_id.load(std::memory_order_acquire) != id) {
                return true;
            }
            condition = reader_condition;
        }
        // Native WaitSet reconciliation (rank 13) must not occur while the
        // waitable-local callback lock (rank 15) is held.
        if (condition != nullptr && !detach_callback(*condition)) return false;
        {
            std::lock_guard lock(reader->callback_mutex);
            // Detach owns the registration phase, so any changed identity
            // means a concurrent close already completed the cleanup.
            if (reader->wait_set_id.load(std::memory_order_acquire) != wait_set_id ||
                reader->registration_id.load(std::memory_order_acquire) != id) {
                return true;
            }
            reader->wake_callback = {};
            reader->detach_callback = {};
            reader->topology_callback = {};
            reader->quarantined_wait_set.reset();
            reader->registration_id.store(0, std::memory_order_release);
            reader->wait_set_id.store(0, std::memory_order_release);
            reader_condition = nullptr;
        }
        reader->complete_deferred_delete();
        return true;
    }

    bool ready() const noexcept {
        if (guard) {
            if (kind == WaitableKind::Event) return guard->pending.load(std::memory_order_acquire);
            return guard->consume_trigger();
        }
        return reader->is_ready();
    }
};

class WaitSetState final : public std::enable_shared_from_this<WaitSetState> {
public:
    WaitSetState(std::shared_ptr<impl::fastdds::Context> context_state, std::uint64_t wait_set_id)
    : context_state_(std::move(context_state)), wait_set_id_(wait_set_id) {}

    ~WaitSetState() noexcept { close(); }

    Result<void> initialize_shutdown_callback() noexcept {
        {
            std::lock_guard lock(native_mutex_);
            try {
                const auto result = native_wait_set_.attach_condition(*wake_->control_condition);
                if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
                    return Result<void>::failure(Error(impl::fastdds::to_error(
                        result, "Fast DDS failed to attach the control condition")));
                }
                control_condition_attached_ = true;
            } catch (...) {
                return Result<void>::failure(
                    Error(ErrorCode::DdsError, "Fast DDS control condition attachment failed"));
            }
        }
        note_topology_mutation();
        const std::weak_ptr<WaitSetState> weak_state = weak_from_this();
        shutdown_callback_id_ = context_state_->register_shutdown_callback([weak_state] {
            if (const auto state = weak_state.lock()) state->wake_->notify();
        });
        if (shutdown_callback_id_ != 0) return Result<void>::success();

        {
            std::lock_guard lock(native_mutex_);
            try {
                (void)wake_->detach(native_wait_set_);
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
        std::lock_guard lock(mutex_);
        if (closing_) {
            return Result<std::uint64_t>::failure(
                Error(ErrorCode::ParentDestroyed, "WaitSet is closing"));
        }
        if (poisoned_) {
            return Result<std::uint64_t>::failure(
                Error(ErrorCode::DdsError, "WaitSet topology is poisoned"));
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

        if (registration->reader) {
            std::lock_guard reader_lock(registration->reader->callback_mutex);
            registration->reader->topology_callback = [weak_state,
                                                       weak_registration](bool enabled) {
                const auto state = weak_state.lock();
                const auto current = weak_registration.lock();
                if (state && current) state->set_reader_blocking(*current, enabled);
            };
        }

        const auto id = next_registration_id_;
        try {
            registrations_.emplace(id, registration);
        } catch (...) {
            // claim() published registration state before this allocation.  A
            // failed map insertion must restore the waitable to the exact
            // pre-add state so a retry is not spuriously AlreadyRegistered.
            registration->release(
                wait_set_id_, [this](eprosima::fastdds::dds::Condition& condition) {
                    return detach_native_condition(condition);
                });
            throw;
        }
        ++next_registration_id_;
        note_topology_mutation();
        wake_->notify();
        return Result<std::uint64_t>::success(id);
    }

    bool remove(std::uint64_t id, WaitableKind kind) noexcept {
        std::shared_ptr<Registration> registration;
        {
            std::lock_guard lock(mutex_);
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
            if (registration->reader) {
                registration->reader->quarantine_wait_set(shared_from_this());
            }
            registration->phase.store(RegistrationPhase::Attached, std::memory_order_release);
            {
                std::lock_guard lock(mutex_);
                poisoned_ = true;
            }
            wake_->notify();
            return false;
        }
        {
            std::lock_guard lock(mutex_);
            const auto found = registrations_.find(registration->id);
            if (found != registrations_.end() && found->second == registration) {
                registrations_.erase(found);
            }
        }
        registration->phase.store(RegistrationPhase::Detached, std::memory_order_release);
        note_topology_mutation();
        wake_->notify();
        return true;
    }

    void set_reader_blocking(Registration& registration, bool enabled) noexcept {
        if (!registration.reader ||
            registration.phase.load(std::memory_order_acquire) != RegistrationPhase::Attached) {
            return;
        }
        if (registration.reader->closing.load(std::memory_order_acquire) ||
            registration.reader->reader == nullptr)
            return;
        if (!enabled) {
            if (registration.reader_condition != nullptr &&
                !detach_native_condition(*registration.reader_condition)) {
                std::lock_guard lock(mutex_);
                poisoned_ = true;
                return;
            }
            registration.reader_condition = nullptr;
        } else if (registration.reader_condition == nullptr) {
            if (attach_reader_condition(registration) != AttachResult::Attached) {
                std::lock_guard lock(mutex_);
                poisoned_ = true;
                return;
            }
        }
        note_topology_mutation();
        wake_->notify();
    }

    void close() noexcept {
        std::uint64_t shutdown_callback_id = 0;
        {
            std::lock_guard lock(mutex_);
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
                std::lock_guard lock(mutex_);
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
            std::lock_guard lock(native_mutex_);
            if (control_condition_attached_) {
                if (!wake_->detach(native_wait_set_)) {
                    // No public object owns this private control condition.
                    // Keep it alive with the WaitSet state on teardown.
                }
                control_condition_attached_ = false;
            }
        }
        wake_->notify();
    }

    Result<void> begin_wait() {
        std::lock_guard lock(mutex_);
        if (closing_) {
            return Result<void>::failure(Error(ErrorCode::ParentDestroyed, "WaitSet is closing"));
        }
        if (poisoned_) {
            return Result<void>::failure(
                Error(ErrorCode::DdsError, "WaitSet topology is poisoned"));
        }
        if (waiting_) {
            return Result<void>::failure(
                Error(ErrorCode::Busy, "WaitSet already has an active wait"));
        }
        waiting_ = true;
        return Result<void>::success();
    }

    void end_wait() noexcept {
        std::lock_guard lock(mutex_);
        waiting_ = false;
    }

    std::uint64_t topology_generation() const noexcept {
        return topology_generation_.load(std::memory_order_acquire);
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
            std::lock_guard lock(native_mutex_);
            result = native_wait_set_.wait(active_conditions, duration);
        }
        (void)wake_->clear();
        if (result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK ||
            result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_TIMEOUT) {
            return Result<void>::success();
        }
        return Result<void>::failure(
            impl::fastdds::to_error(result, "Fast DDS WaitSet wait failed"));
    }

    Result<void> repair_control_guard_if_needed() noexcept {
        if (!wake_->broken.load(std::memory_order_acquire)) return Result<void>::success();
        std::lock_guard lock(native_mutex_);
        if (!wake_->broken.load(std::memory_order_acquire)) return Result<void>::success();
        if (!wake_->replace(native_wait_set_)) {
            poisoned_.store(true, std::memory_order_release);
            return Result<void>::failure(
                Error(ErrorCode::DdsError, "Fast DDS control guard replacement failed"));
        }
        note_topology_mutation();
        return Result<void>::success();
    }

private:
    void note_topology_mutation() noexcept {
        auto generation = topology_generation_.load(std::memory_order_acquire);
        while (true) {
            if (generation == std::numeric_limits<std::uint64_t>::max()) {
                poisoned_.store(true, std::memory_order_release);
                return;
            }
            if (topology_generation_.compare_exchange_weak(
                    generation, generation + 1, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return;
            }
        }
    }

    AttachResult attach_reader_condition(Registration& registration) noexcept {
        if (!registration.reader) return AttachResult::Attached;
        if (!registration.reader->blocking_enabled.load(std::memory_order_acquire))
            return AttachResult::Attached;
        std::lock_guard<std::mutex> reader_lock(registration.reader->reader_mutex);
        if (registration.reader->closing.load(std::memory_order_acquire) ||
            registration.reader->reader == nullptr) {
            return AttachResult::Closing;
        }
        auto& condition = registration.reader->reader->get_statuscondition();
        std::lock_guard lock(native_mutex_);
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
        std::lock_guard lock(native_mutex_);
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
        std::lock_guard lock(mutex_);
        registrations.reserve(registrations_.size());
        for (const auto& entry : registrations_) {
            if (entry.second->phase.load(std::memory_order_acquire) == RegistrationPhase::Attached)
                registrations.push_back(entry.second);
        }
        return registrations;
    }

    bool is_closing() const noexcept {
        std::lock_guard lock(mutex_);
        return closing_;
    }

    std::shared_ptr<impl::fastdds::Context> context_state_;
    const std::uint64_t wait_set_id_;
    std::shared_ptr<WaitSetWake> wake_{std::make_shared<WaitSetWake>()};

    mutable impl::RankedMutex<impl::LockRank::WaitSetTopology> mutex_;
    impl::RankedMutex<impl::LockRank::WaitSetReconciliation> native_mutex_;
    eprosima::fastdds::dds::WaitSet native_wait_set_;
    std::uint64_t next_registration_id_{1};
    std::uint64_t shutdown_callback_id_{0};
    bool control_condition_attached_{false};
    bool closing_{false};
    bool waiting_{false};
    std::atomic<bool> poisoned_{false};
    std::atomic<std::uint64_t> topology_generation_{1};
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

inline Result<std::uint64_t> add_guard(
    const std::shared_ptr<WaitSetState>& state, const std::shared_ptr<GuardConditionState>& guard,
    WaitableKind kind) {
    if (guard->context() != state->context_state_) {
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

inline Result<std::uint64_t> add_reader(
    const std::shared_ptr<WaitSetState>& state,
    const std::shared_ptr<impl::ReaderWaitState>& reader, WaitableKind kind) {
    if (reader->context() != state->context_state_) {
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

}  // namespace impl

class WaitSet::Impl {
public:
    explicit Impl(std::shared_ptr<impl::WaitSetState> state) noexcept : state_(std::move(state)) {}

    ~Impl() noexcept { state_->close(); }
    Result<WaitToken> add(const std::shared_ptr<GuardConditionState>&, WaitableKind);
    Result<WaitToken> add(const std::shared_ptr<impl::ReaderWaitState>&, WaitableKind);
    Result<void> remove(WaitToken);
    Result<WaitResult> wait(WaitTimeout);

private:
    std::shared_ptr<impl::WaitSetState> state_;
};

}  // namespace dmw

#endif  // DMW_IMPL__WAIT_SET_IMPL_HPP_
