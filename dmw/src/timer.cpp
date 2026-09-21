#include "dmw/timer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

#include "dmw/error.hpp"
#include "impl/timer_impl.hpp"

namespace dmw {

namespace impl {

namespace {

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();

/// Saturation keeps every public scheduling quantity overflow-safe.
std::int64_t saturating_add(std::int64_t lhs, std::int64_t rhs) noexcept {
    if (rhs > 0 && lhs > kInt64Max - rhs) return kInt64Max;
    if (rhs < 0 && lhs < kInt64Min - rhs) return kInt64Min;
    return lhs + rhs;
}

std::int64_t saturating_sub(std::int64_t lhs, std::int64_t rhs) noexcept {
    if (rhs > 0 && lhs < kInt64Min + rhs) return kInt64Min;
    if (rhs < 0 && lhs > kInt64Max + rhs) return kInt64Max;
    return lhs - rhs;
}

/// Only ever called with non-negative operands.
std::int64_t saturating_mul(std::int64_t lhs, std::int64_t rhs) noexcept {
    if (lhs == 0 || rhs == 0) return 0;
    if (lhs > kInt64Max / rhs) return kInt64Max;
    return lhs * rhs;
}

/// First period-grid point strictly later than `now`, skipping missed cycles.
std::int64_t next_period_grid_point(
    std::int64_t expected, std::int64_t period, std::int64_t now) noexcept {
    auto next = saturating_add(expected, period);
    if (next > now) return next;
    const auto behind = saturating_sub(now, next);
    const auto missed = saturating_add(1, behind / period);
    next = saturating_add(next, saturating_mul(missed, period));
    return next > now ? next : now;
}

}  // namespace

TimerState::TimerState(
    std::shared_ptr<impl::Context> context, std::shared_ptr<ClockState> clock,
    std::chrono::nanoseconds period, bool autostart) noexcept
: context_(std::move(context)),
  clock_(std::move(clock)),
  period_ns_(period.count()),
  canceled_(!autostart) {
    const auto now = clock_->current_nanoseconds();
    last_call_ns_ = now;
    next_call_ns_ = saturating_add(now, period.count());
}

TimerState::~TimerState() noexcept {
    if (clock_ && clock_wake_handle_ != 0) {
        clock_->unregister_wake_target(clock_wake_handle_);
        clock_wake_handle_ = 0;
    }
}

void TimerState::initialize_clock_wake_target() {
    const std::weak_ptr<TimerState> weak = weak_from_this();
    clock_wake_handle_ = clock_->register_wake_target([weak] {
        if (const auto state = weak.lock()) state->notify_wait_set_noexcept();
    });
}

Result<void> TimerState::notify_wait_set() {
    std::lock_guard lock(callback_mutex);
    if (wake_callback) return wake_callback();
    return Result<void>::success();
}

void TimerState::notify_wait_set_noexcept() noexcept {
    try {
        (void)notify_wait_set();
    } catch (...) {
        // A failed wake must not escape a clock update or a destructor.
    }
}

void TimerState::detach_wait_set() noexcept {
    std::function<void()> callback;
    {
        std::lock_guard lock(callback_mutex);
        callback = detach_callback;
    }
    if (callback) callback();
}

void TimerState::close() noexcept {
    closing.store(true, std::memory_order_release);
    detach_wait_set();
    notify_wait_set_noexcept();
}

Result<bool> TimerState::is_canceled() const {
    if (closing.load(std::memory_order_acquire)) {
        return Result<bool>::failure(Error(ErrorCode::ParentDestroyed, "Timer is closing"));
    }
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    std::lock_guard lock(state_mutex_);
    return Result<bool>::success(canceled_);
}

Result<bool> TimerState::is_ready() const {
    if (closing.load(std::memory_order_acquire)) {
        return Result<bool>::failure(Error(ErrorCode::ParentDestroyed, "Timer is closing"));
    }
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    std::lock_guard lock(state_mutex_);
    if (canceled_) return Result<bool>::success(false);
    return Result<bool>::success(clock_->current_nanoseconds() >= next_call_ns_);
}

bool TimerState::logically_ready() const noexcept {
    if (closing.load(std::memory_order_acquire) || context_->is_shutdown()) return false;
    std::lock_guard lock(state_mutex_);
    if (canceled_) return false;
    return clock_->current_nanoseconds() >= next_call_ns_;
}

Result<void> TimerState::cancel() {
    if (closing.load(std::memory_order_acquire)) {
        return Result<void>::failure(Error(ErrorCode::ParentDestroyed, "Timer is closing"));
    }
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    {
        std::lock_guard lock(state_mutex_);
        if (canceled_) return Result<void>::success();
        canceled_ = true;
    }
    // Registered WaitSets must recompute their earliest deadline.
    return notify_wait_set();
}

Result<void> TimerState::reset() {
    if (closing.load(std::memory_order_acquire)) {
        return Result<void>::failure(Error(ErrorCode::ParentDestroyed, "Timer is closing"));
    }
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    {
        std::lock_guard lock(state_mutex_);
        const auto now = clock_->current_nanoseconds();
        canceled_ = false;
        last_call_ns_ = now;
        next_call_ns_ = saturating_add(now, period_ns_.load(std::memory_order_acquire));
    }
    return notify_wait_set();
}

Result<std::chrono::nanoseconds> TimerState::exchange_period(
    std::chrono::nanoseconds new_period) {
    if (new_period < std::chrono::nanoseconds::zero()) {
        return Result<std::chrono::nanoseconds>::failure(
            Error(ErrorCode::InvalidArgument, "Timer period must not be negative"));
    }
    if (closing.load(std::memory_order_acquire)) {
        return Result<std::chrono::nanoseconds>::failure(
            Error(ErrorCode::ParentDestroyed, "Timer is closing"));
    }
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<std::chrono::nanoseconds>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    // The next call point is intentionally left untouched: the new period is
    // applied by the next successful consume(), or by an explicit reset().
    const auto previous = period_ns_.exchange(new_period.count(), std::memory_order_acq_rel);
    auto notified = notify_wait_set();
    if (!notified)
        return Result<std::chrono::nanoseconds>::failure(std::move(notified.error()));
    return Result<std::chrono::nanoseconds>::success(std::chrono::nanoseconds(previous));
}

Result<std::chrono::nanoseconds> TimerState::time_until_next_call() const {
    if (closing.load(std::memory_order_acquire)) {
        return Result<std::chrono::nanoseconds>::failure(
            Error(ErrorCode::ParentDestroyed, "Timer is closing"));
    }
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<std::chrono::nanoseconds>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    std::lock_guard lock(state_mutex_);
    if (canceled_) {
        return Result<std::chrono::nanoseconds>::failure(
            Error(ErrorCode::InvalidState, "Timer is canceled"));
    }
    const auto remaining = saturating_sub(next_call_ns_, clock_->current_nanoseconds());
    return Result<std::chrono::nanoseconds>::success(
        std::chrono::nanoseconds(std::max<std::int64_t>(remaining, 0)));
}

Result<bool> TimerState::consume(TimerInfo& info) {
    if (closing.load(std::memory_order_acquire)) {
        return Result<bool>::failure(Error(ErrorCode::ParentDestroyed, "Timer is closing"));
    }
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    TimerInfo committed;
    {
        std::lock_guard lock(state_mutex_);
        const auto now = clock_->current_nanoseconds();
        if (canceled_ || now < next_call_ns_) {
            // success + false leaves the caller output untouched.
            return Result<bool>::success(false);
        }
        const auto expected = next_call_ns_;
        const auto elapsed = saturating_sub(now, last_call_ns_);
        last_call_ns_ = now;
        const auto period = period_ns_.load(std::memory_order_acquire);
        next_call_ns_ = period > 0 ? next_period_grid_point(expected, period, now) : now;

        committed.expected_call_time = TimePoint{expected, clock_->type()};
        committed.actual_call_time = TimePoint{now, clock_->type()};
        committed.elapsed_since_last_call = std::chrono::nanoseconds(elapsed);
    }
    info = committed;
    return Result<bool>::success(true);
}

std::optional<std::chrono::steady_clock::time_point> TimerState::steady_deadline() const noexcept {
    if (closing.load(std::memory_order_acquire) || context_->is_shutdown()) return std::nullopt;
    std::lock_guard lock(state_mutex_);
    if (canceled_) return std::nullopt;
    // A ROS clock under override can pause or jump; only a clock update may
    // advance such a timer's readiness.
    if (clock_->ros_override_active()) return std::nullopt;

    const auto steady_now = std::chrono::steady_clock::now();
    const auto now_ns = clock_->current_nanoseconds();
    if (now_ns >= next_call_ns_) return steady_now;
    const auto remaining = std::chrono::nanoseconds(saturating_sub(next_call_ns_, now_ns));
    // Clamping is safe: an early wake only re-evaluates logical readiness.
    constexpr auto kMaxNativeWait =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::hours(24 * 365 * 10));
    return steady_now + std::min(remaining, kMaxNativeWait);
}

}  // namespace impl

Timer::Impl::Impl(
    std::shared_ptr<impl::Context> context, std::shared_ptr<impl::ClockState> clock,
    std::chrono::nanoseconds period, bool autostart) {
    state_ = std::make_shared<impl::TimerState>(
        std::move(context), std::move(clock), period, autostart);
    state_->initialize_clock_wake_target();
}

Timer::Timer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Timer::~Timer() noexcept = default;

std::chrono::nanoseconds Timer::period() const noexcept { return impl_->period(); }
ClockType Timer::clock_type() const noexcept { return impl_->clock_type(); }
Result<bool> Timer::is_canceled() const { return impl_->is_canceled(); }
Result<bool> Timer::is_ready() const { return impl_->is_ready(); }
Result<void> Timer::cancel() { return impl_->cancel(); }
Result<void> Timer::reset() { return impl_->reset(); }
Result<std::chrono::nanoseconds> Timer::exchange_period(std::chrono::nanoseconds new_period) {
    return impl_->exchange_period(new_period);
}
Result<std::chrono::nanoseconds> Timer::time_until_next_call() const {
    return impl_->time_until_next_call();
}
Result<bool> Timer::consume(TimerInfo& info) { return impl_->consume(info); }

}  // namespace dmw
