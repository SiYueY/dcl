#ifndef DMW_IMPL__FASTDDS__QOS_HPP_
#define DMW_IMPL__FASTDDS__QOS_HPP_

#include <chrono>
#include <cstdint>
#include <limits>

#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/rtps/attributes/HistoryAttributes.h>
#include <fastdds/rtps/common/Time_t.h>

#include "dmw/qos.hpp"
#include "dmw/runtime_mode.hpp"

namespace dmw {

namespace impl {

inline Result<eprosima::fastrtps::Duration_t> to_duration(QosDuration duration) {
    using eprosima::fastrtps::Duration_t;

    if (duration.kind() == QosDuration::Kind::SystemDefault) {
        return Result<Duration_t>::failure(Error(
            ErrorCode::InvalidState, "SystemDefault duration has no concrete Fast DDS value"));
    }
    if (duration.kind() == QosDuration::Kind::Infinite) {
        return Result<Duration_t>::success(
            Duration_t(TIME_T_INFINITE_SECONDS, TIME_T_INFINITE_NANOSECONDS));
    }

    const auto value = duration.value();
    constexpr std::int64_t kNanosecondsPerSecond = 1000000000LL;
    const auto seconds = value.count() / kNanosecondsPerSecond;
    const auto nanoseconds = value.count() % kNanosecondsPerSecond;
    if (seconds > std::numeric_limits<std::int32_t>::max()) {
        return Result<Duration_t>::failure(
            Error(ErrorCode::Unsupported, "QoS duration exceeds the Fast DDS range"));
    }
    return Result<Duration_t>::success(
        Duration_t(static_cast<std::int32_t>(seconds), static_cast<std::uint32_t>(nanoseconds)));
}

template <class QosT>
void apply_ros2_compatibility_defaults(QosT& qos) noexcept {
    using namespace eprosima::fastdds::dds;

    qos.history().kind = KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 10;
    qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    qos.durability().kind = VOLATILE_DURABILITY_QOS;
}

/// Apply only middleware-neutral DMW QoS fields to a concrete DDS endpoint QoS.
template <class QosT>
Result<void> apply_neutral_qos(const Qos& source, QosT& qos) {
    using namespace eprosima::fastdds::dds;

    if (source.history() == HistoryPolicy::KeepLast) {
        if (source.depth() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            return Result<void>::failure(
                Error(ErrorCode::Unsupported, "QoS history depth exceeds the Fast DDS range"));
        }
        qos.history().kind = KEEP_LAST_HISTORY_QOS;
        qos.history().depth = static_cast<std::int32_t>(source.depth());
    } else if (source.history() == HistoryPolicy::KeepAll) {
        qos.history().kind = KEEP_ALL_HISTORY_QOS;
    }

    if (source.reliability() == ReliabilityPolicy::Reliable) {
        qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    } else if (source.reliability() == ReliabilityPolicy::BestEffort) {
        qos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    }

    if (source.durability() == DurabilityPolicy::Volatile) {
        qos.durability().kind = VOLATILE_DURABILITY_QOS;
    } else if (source.durability() == DurabilityPolicy::TransientLocal) {
        qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    }

    if (source.deadline().kind() != QosDuration::Kind::SystemDefault) {
        auto duration = to_duration(source.deadline());
        if (!duration) return Result<void>::failure(std::move(duration.error()));
        qos.deadline().period = std::move(duration.value());
    }
    if (source.lifespan().kind() != QosDuration::Kind::SystemDefault) {
        auto duration = to_duration(source.lifespan());
        if (!duration) return Result<void>::failure(std::move(duration.error()));
        qos.lifespan().duration = std::move(duration.value());
    }
    if (source.liveliness() == LivelinessPolicy::Automatic) {
        qos.liveliness().kind = AUTOMATIC_LIVELINESS_QOS;
    } else if (source.liveliness() == LivelinessPolicy::ManualByTopic) {
        qos.liveliness().kind = MANUAL_BY_TOPIC_LIVELINESS_QOS;
    }
    if (source.liveliness_lease_duration().kind() != QosDuration::Kind::SystemDefault) {
        auto duration = to_duration(source.liveliness_lease_duration());
        if (!duration) return Result<void>::failure(std::move(duration.error()));
        qos.liveliness().lease_duration = std::move(duration.value());
    }
    return Result<void>::success();
}

inline void apply_ros2_writer_implementation_policy(
    eprosima::fastdds::dds::DataWriterQos& qos) noexcept {
    qos.endpoint().history_memory_policy =
        eprosima::fastrtps::rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE;
    qos.publish_mode().kind = eprosima::fastrtps::SYNCHRONOUS_PUBLISH_MODE;
    qos.data_sharing().off();
    // This is a frozen rmw_fastrtps compatibility value, not a public DMW QoS policy.
    qos.reliability().max_blocking_time = eprosima::fastrtps::Duration_t(0, 100000000U);
}

inline void apply_ros2_reader_implementation_policy(
    eprosima::fastdds::dds::DataReaderQos& qos) noexcept {
    qos.endpoint().history_memory_policy =
        eprosima::fastrtps::rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE;
    qos.data_sharing().off();
}

inline Result<eprosima::fastdds::dds::DataWriterQos> to_writer_qos(
    const Qos& source, RuntimeMode runtime_mode, eprosima::fastdds::dds::DataWriterQos qos) {
    if (runtime_mode == RuntimeMode::ROS2) apply_ros2_compatibility_defaults(qos);
    auto result = apply_neutral_qos(source, qos);
    if (!result)
        return Result<eprosima::fastdds::dds::DataWriterQos>::failure(std::move(result.error()));
    if (runtime_mode == RuntimeMode::ROS2) apply_ros2_writer_implementation_policy(qos);
    return Result<eprosima::fastdds::dds::DataWriterQos>::success(std::move(qos));
}

inline Result<eprosima::fastdds::dds::DataReaderQos> to_reader_qos(
    const Qos& source, RuntimeMode runtime_mode, eprosima::fastdds::dds::DataReaderQos qos) {
    if (runtime_mode == RuntimeMode::ROS2) apply_ros2_compatibility_defaults(qos);
    auto result = apply_neutral_qos(source, qos);
    if (!result)
        return Result<eprosima::fastdds::dds::DataReaderQos>::failure(std::move(result.error()));
    if (runtime_mode == RuntimeMode::ROS2) apply_ros2_reader_implementation_policy(qos);
    return Result<eprosima::fastdds::dds::DataReaderQos>::success(std::move(qos));
}

inline Result<QosDuration> from_duration(const eprosima::fastrtps::Duration_t& duration) {
    if (duration.is_infinite()) return Result<QosDuration>::success(QosDuration::infinite());
    if (duration.seconds < 0 || duration.nanosec >= 1000000000U) {
        return Result<QosDuration>::failure(
            Error(ErrorCode::DDSError, "Fast DDS returned an invalid QoS duration"));
    }
    constexpr std::int64_t kNanosecondsPerSecond = 1000000000LL;
    if (duration.seconds > std::numeric_limits<std::int64_t>::max() / kNanosecondsPerSecond) {
        return Result<QosDuration>::failure(
            Error(ErrorCode::Unsupported, "Fast DDS QoS duration exceeds DMW range"));
    }
    return QosDuration::finite(std::chrono::nanoseconds(
        static_cast<std::int64_t>(duration.seconds) * kNanosecondsPerSecond + duration.nanosec));
}

template <class QosT>
Result<Qos> from_neutral_qos(const QosT& source) {
    using namespace eprosima::fastdds::dds;
    Qos qos;
    switch (source.history().kind) {
        case KEEP_LAST_HISTORY_QOS: {
            if (source.history().depth <= 0) {
                return Result<Qos>::failure(
                    Error(ErrorCode::DDSError, "Fast DDS returned an invalid KeepLast depth"));
            }
            auto result = qos.keep_last(static_cast<std::size_t>(source.history().depth));
            if (!result) return Result<Qos>::failure(std::move(result.error()));
            break;
        }
        case KEEP_ALL_HISTORY_QOS:
            qos.keep_all();
            break;
        default:
            qos.history_system_default();
            break;
    }
    switch (source.reliability().kind) {
        case RELIABLE_RELIABILITY_QOS:
            qos.reliable();
            break;
        case BEST_EFFORT_RELIABILITY_QOS:
            qos.best_effort();
            break;
        default:
            qos.reliability_system_default();
            break;
    }
    switch (source.durability().kind) {
        case VOLATILE_DURABILITY_QOS:
            qos.volatile_();
            break;
        case TRANSIENT_LOCAL_DURABILITY_QOS:
            qos.transient_local();
            break;
        default:
            qos.durability_system_default();
            break;
    }
    auto deadline = from_duration(source.deadline().period);
    if (!deadline) return Result<Qos>::failure(std::move(deadline.error()));
    auto lifespan = from_duration(source.lifespan().duration);
    if (!lifespan) return Result<Qos>::failure(std::move(lifespan.error()));
    auto lease_duration = from_duration(source.liveliness().lease_duration);
    if (!lease_duration) return Result<Qos>::failure(std::move(lease_duration.error()));
    qos.deadline(deadline.value())
        .lifespan(lifespan.value())
        .liveliness_lease_duration(lease_duration.value());
    switch (source.liveliness().kind) {
        case AUTOMATIC_LIVELINESS_QOS:
            qos.liveliness(LivelinessPolicy::Automatic);
            break;
        case MANUAL_BY_TOPIC_LIVELINESS_QOS:
            qos.liveliness(LivelinessPolicy::ManualByTopic);
            break;
        default:
            break;
    }
    return Result<Qos>::success(std::move(qos));
}

/// Reverse map the discovery-visible subset of an RTPS WriterQos/ReaderQos.
///
/// History is a DDS-level policy that discovery metadata does not carry, so it
/// stays SystemDefault/unknown rather than being guessed.
template <class DiscoveryQosT>
Result<Qos> from_discovery_qos(const DiscoveryQosT& source) {
    using namespace eprosima::fastdds::dds;
    Qos qos;
    switch (source.m_reliability.kind) {
        case RELIABLE_RELIABILITY_QOS:
            qos.reliable();
            break;
        case BEST_EFFORT_RELIABILITY_QOS:
            qos.best_effort();
            break;
        default:
            qos.reliability_system_default();
            break;
    }
    switch (source.m_durability.kind) {
        case VOLATILE_DURABILITY_QOS:
            qos.volatile_();
            break;
        case TRANSIENT_LOCAL_DURABILITY_QOS:
            qos.transient_local();
            break;
        default:
            qos.durability_system_default();
            break;
    }
    const auto deadline = from_duration(source.m_deadline.period);
    if (!deadline) return Result<Qos>::failure(std::move(deadline.error()));
    const auto lifespan = from_duration(source.m_lifespan.duration);
    if (!lifespan) return Result<Qos>::failure(std::move(lifespan.error()));
    const auto lease_duration = from_duration(source.m_liveliness.lease_duration);
    if (!lease_duration) return Result<Qos>::failure(std::move(lease_duration.error()));
    qos.deadline(deadline.value())
        .lifespan(lifespan.value())
        .liveliness_lease_duration(lease_duration.value());
    switch (source.m_liveliness.kind) {
        case AUTOMATIC_LIVELINESS_QOS:
            qos.liveliness(LivelinessPolicy::Automatic);
            break;
        case MANUAL_BY_TOPIC_LIVELINESS_QOS:
            qos.liveliness(LivelinessPolicy::ManualByTopic);
            break;
        default:
            break;
    }
    return Result<Qos>::success(std::move(qos));
}

/// Listener-path helper: a discovery QoS that cannot be reverse mapped falls
/// back to the explicit unknown contract instead of reporting a fake value.
template <class DiscoveryQosT>
Qos discovery_qos_or_unknown(const DiscoveryQosT& source) noexcept {
    try {
        auto converted = from_discovery_qos(source);
        if (converted) return converted.value();
    } catch (...) {
    }
    return Qos{};
}

}  // namespace impl
}  // namespace dmw

#endif  // DMW_IMPL__FASTDDS__QOS_HPP_
