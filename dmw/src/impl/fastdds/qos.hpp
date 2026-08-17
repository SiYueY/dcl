#ifndef DMW_IMPL__FASTDDS__QOS_HPP_
#define DMW_IMPL__FASTDDS__QOS_HPP_

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

namespace fastdds {

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
Result<void> to_qos(const Qos& source, RuntimeMode runtime_mode, QosT& qos) {
    using namespace eprosima::fastdds::dds;

    if (runtime_mode == RuntimeMode::ROS2) {
        qos.history().kind = KEEP_LAST_HISTORY_QOS;
        qos.history().depth = 10;
        qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        qos.durability().kind = VOLATILE_DURABILITY_QOS;
    }

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

inline Result<eprosima::fastdds::dds::DataWriterQos> to_writer_qos(
    const Qos& source, RuntimeMode runtime_mode) {
    auto qos = eprosima::fastdds::dds::DATAWRITER_QOS_DEFAULT;
    auto result = to_qos(source, runtime_mode, qos);
    if (!result)
        return Result<eprosima::fastdds::dds::DataWriterQos>::failure(std::move(result.error()));
    if (runtime_mode == RuntimeMode::ROS2) {
        qos.endpoint().history_memory_policy =
            eprosima::fastrtps::rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE;
        qos.publish_mode().kind = eprosima::fastrtps::SYNCHRONOUS_PUBLISH_MODE;
        qos.data_sharing().off();
        // This is a frozen rmw_fastrtps compatibility value, not a public DMW QoS policy.
        qos.reliability().max_blocking_time = eprosima::fastrtps::Duration_t(0, 100000000U);
    }
    return Result<eprosima::fastdds::dds::DataWriterQos>::success(std::move(qos));
}

inline Result<eprosima::fastdds::dds::DataReaderQos> to_reader_qos(
    const Qos& source, RuntimeMode runtime_mode) {
    auto qos = eprosima::fastdds::dds::DATAREADER_QOS_DEFAULT;
    auto result = to_qos(source, runtime_mode, qos);
    if (!result)
        return Result<eprosima::fastdds::dds::DataReaderQos>::failure(std::move(result.error()));
    if (runtime_mode == RuntimeMode::ROS2) {
        qos.endpoint().history_memory_policy =
            eprosima::fastrtps::rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE;
        qos.data_sharing().off();
    }
    return Result<eprosima::fastdds::dds::DataReaderQos>::success(std::move(qos));
}

}  // namespace fastdds
}  // namespace impl
}  // namespace dmw

#endif  // DMW_IMPL__FASTDDS__QOS_HPP_
