#ifndef DMW_QOS_HPP_
#define DMW_QOS_HPP_

#include <chrono>
#include <cstddef>
#include <exception>

#include "dmw/error.hpp"
#include "dmw/result.hpp"

namespace dmw {

enum class HistoryPolicy { SystemDefault, KeepLast, KeepAll };

enum class ReliabilityPolicy { SystemDefault, Reliable, BestEffort };

enum class DurabilityPolicy { SystemDefault, Volatile, TransientLocal };

enum class LivelinessPolicy { SystemDefault, Automatic, ManualByTopic };

/// Duration value used by QoS policies without magic sentinel values.
class QosDuration {
public:
    enum class Kind { SystemDefault, Infinite, Finite };

    static QosDuration system_default() noexcept {
        return QosDuration(Kind::SystemDefault, std::chrono::nanoseconds::zero());
    }

    static QosDuration infinite() noexcept {
        return QosDuration(Kind::Infinite, std::chrono::nanoseconds::zero());
    }

    static Result<QosDuration> finite(std::chrono::nanoseconds value) {
        if (value < std::chrono::nanoseconds::zero()) {
            return Result<QosDuration>::failure(
                Error(ErrorCode::InvalidArgument, "QoS duration must not be negative"));
        }
        return Result<QosDuration>::success(QosDuration(Kind::Finite, value));
    }

    Kind kind() const noexcept { return kind_; }

    std::chrono::nanoseconds value() const noexcept {
        if (kind_ != Kind::Finite) {
            std::terminate();
        }
        return value_;
    }

private:
    QosDuration(Kind kind, std::chrono::nanoseconds value) noexcept : kind_(kind), value_(value) {}

    Kind kind_;
    std::chrono::nanoseconds value_;
};

/// Middleware-neutral QoS descriptor.
class Qos {
public:
    Qos() = default;

    static Qos system_default() { return Qos(); }

    static Qos ros2_default() {
        Qos qos;
        qos.history_ = HistoryPolicy::KeepLast;
        qos.depth_ = 10;
        qos.reliability_ = ReliabilityPolicy::Reliable;
        qos.durability_ = DurabilityPolicy::Volatile;
        return qos;
    }

    static Qos ros2_services_default() { return ros2_default(); }

    Result<void> keep_last(std::size_t depth) {
        if (depth == 0) {
            return Result<void>::failure(
                Error(ErrorCode::InvalidArgument, "KeepLast depth must be greater than zero"));
        }
        history_ = HistoryPolicy::KeepLast;
        depth_ = depth;
        return Result<void>::success();
    }

    Qos& keep_all() noexcept {
        history_ = HistoryPolicy::KeepAll;
        depth_ = 0;
        return *this;
    }

    Qos& history_system_default() noexcept {
        history_ = HistoryPolicy::SystemDefault;
        depth_ = 0;
        return *this;
    }

    Qos& reliable() noexcept {
        reliability_ = ReliabilityPolicy::Reliable;
        return *this;
    }

    Qos& best_effort() noexcept {
        reliability_ = ReliabilityPolicy::BestEffort;
        return *this;
    }

    Qos& transient_local() noexcept {
        durability_ = DurabilityPolicy::TransientLocal;
        return *this;
    }

    Qos& volatile_() noexcept {
        durability_ = DurabilityPolicy::Volatile;
        return *this;
    }

    Qos& reliability_system_default() noexcept {
        reliability_ = ReliabilityPolicy::SystemDefault;
        return *this;
    }

    Qos& durability_system_default() noexcept {
        durability_ = DurabilityPolicy::SystemDefault;
        return *this;
    }

    Qos& deadline(QosDuration value) noexcept {
        deadline_ = value;
        return *this;
    }

    Qos& lifespan(QosDuration value) noexcept {
        lifespan_ = value;
        return *this;
    }

    Qos& liveliness(LivelinessPolicy value) noexcept {
        liveliness_ = value;
        return *this;
    }

    Qos& liveliness_lease_duration(QosDuration value) noexcept {
        liveliness_lease_duration_ = value;
        return *this;
    }

    HistoryPolicy history() const noexcept { return history_; }

    std::size_t depth() const noexcept { return depth_; }

    ReliabilityPolicy reliability() const noexcept { return reliability_; }

    DurabilityPolicy durability() const noexcept { return durability_; }

    QosDuration deadline() const noexcept { return deadline_; }

    QosDuration lifespan() const noexcept { return lifespan_; }

    LivelinessPolicy liveliness() const noexcept { return liveliness_; }

    QosDuration liveliness_lease_duration() const noexcept { return liveliness_lease_duration_; }

private:
    HistoryPolicy history_{HistoryPolicy::SystemDefault};
    std::size_t depth_{0};
    ReliabilityPolicy reliability_{ReliabilityPolicy::SystemDefault};
    DurabilityPolicy durability_{DurabilityPolicy::SystemDefault};
    QosDuration deadline_{QosDuration::system_default()};
    QosDuration lifespan_{QosDuration::system_default()};
    LivelinessPolicy liveliness_{LivelinessPolicy::SystemDefault};
    QosDuration liveliness_lease_duration_{QosDuration::system_default()};
};

}  // namespace dmw

#endif  // DMW_QOS_HPP_
