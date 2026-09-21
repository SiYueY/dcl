#ifndef DMW_IMPL__WAIT_SET_IMPL_HPP_
#define DMW_IMPL__WAIT_SET_IMPL_HPP_

#include "dmw/wait_set.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <limits>
#include <optional>
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
#include "dmw/graph_event.hpp"
#include "dmw/guard_condition.hpp"
#include "dmw/server.hpp"
#include "dmw/subscriber.hpp"
#include "impl/context_impl.hpp"
#include "impl/publisher_impl.hpp"
#include "impl/subscriber_impl.hpp"
#include "impl/event_impl.hpp"
#include "impl/context.hpp"
#include "impl/return_code.hpp"
#include "impl/guard_condition_impl.hpp"
#include "impl/graph_impl.hpp"
#include "impl/lock_rank.hpp"
#include "impl/reader_wait_state.hpp"
#include "impl/client_impl.hpp"
#include "impl/server_impl.hpp"
#include "impl/timer_impl.hpp"

namespace dmw {

namespace impl {

inline std::atomic<std::uint64_t> next_wait_set_id{1};

struct WaitSetWake {
    Result<void> notify() {
        generation.fetch_add(1, std::memory_order_acq_rel);
        std::lock_guard<std::mutex> lock(mutex);
        try {
            if (control_condition->set_trigger_value(true) !=
                eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
                broken.store(true, std::memory_order_release);
                return Result<void>::failure(
                    Error(ErrorCode::DDSError, "Fast DDS control condition trigger failed"));
            }
        } catch (...) {
            broken.store(true, std::memory_order_release);
            return Result<void>::failure(
                Error(ErrorCode::DDSError, "Fast DDS control condition trigger failed"));
        }
        return Result<void>::success();
    }

    bool clear_if_unchanged(std::uint64_t observed_generation) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (generation.load(std::memory_order_acquire) != observed_generation) return false;
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
    std::atomic<std::uint64_t> generation{0};
    std::atomic<bool> broken{false};
    std::shared_ptr<eprosima::fastdds::dds::GuardCondition> control_condition{
        std::make_shared<eprosima::fastdds::dds::GuardCondition>()};
};

enum class RegistrationPhase { Attached, Detaching, Detached };

enum class AttachResult { Attached, AlreadyRegistered, Closing, DDSError };

struct Registration {
    std::uint64_t id{0};
    WaitableKind kind{WaitableKind::Subscriber};
    std::shared_ptr<GuardConditionState> guard;
    std::shared_ptr<impl::TimerState> timer;
    std::shared_ptr<impl::GraphEventState> graph_event;
    /// One constituent reader for a single-endpoint waitable, or the whole set
    /// of channels behind one Action aggregate token.
    std::vector<std::shared_ptr<impl::ReaderWaitState>> readers;
    /// Native conditions attached for `readers`, parallel and possibly null.
    std::vector<eprosima::fastdds::dds::StatusCondition*> reader_conditions;
    /// Optional logical sub-channel provider used by aggregates whose
    /// readiness is not limited to reader data availability.
    std::function<std::uint32_t()> logical_detail;
    /// Optional absolute steady deadline that must shorten a native wait.
    std::function<std::optional<std::chrono::steady_clock::time_point>()> runtime_deadline;
    std::atomic<RegistrationPhase> phase{RegistrationPhase::Attached};

    bool is_closing() const noexcept {
        if (guard) return guard->closing.load(std::memory_order_acquire);
        if (timer) return timer->closing.load(std::memory_order_acquire);
        if (graph_event) return graph_event->closing.load(std::memory_order_acquire);
        for (const auto& reader : readers) {
            if (reader->closing.load(std::memory_order_acquire)) return true;
        }
        return false;
    }

    template <typename AttachCallback, typename DetachCallback>
    AttachResult claim(
        std::uint64_t wait_set_id, const std::shared_ptr<WaitSetWake>& wake,
        AttachCallback&& attach_callback, DetachCallback&& detach_callback) noexcept {
        if (guard) {
            return claim_logical_waitable(
                *guard, wait_set_id, wake, std::forward<AttachCallback>(attach_callback),
                std::forward<DetachCallback>(detach_callback));
        }
        if (timer) {
            return claim_logical_waitable(
                *timer, wait_set_id, wake, std::forward<AttachCallback>(attach_callback),
                std::forward<DetachCallback>(detach_callback));
        }
        if (graph_event) {
            return claim_logical_waitable(
                *graph_event, wait_set_id, wake, std::forward<AttachCallback>(attach_callback),
                std::forward<DetachCallback>(detach_callback));
        }

        // Every constituent reader is claimed under the same public token.  A
        // reader is claimed one lock at a time because same-rank nesting is
        // forbidden by the lock hierarchy.
        for (const auto& reader : readers) {
            std::lock_guard lock(reader->callback_mutex);
            if (reader->closing.load(std::memory_order_acquire)) {
                clear_reader_claims();
                return AttachResult::Closing;
            }
            if (reader->wait_set_id.load(std::memory_order_acquire) != 0 ||
                reader->claim_in_progress) {
                clear_reader_claims();
                return AttachResult::AlreadyRegistered;
            }
            reader->claim_in_progress = true;
        }
        const auto attached = attach_callback();
        if (attached != AttachResult::Attached) {
            clear_reader_claims();
            return attached;
        }
        bool closed_during_claim = false;
        std::function<bool()> rollback;
        for (const auto& reader : readers) {
            std::lock_guard lock(reader->callback_mutex);
            reader->claim_in_progress = false;
            reader->callback_cv.notify_all();
            reader->wait_set_id.store(wait_set_id, std::memory_order_release);
            reader->registration_id.store(id, std::memory_order_release);
            reader->wake_callback = [wake] { return wake->notify(); };
            reader->detach_callback = detach_callback;
            if (reader->closing.load(std::memory_order_acquire)) {
                closed_during_claim = true;
                rollback = detach_callback;
            }
        }
        if (closed_during_claim) {
            if (rollback) (void)rollback();
            return AttachResult::Closing;
        }
        return AttachResult::Attached;
    }

    template <typename DetachCallback>
    bool release(std::uint64_t wait_set_id, DetachCallback&& detach_callback) noexcept {
        if (guard) return release_logical_waitable(*guard, wait_set_id);
        if (timer) return release_logical_waitable(*timer, wait_set_id);
        if (graph_event) return release_logical_waitable(*graph_event, wait_set_id);

        for (std::size_t index = 0; index < readers.size(); ++index) {
            const auto& reader = readers[index];
            eprosima::fastdds::dds::StatusCondition* condition = nullptr;
            {
                std::lock_guard lock(reader->callback_mutex);
                if (reader->wait_set_id.load(std::memory_order_acquire) != wait_set_id ||
                    reader->registration_id.load(std::memory_order_acquire) != id) {
                    continue;  // a concurrent close already cleaned this reader
                }
                condition = reader_conditions[index];
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
                    continue;
                }
                reader->wake_callback = {};
                reader->detach_callback = {};
                reader->topology_callback = {};
                reader->quarantined_wait_set.reset();
                reader->registration_id.store(0, std::memory_order_release);
                reader->wait_set_id.store(0, std::memory_order_release);
                reader_conditions[index] = nullptr;
            }
            reader->complete_deferred_delete();
        }
        return true;
    }

    /// Sub-channels ready right now; 0 means "not ready".  Consuming
    /// waitables (GuardCondition) commit their trigger here.
    std::uint32_t ready_mask() noexcept {
        if (guard) {
            if (kind == WaitableKind::Event) {
                return guard->pending.load(std::memory_order_acquire) ? kWaitableReadyBit : 0U;
            }
            return guard->consume_trigger() ? kWaitableReadyBit : 0U;
        }
        if (timer) return timer->logically_ready() ? kWaitableReadyBit : 0U;
        if (graph_event) return graph_event->logically_ready() ? kWaitableReadyBit : 0U;
        std::uint32_t mask = 0;
        for (std::size_t index = 0; index < readers.size(); ++index) {
            if (!readers[index]->is_ready()) continue;
            mask |= reader_detail_bit(kind, index);
        }
        if (logical_detail) mask |= logical_detail();
        return mask;
    }

private:
    /// Map one constituent reader index to its aggregate sub-channel bit.
    static std::uint32_t reader_detail_bit(WaitableKind kind, std::size_t index) noexcept {
        if (kind == WaitableKind::ActionClient) {
            static constexpr std::uint32_t kBits[] = {
                kActionGoalResponseBit, kActionCancelResponseBit, kActionResultResponseBit,
                kActionFeedbackBit, kActionStatusBit};
            return index < 5 ? kBits[index] : kWaitableReadyBit;
        }
        if (kind == WaitableKind::ActionServer) {
            static constexpr std::uint32_t kBits[] = {
                kActionGoalRequestBit, kActionCancelRequestBit, kActionResultRequestBit};
            return index < 3 ? kBits[index] : kWaitableReadyBit;
        }
        return kWaitableReadyBit;
    }

    /// Drop claim markers taken by a claim attempt that did not commit.
    void clear_reader_claims() noexcept {
        for (const auto& reader : readers) {
            std::lock_guard lock(reader->callback_mutex);
            if (!reader->claim_in_progress) continue;
            reader->claim_in_progress = false;
            reader->callback_cv.notify_all();
        }
    }

    /// GuardCondition and Timer share one non-native registration protocol:
    /// both are logical waitables with a clock/trigger wake path and no native
    /// condition of their own.
    template <typename State, typename AttachCallback, typename DetachCallback>
    AttachResult claim_logical_waitable(
        State& state, std::uint64_t wait_set_id, const std::shared_ptr<WaitSetWake>& wake,
        AttachCallback&& attach_callback, DetachCallback&& detach_callback) noexcept {
        std::lock_guard lock(state.callback_mutex);
        if (state.closing.load(std::memory_order_acquire)) return AttachResult::Closing;
        if (state.wait_set_id.load(std::memory_order_acquire) != 0) {
            return AttachResult::AlreadyRegistered;
        }
        const auto attached = attach_callback();
        if (attached != AttachResult::Attached) return attached;
        state.wait_set_id.store(wait_set_id, std::memory_order_release);
        state.registration_id.store(id, std::memory_order_release);
        state.wake_callback = [wake] { return wake->notify(); };
        state.detach_callback = std::forward<DetachCallback>(detach_callback);
        return AttachResult::Attached;
    }

    template <typename State>
    bool release_logical_waitable(State& state, std::uint64_t wait_set_id) noexcept {
        std::lock_guard lock(state.callback_mutex);
        if (state.wait_set_id.load(std::memory_order_acquire) != wait_set_id ||
            state.registration_id.load(std::memory_order_acquire) != id) {
            return true;
        }
        state.wake_callback = {};
        state.detach_callback = {};
        state.registration_id.store(0, std::memory_order_release);
        state.wait_set_id.store(0, std::memory_order_release);
        return true;
    }
};

class WaitSetState final : public std::enable_shared_from_this<WaitSetState> {
public:
    WaitSetState(std::shared_ptr<impl::Context> context, std::uint64_t wait_set_id)
    : context_(std::move(context)), wait_set_id_(wait_set_id) {}

    ~WaitSetState() noexcept { close(); }

    Result<void> initialize_shutdown_callback() noexcept {
        {
            std::lock_guard lock(native_mutex_);
            try {
                const auto result = native_wait_set_.attach_condition(*wake_->control_condition);
                if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
                    return Result<void>::failure(Error(
                        impl::to_error(result, "Fast DDS failed to attach the control condition")));
                }
                control_condition_attached_ = true;
            } catch (...) {
                return Result<void>::failure(
                    Error(ErrorCode::DDSError, "Fast DDS control condition attachment failed"));
            }
        }
        note_topology_mutation();
        const std::weak_ptr<WaitSetState> weak_state = weak_from_this();
        shutdown_callback_id_ = context_->register_shutdown_callback([weak_state] {
            if (const auto context = weak_state.lock()) context->wake_->notify();
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
        std::shared_ptr<GuardConditionState> guard,
        std::vector<std::shared_ptr<impl::ReaderWaitState>> readers,
        std::shared_ptr<impl::TimerState> timer,
        std::shared_ptr<impl::GraphEventState> graph_event, WaitableKind kind,
        std::function<std::uint32_t()> logical_detail = {},
        std::function<std::optional<std::chrono::steady_clock::time_point>()> runtime_deadline =
            {}) {
        std::lock_guard lock(mutex_);
        if (closing_) {
            return Result<std::uint64_t>::failure(
                Error(ErrorCode::ParentDestroyed, "WaitSet is closing"));
        }
        if (poisoned_) {
            return Result<std::uint64_t>::failure(
                Error(ErrorCode::DDSError, "WaitSet topology is poisoned"));
        }
        if (next_registration_id_ == 0) {
            return Result<std::uint64_t>::failure(
                Error(ErrorCode::ResourceExhausted, "WaitSet registration IDs are exhausted"));
        }

        auto registration = std::make_shared<Registration>();
        registration->id = next_registration_id_;
        registration->kind = kind;
        registration->guard = std::move(guard);
        registration->timer = std::move(timer);
        registration->graph_event = std::move(graph_event);
        registration->readers = std::move(readers);
        registration->reader_conditions.assign(registration->readers.size(), nullptr);
        registration->logical_detail = std::move(logical_detail);
        registration->runtime_deadline = std::move(runtime_deadline);

        // Publish a mutation before waking a native wait.  wait_for_notification()
        // will not clear the control condition and re-enter an infinite wait
        // until this mutation has finished reconciling the native WaitSet.
        // This is the handoff that closes the notify/clear/reattach race.
        const TopologyMutationGuard topology_mutation(*this);

        const std::weak_ptr<WaitSetState> weak_state = weak_from_this();
        const std::weak_ptr<Registration> weak_registration = registration;
        const auto attach = registration->claim(
            wait_set_id_, wake_,
            [this, registration] { return attach_reader_conditions(*registration); },
            [weak_state, weak_registration] {
                const auto context = weak_state.lock();
                const auto detached_registration = weak_registration.lock();
                return context && detached_registration && context->detach(detached_registration);
            });
        if (attach == AttachResult::AlreadyRegistered) {
            return Result<std::uint64_t>::failure(
                Error(ErrorCode::AlreadyRegistered, "Waitable is already registered"));
        }
        if (attach == AttachResult::Closing) {
            return Result<std::uint64_t>::failure(
                Error(ErrorCode::ParentDestroyed, "Waitable is closing"));
        }
        if (attach == AttachResult::DDSError) {
            return Result<std::uint64_t>::failure(
                Error(ErrorCode::DDSError, "Fast DDS failed to attach a reader condition"));
        }
        if (registration->phase.load(std::memory_order_acquire) != RegistrationPhase::Attached) {
            registration->release(
                wait_set_id_, [this](eprosima::fastdds::dds::Condition& condition) {
                    return detach_native_condition(condition);
                });
            return Result<std::uint64_t>::failure(
                Error(ErrorCode::ParentDestroyed, "Waitable is closing"));
        }

        for (std::size_t index = 0; index < registration->readers.size(); ++index) {
            std::lock_guard reader_lock(registration->readers[index]->callback_mutex);
            registration->readers[index]->topology_callback =
                [weak_state, weak_registration, index](bool enabled) {
                    const auto context = weak_state.lock();
                    const auto current = weak_registration.lock();
                    if (context && current) context->set_reader_blocking(*current, index, enabled);
                };
        }

        const auto id = next_registration_id_;
        try {
            registrations_.emplace(id, registration);
        } catch (...) {
            // claim() published registration context before this allocation.  A
            // failed map insertion must restore the waitable to the exact
            // pre-add context so a retry is not spuriously AlreadyRegistered.
            registration->release(
                wait_set_id_, [this](eprosima::fastdds::dds::Condition& condition) {
                    return detach_native_condition(condition);
                });
            throw;
        }
        ++next_registration_id_;
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

        // See add(): the mutation guard wakes a blocking native wait before
        // attempting the detach and prevents it from re-entering early.
        const TopologyMutationGuard topology_mutation(*this);

        if (!registration->release(
                wait_set_id_, [this](eprosima::fastdds::dds::Condition& condition) {
                    return detach_native_condition(condition);
                })) {
            for (const auto& reader : registration->readers) {
                reader->quarantine_wait_set(shared_from_this());
            }
            registration->phase.store(RegistrationPhase::Attached, std::memory_order_release);
            {
                std::lock_guard lock(mutex_);
                poisoned_ = true;
            }
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
        return true;
    }

    /// Attach or detach one constituent reader of a registration.  A Server
    /// uses this to drop an unread request reader while its capacity is full.
    void set_reader_blocking(
        Registration& registration, std::size_t index, bool enabled) noexcept {
        if (index >= registration.readers.size() ||
            registration.phase.load(std::memory_order_acquire) != RegistrationPhase::Attached) {
            return;
        }
        const auto& reader = registration.readers[index];
        if (reader->closing.load(std::memory_order_acquire) || reader->reader == nullptr) return;
        // Attaching or detaching a reader StatusCondition reconciles the
        // native WaitSet.  The guard supplies a strict handoff with an
        // infinite native wait.
        const TopologyMutationGuard topology_mutation(*this);
        if (!enabled) {
            auto* condition = registration.reader_conditions[index];
            if (condition != nullptr && !detach_native_condition(*condition)) {
                std::lock_guard lock(mutex_);
                poisoned_ = true;
                return;
            }
            registration.reader_conditions[index] = nullptr;
        } else if (registration.reader_conditions[index] == nullptr) {
            if (attach_single_reader_condition(registration, index) != AttachResult::Attached) {
                std::lock_guard lock(mutex_);
                poisoned_ = true;
                return;
            }
        }
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
        if (shutdown_callback_id != 0) context_->unregister_shutdown_callback(shutdown_callback_id);
        wake_->notify();
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
                    // Keep it alive with the WaitSet context on teardown.
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
                Error(ErrorCode::DDSError, "WaitSet topology is poisoned"));
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

    Result<void> wait_for_notification(
        const eprosima::fastrtps::Duration_t& timeout, std::uint64_t observed_wake_generation) {
        eprosima::fastdds::dds::ConditionSeq active_conditions;
        eprosima::fastrtps::types::ReturnCode_t result;
        {
            // Clearing an already-observed control wake and entering the next
            // native wait must be ordered with topology mutation publication.
            // Otherwise a mutator can notify, block on native_mutex_, and have
            // its wake cleared before it reconciles the native WaitSet.
            std::unique_lock topology_lock(topology_handoff_mutex_);
            topology_handoff_cv_.wait(
                topology_lock, [this] { return active_topology_mutations_ == 0; });
            std::lock_guard lock(native_mutex_);
            if (!wake_->clear_if_unchanged(observed_wake_generation)) {
                return Result<void>::success();
            }
            topology_lock.unlock();
            result = native_wait_set_.wait(active_conditions, timeout);
        }
        if (result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK ||
            result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_TIMEOUT) {
            return Result<void>::success();
        }
        return Result<void>::failure(impl::to_error(result, "Fast DDS WaitSet wait failed"));
    }

    Result<void> wait_for_notification(
        std::chrono::nanoseconds timeout, std::uint64_t observed_wake_generation) {
        constexpr auto kNanosecondsPerSecond = std::chrono::nanoseconds::period::den;
        const auto count = timeout.count();
        const auto seconds = count / kNanosecondsPerSecond;
        const auto nanoseconds = count % kNanosecondsPerSecond;
        const eprosima::fastrtps::Duration_t duration(
            static_cast<std::int32_t>(seconds), static_cast<std::uint32_t>(nanoseconds));
        return wait_for_notification(duration, observed_wake_generation);
    }

    Result<void> wait_for_notification(std::uint64_t observed_wake_generation) {
        return wait_for_notification(eprosima::fastrtps::c_TimeInfinite, observed_wake_generation);
    }

    Result<void> repair_control_guard_if_needed() noexcept {
        if (!wake_->broken.load(std::memory_order_acquire)) return Result<void>::success();
        std::lock_guard lock(native_mutex_);
        if (!wake_->broken.load(std::memory_order_acquire)) return Result<void>::success();
        if (!wake_->replace(native_wait_set_)) {
            poisoned_.store(true, std::memory_order_release);
            return Result<void>::failure(
                Error(ErrorCode::DDSError, "Fast DDS control guard replacement failed"));
        }
        note_topology_mutation();
        return Result<void>::success();
    }

private:
    class TopologyMutationGuard {
    public:
        explicit TopologyMutationGuard(WaitSetState& state) noexcept : state_(state) {
            {
                std::lock_guard lock(state_.topology_handoff_mutex_);
                ++state_.active_topology_mutations_;
            }
            // This notification is intentionally after publication.  A
            // waiter that consumes it must observe the in-flight mutation.
            state_.wake_->notify();
        }

        ~TopologyMutationGuard() noexcept {
            state_.note_topology_mutation();
            {
                std::lock_guard lock(state_.topology_handoff_mutex_);
                --state_.active_topology_mutations_;
            }
            state_.topology_handoff_cv_.notify_all();
            state_.wake_->notify();
        }

        TopologyMutationGuard(const TopologyMutationGuard&) = delete;
        TopologyMutationGuard& operator=(const TopologyMutationGuard&) = delete;

    private:
        WaitSetState& state_;
    };

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

    /// Attach one constituent reader's StatusCondition to the native WaitSet.
    AttachResult attach_single_reader_condition(
        Registration& registration, std::size_t index) noexcept {
        const auto& reader = registration.readers[index];
        if (!reader->blocking_enabled.load(std::memory_order_acquire)) {
            return AttachResult::Attached;
        }
        std::unique_lock<std::mutex> reader_lock(reader->reader_mutex);
        if (reader->closing.load(std::memory_order_acquire) || reader->reader == nullptr) {
            return AttachResult::Closing;
        }
        auto* condition = &reader->reader->get_statuscondition();
        bool attached = false;
        {
            std::lock_guard lock(native_mutex_);
            try {
                attached =
                    condition->set_enabled_statuses(
                        eprosima::fastdds::dds::StatusMask::data_available()) ==
                        eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK &&
                    native_wait_set_.attach_condition(*condition) ==
                        eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
            } catch (...) {
                attached = false;
            }
        }
        reader_lock.unlock();
        if (!attached) return AttachResult::DDSError;
        registration.reader_conditions[index] = condition;
        return AttachResult::Attached;
    }

    /// Attach every constituent reader that currently blocks on data.
    AttachResult attach_reader_conditions(Registration& registration) noexcept {
        AttachResult outcome = AttachResult::Attached;
        for (std::size_t index = 0; index < registration.readers.size(); ++index) {
            const auto result = attach_single_reader_condition(registration, index);
            if (result == AttachResult::Attached) continue;
            outcome = result;
            break;
        }
        if (outcome != AttachResult::Attached) detach_attached_reader_conditions(registration);
        return outcome;
    }

    /// Roll back partially attached conditions without touching waitable state.
    void detach_attached_reader_conditions(Registration& registration) noexcept {
        for (auto& condition : registration.reader_conditions) {
            if (condition == nullptr) continue;
            (void)detach_native_condition(*condition);
            condition = nullptr;
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

    std::shared_ptr<impl::Context> context_;
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
    std::mutex topology_handoff_mutex_;
    std::condition_variable topology_handoff_cv_;
    std::size_t active_topology_mutations_{0};
    std::unordered_map<std::uint64_t, std::shared_ptr<Registration>> registrations_;
};

class WaitActivityGuard {
public:
    explicit WaitActivityGuard(std::shared_ptr<WaitSetState> context) noexcept
    : context_(std::move(context)) {}
    ~WaitActivityGuard() noexcept { context_->end_wait(); }

    WaitActivityGuard(const WaitActivityGuard&) = delete;
    WaitActivityGuard& operator=(const WaitActivityGuard&) = delete;

private:
    std::shared_ptr<WaitSetState> context_;
};

inline Result<std::uint64_t> add_guard(
    const std::shared_ptr<WaitSetState>& context, const std::shared_ptr<GuardConditionState>& guard,
    WaitableKind kind) {
    if (guard->context() != context->context_) {
        return Result<std::uint64_t>::failure(
            Error(ErrorCode::InvalidArgument, "Waitable belongs to another Context"));
    }
    const auto operation = context->context_->try_acquire_operation();
    if (!operation) {
        return Result<std::uint64_t>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    auto registration = context->add(guard, {}, nullptr, nullptr, kind);
    if (!registration) return Result<std::uint64_t>::failure(std::move(registration.error()));
    return registration;
}

inline Result<std::uint64_t> add_timer(
    const std::shared_ptr<WaitSetState>& context,
    const std::shared_ptr<impl::TimerState>& timer) {
    if (timer->context() != context->context_) {
        return Result<std::uint64_t>::failure(
            Error(ErrorCode::InvalidArgument, "Waitable belongs to another Context"));
    }
    const auto operation = context->context_->try_acquire_operation();
    if (!operation) {
        return Result<std::uint64_t>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    auto registration = context->add(nullptr, {}, timer, nullptr, WaitableKind::Timer);
    if (!registration) return Result<std::uint64_t>::failure(std::move(registration.error()));
    return registration;
}

inline Result<std::uint64_t> add_graph_event(
    const std::shared_ptr<WaitSetState>& context,
    const std::shared_ptr<impl::GraphEventState>& graph_event) {
    if (graph_event->context() != context->context_) {
        return Result<std::uint64_t>::failure(
            Error(ErrorCode::InvalidArgument, "Waitable belongs to another Context"));
    }
    const auto operation = context->context_->try_acquire_operation();
    if (!operation) {
        return Result<std::uint64_t>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    auto registration = context->add(nullptr, {}, nullptr, graph_event, WaitableKind::GraphEvent);
    if (!registration) return Result<std::uint64_t>::failure(std::move(registration.error()));
    return registration;
}

/// Register one composite token backed by one or more reader channels.
inline Result<std::uint64_t> add_readers(
    const std::shared_ptr<WaitSetState>& context,
    std::vector<std::shared_ptr<impl::ReaderWaitState>> readers, WaitableKind kind,
    std::function<std::uint32_t()> logical_detail = {},
    std::function<std::optional<std::chrono::steady_clock::time_point>()> runtime_deadline = {}) {
    if (readers.empty()) {
        return Result<std::uint64_t>::failure(
            Error(ErrorCode::InvalidArgument, "Waitable has no readable channel"));
    }
    for (const auto& reader : readers) {
        if (reader->context() != context->context_) {
            return Result<std::uint64_t>::failure(
                Error(ErrorCode::InvalidArgument, "Waitable belongs to another Context"));
        }
    }
    const auto operation = context->context_->try_acquire_operation();
    if (!operation) {
        return Result<std::uint64_t>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    auto registration =
        context->add(
            nullptr, std::move(readers), nullptr, nullptr, kind, std::move(logical_detail),
            std::move(runtime_deadline));
    if (!registration) return Result<std::uint64_t>::failure(std::move(registration.error()));
    return registration;
}

inline Result<std::uint64_t> add_reader(
    const std::shared_ptr<WaitSetState>& context,
    const std::shared_ptr<impl::ReaderWaitState>& reader, WaitableKind kind) {
    return add_readers(context, {reader}, kind);
}

}  // namespace impl

class WaitSet::Impl {
public:
    explicit Impl(std::shared_ptr<impl::WaitSetState> context) noexcept
    : context_(std::move(context)) {}

    ~Impl() noexcept { context_->close(); }
    Result<WaitableRegistration> add(const std::shared_ptr<GuardConditionState>&, WaitableKind);
    Result<WaitableRegistration> add(const std::shared_ptr<impl::ReaderWaitState>&, WaitableKind);
    Result<WaitableRegistration> add(const std::shared_ptr<impl::TimerState>&, WaitableKind);
    Result<WaitableRegistration> add(const std::shared_ptr<impl::GraphEventState>&, WaitableKind);
    /// Register one Action aggregate token backed by several reader channels.
    Result<WaitableRegistration> add_composite(
        const std::vector<std::shared_ptr<impl::ReaderWaitState>>&, WaitableKind,
        std::function<std::uint32_t()> logical_detail = {},
        std::function<std::optional<std::chrono::steady_clock::time_point>()> runtime_deadline =
            {});
    Result<void> remove(WaitableRegistration registration);
    Result<WaitResult> wait(WaitTimeout);

private:
    std::shared_ptr<impl::WaitSetState> context_;
};

}  // namespace dmw

#endif  // DMW_IMPL__WAIT_SET_IMPL_HPP_
