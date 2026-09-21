#ifndef DMW_IMPL__TIMER_IMPL_HPP_
#define DMW_IMPL__TIMER_IMPL_HPP_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "dmw/timer.hpp"
#include "impl/clock_impl.hpp"
#include "impl/context.hpp"
#include "impl/lock_rank.hpp"

namespace dmw {

namespace impl {

struct Registration;

/// Clock-bound scheduling state shared by the Timer facade and the WaitSet.
///
/// The state owns no DDS entity, creates no thread, and never executes a user
/// callback.  Readiness is level-triggered: only a successful consume() commits
/// schedule progress.
class TimerState : public std::enable_shared_from_this<TimerState> {
public:
    TimerState(
        std::shared_ptr<impl::Context> context, std::shared_ptr<ClockState> clock,
        std::chrono::nanoseconds period, bool autostart) noexcept;
    ~TimerState() noexcept;

    /// Register the clock-update wake relationship after construction.
    void initialize_clock_wake_target();

    const std::shared_ptr<impl::Context>& context() const noexcept { return context_; }
    const std::shared_ptr<ClockState>& clock() const noexcept { return clock_; }

    std::chrono::nanoseconds period() const noexcept {
        return std::chrono::nanoseconds(period_ns_.load(std::memory_order_acquire));
    }

    ClockType clock_type() const noexcept { return clock_->type(); }

    Result<bool> is_canceled() const;
    Result<bool> is_ready() const;
    Result<void> cancel();
    Result<void> reset();
    Result<std::chrono::nanoseconds> exchange_period(std::chrono::nanoseconds new_period);
    Result<std::chrono::nanoseconds> time_until_next_call() const;
    Result<bool> consume(TimerInfo& info);

    /// Level-triggered readiness without consuming any schedule progress.
    bool logically_ready() const noexcept;

    /// Earliest absolute steady deadline at which this timer can become ready.
    ///
    /// Returns nullopt when the timer is canceled, already closed, or driven by
    /// a ROS clock with override enabled: such a timer must be re-evaluated from
    /// a clock update, never from elapsed steady time.
    std::optional<std::chrono::steady_clock::time_point> steady_deadline() const noexcept;

    Result<void> notify_wait_set();

    void detach_wait_set() noexcept;

    void close() noexcept;

    std::atomic<bool> closing{false};
    std::atomic<std::uint64_t> wait_set_id{0};
    std::atomic<std::uint64_t> registration_id{0};
    impl::RankedMutex<impl::LockRank::WaitableLocal> callback_mutex;
    std::function<Result<void>()> wake_callback;
    std::function<void()> detach_callback;

private:
    friend struct impl::Registration;

    std::int64_t clock_now_nanoseconds_locked() const noexcept;
    void notify_wait_set_noexcept() noexcept;

    std::shared_ptr<impl::Context> context_;
    std::shared_ptr<ClockState> clock_;
    std::atomic<std::int64_t> period_ns_{0};
    ClockState::WakeHandle clock_wake_handle_{0};
    mutable std::mutex state_mutex_;
    bool canceled_{false};
    std::int64_t last_call_ns_{0};
    std::int64_t next_call_ns_{0};
};

}  // namespace impl

class Timer::Impl {
public:
    Impl(
        std::shared_ptr<impl::Context> context, std::shared_ptr<impl::ClockState> clock,
        std::chrono::nanoseconds period, bool autostart);

    ~Impl() noexcept { state_->close(); }

    std::chrono::nanoseconds period() const noexcept { return state_->period(); }
    ClockType clock_type() const noexcept { return state_->clock_type(); }
    Result<bool> is_canceled() const { return state_->is_canceled(); }
    Result<bool> is_ready() const { return state_->is_ready(); }
    Result<void> cancel() { return state_->cancel(); }
    Result<void> reset() { return state_->reset(); }
    Result<std::chrono::nanoseconds> exchange_period(std::chrono::nanoseconds new_period) {
        return state_->exchange_period(new_period);
    }
    Result<std::chrono::nanoseconds> time_until_next_call() const {
        return state_->time_until_next_call();
    }
    Result<bool> consume(TimerInfo& info) { return state_->consume(info); }

    const std::shared_ptr<impl::TimerState>& wait_state() const noexcept { return state_; }

private:
    std::shared_ptr<impl::TimerState> state_;
};

}  // namespace dmw

#endif  // DMW_IMPL__TIMER_IMPL_HPP_
