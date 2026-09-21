#include <cassert>

#include "dmw/clock.hpp"
#include "dmw/context.hpp"

int main() {
    auto context = dmw::Context::create({});
    assert(context);
    auto steady = context.value()->create_clock(dmw::ClockType::Steady);
    auto ros = context.value()->create_clock(dmw::ClockType::Ros);
    assert(steady && ros);
    assert(steady.value()->now());
    assert(!steady.value()->enable_ros_time_override(true));
    assert(steady.value()->enable_ros_time_override(true).error().code() == dmw::ErrorCode::InvalidState);
    assert(ros.value()->enable_ros_time_override(true));
    assert(ros.value()->set_ros_time({42, dmw::ClockType::Ros}));
    const auto now = ros.value()->now();
    assert(now && now.value().nanoseconds == 42);
    assert(!ros.value()->set_ros_time({42, dmw::ClockType::System}));
    return 0;
}
