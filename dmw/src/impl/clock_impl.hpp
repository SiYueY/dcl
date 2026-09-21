#ifndef DMW_IMPL__CLOCK_IMPL_HPP_
#define DMW_IMPL__CLOCK_IMPL_HPP_

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dmw/clock.hpp"
#include "impl/context.hpp"

namespace dmw {

namespace impl {

/// Language-neutral clock runtime shared by every Clock facade of one Context.
///
/// The state owns no public Clock/Timer object.  Dependents (Timer, WaitSet)
/// register a wake callback and are notified outside the clock lock whenever a
/// committed clock update changes logical readiness.
class ClockState {
public:
    using WakeHandle = std::uint64_t;

    ClockState(std::shared_ptr<impl::Context> context, ClockType type) noexcept
    : context_(std::move(context)), type_(type) {}

    ClockType type() const noexcept { return type_; }

    const std::shared_ptr<impl::Context>& context() const noexcept { return context_; }

    Result<TimePoint> now() const;

    /// Current clock value in nanoseconds for this clock's own time domain.
    ///
    /// Unlike now() this performs no Context operation-guard check, so it is
    /// safe for readiness evaluation inside a WaitSet loop.
    std::int64_t current_nanoseconds() const noexcept;

    /// True when this is a ROS clock whose value is driven by override only.
    bool ros_override_active() const noexcept;

    Result<bool> ros_time_override_enabled() const;

    Result<void> enable_ros_time_override(bool enabled);

    Result<void> set_ros_time(TimePoint time);

    /// Monotonic counter advanced by every committed ROS-time update.
    std::uint64_t generation() const noexcept {
        std::lock_guard lock(mutex_);
        return generation_;
    }

    /// Register a wake target invoked after each committed clock update.
    WakeHandle register_wake_target(std::function<void()> callback);

    void unregister_wake_target(WakeHandle handle) noexcept;

private:
    std::vector<std::function<void()>> collect_wake_targets_locked() const;

    std::shared_ptr<impl::Context> context_;
    const ClockType type_;
    mutable std::mutex mutex_;
    bool ros_override_{false};
    std::int64_t ros_time_{0};
    std::uint64_t generation_{0};
    std::uint64_t next_wake_handle_{1};
    std::unordered_map<WakeHandle, std::function<void()>> wake_targets_;
};

}  // namespace impl

class Clock::Impl {
public:
    Impl(std::shared_ptr<impl::Context> context, ClockType type)
    : state_(std::make_shared<impl::ClockState>(std::move(context), type)) {}

    ClockType type() const noexcept { return state_->type(); }
    Result<TimePoint> now() const { return state_->now(); }
    Result<bool> ros_time_override_enabled() const {
        return state_->ros_time_override_enabled();
    }
    Result<void> enable_ros_time_override(bool enabled) {
        return state_->enable_ros_time_override(enabled);
    }
    Result<void> set_ros_time(TimePoint time) { return state_->set_ros_time(time); }

    const std::shared_ptr<impl::ClockState>& state() const noexcept { return state_; }

private:
    std::shared_ptr<impl::ClockState> state_;
};

}  // namespace dmw

#endif  // DMW_IMPL__CLOCK_IMPL_HPP_
