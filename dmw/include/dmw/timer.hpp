#ifndef DMW_TIMER_HPP_
#define DMW_TIMER_HPP_

#include <chrono>
#include <memory>
#include <utility>

#include "dmw/clock.hpp"
#include "dmw/result.hpp"
#include "dmw/visibility_control.hpp"

namespace dmw {

class Context;
class WaitSet;

/// Configuration for one Clock-bound Timer.
struct TimerOptions {
    std::chrono::nanoseconds period{0};
    bool autostart{true};
};

/// Scheduling snapshot committed by one successful Timer::consume().
struct TimerInfo {
    TimePoint expected_call_time;
    TimePoint actual_call_time;
    std::chrono::nanoseconds elapsed_since_last_call{0};
};

/// Clock-bound scheduling primitive.  Owns no DDS entity and no worker thread.
class DMW_PUBLIC Timer {
public:
    ~Timer() noexcept;

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&&) = delete;
    Timer& operator=(Timer&&) = delete;

    std::chrono::nanoseconds period() const noexcept;
    ClockType clock_type() const noexcept;

    Result<bool> is_canceled() const;
    Result<bool> is_ready() const;

    Result<void> cancel();
    Result<void> reset();

    Result<std::chrono::nanoseconds> exchange_period(std::chrono::nanoseconds new_period);

    Result<std::chrono::nanoseconds> time_until_next_call() const;

    /// Consume one ready deadline and commit its TimerInfo.
    Result<bool> consume(TimerInfo& info);

private:
    friend class Context;
    friend class WaitSet;

    class Impl;

    explicit Timer(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace dmw

#endif  // DMW_TIMER_HPP_
