#ifndef DMW_CLOCK_HPP_
#define DMW_CLOCK_HPP_

#include <cstdint>
#include <memory>

#include "dmw/result.hpp"
#include "dmw/visibility_control.hpp"

namespace dmw {

enum class ClockType { System, Steady, Ros };

struct TimePoint {
    std::int64_t nanoseconds{0};
    ClockType clock_type{ClockType::System};
};

class DMW_PUBLIC Clock {
public:
    ~Clock() noexcept;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;

    ClockType type() const noexcept;
    Result<TimePoint> now() const;
    Result<bool> ros_time_override_enabled() const;
    Result<void> enable_ros_time_override(bool enabled);
    Result<void> set_ros_time(TimePoint time);

private:
    friend class Context;
    class Impl;
    explicit Clock(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dmw

#endif  // DMW_CLOCK_HPP_
