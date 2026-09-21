#include "dmw/clock.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "dmw/error.hpp"
#include "impl/clock_impl.hpp"

namespace dmw {

namespace impl {

namespace {

std::int64_t system_nanoseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t steady_nanoseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

Result<TimePoint> ClockState::now() const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<TimePoint>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return Result<TimePoint>::success({current_nanoseconds(), type_});
}

std::int64_t ClockState::current_nanoseconds() const noexcept {
    std::lock_guard lock(mutex_);
    if (type_ == ClockType::Ros && ros_override_) {
        return ros_time_;
    }
    if (type_ == ClockType::Steady) {
        return steady_nanoseconds();
    }
    return system_nanoseconds();
}

bool ClockState::ros_override_active() const noexcept {
    std::lock_guard lock(mutex_);
    return type_ == ClockType::Ros && ros_override_;
}

Result<bool> ClockState::ros_time_override_enabled() const {
    if (type_ != ClockType::Ros) {
        return Result<bool>::failure(Error(ErrorCode::InvalidState, "Clock is not a ROS clock"));
    }
    std::lock_guard lock(mutex_);
    return Result<bool>::success(ros_override_);
}

Result<void> ClockState::enable_ros_time_override(bool enabled) {
    if (type_ != ClockType::Ros) {
        return Result<void>::failure(Error(ErrorCode::InvalidState, "Clock is not a ROS clock"));
    }
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    std::vector<std::function<void()>> targets;
    {
        std::lock_guard lock(mutex_);
        if (ros_override_ == enabled) return Result<void>::success();
        ros_override_ = enabled;
        ++generation_;
        targets = collect_wake_targets_locked();
    }
    for (auto& target : targets) target();
    return Result<void>::success();
}

Result<void> ClockState::set_ros_time(TimePoint time) {
    if (type_ != ClockType::Ros || time.clock_type != ClockType::Ros) {
        return Result<void>::failure(
            Error(ErrorCode::InvalidArgument, "ROS time requires a ROS clock"));
    }
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    std::vector<std::function<void()>> targets;
    {
        std::lock_guard lock(mutex_);
        ros_time_ = time.nanoseconds;
        ++generation_;
        // A committed update can pause, resume, or jump ROS time, so
        // dependents must re-evaluate logical readiness.  Notify even when the
        // value is unchanged: the caller signalled an update.
        if (ros_override_) targets = collect_wake_targets_locked();
    }
    for (auto& target : targets) target();
    return Result<void>::success();
}

ClockState::WakeHandle ClockState::register_wake_target(std::function<void()> callback) {
    std::lock_guard lock(mutex_);
    const auto handle = next_wake_handle_++;
    wake_targets_.emplace(handle, std::move(callback));
    return handle;
}

void ClockState::unregister_wake_target(WakeHandle handle) noexcept {
    std::lock_guard lock(mutex_);
    wake_targets_.erase(handle);
}

std::vector<std::function<void()>> ClockState::collect_wake_targets_locked() const {
    std::vector<std::function<void()>> targets;
    targets.reserve(wake_targets_.size());
    for (const auto& entry : wake_targets_) {
        if (entry.second) targets.push_back(entry.second);
    }
    return targets;
}

}  // namespace impl

Clock::Clock(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Clock::~Clock() noexcept = default;
ClockType Clock::type() const noexcept { return impl_->type(); }
Result<TimePoint> Clock::now() const { return impl_->now(); }
Result<bool> Clock::ros_time_override_enabled() const {
    return impl_->ros_time_override_enabled();
}
Result<void> Clock::enable_ros_time_override(bool enabled) {
    return impl_->enable_ros_time_override(enabled);
}
Result<void> Clock::set_ros_time(TimePoint time) { return impl_->set_ros_time(time); }

}  // namespace dmw
