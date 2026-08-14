// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__IMPL__FASTDDS__QOS_HPP_
#define DMW__IMPL__FASTDDS__QOS_HPP_

#include <cstdint>
#include <limits>

#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/rtps/attributes/HistoryAttributes.h>
#include <fastdds/rtps/common/Time_t.h>

#include "dmw/qos.hpp"
#include "dmw/compatibility.hpp"

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
Result<void> apply_common_qos(const Qos& source, CompatibilityProfile profile, QosT& target) {
    using namespace eprosima::fastdds::dds;

    if (profile == CompatibilityProfile::Ros2FastDdsHumble) {
        target.history().kind = KEEP_LAST_HISTORY_QOS;
        target.history().depth = 10;
        target.reliability().kind = RELIABLE_RELIABILITY_QOS;
        target.durability().kind = VOLATILE_DURABILITY_QOS;
    }

    if (source.history() == HistoryPolicy::KeepLast) {
        if (source.depth() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            return Result<void>::failure(
                Error(ErrorCode::Unsupported, "QoS history depth exceeds the Fast DDS range"));
        }
        target.history().kind = KEEP_LAST_HISTORY_QOS;
        target.history().depth = static_cast<std::int32_t>(source.depth());
    } else if (source.history() == HistoryPolicy::KeepAll) {
        target.history().kind = KEEP_ALL_HISTORY_QOS;
    }

    if (source.reliability() == ReliabilityPolicy::Reliable) {
        target.reliability().kind = RELIABLE_RELIABILITY_QOS;
    } else if (source.reliability() == ReliabilityPolicy::BestEffort) {
        target.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    }

    if (source.durability() == DurabilityPolicy::Volatile) {
        target.durability().kind = VOLATILE_DURABILITY_QOS;
    } else if (source.durability() == DurabilityPolicy::TransientLocal) {
        target.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    }

    if (source.deadline().kind() != QosDuration::Kind::SystemDefault) {
        auto duration = to_duration(source.deadline());
        if (!duration) return Result<void>::failure(std::move(duration.error()));
        target.deadline().period = std::move(duration.value());
    }
    if (source.lifespan().kind() != QosDuration::Kind::SystemDefault) {
        auto duration = to_duration(source.lifespan());
        if (!duration) return Result<void>::failure(std::move(duration.error()));
        target.lifespan().duration = std::move(duration.value());
    }
    if (source.liveliness() == LivelinessPolicy::Automatic) {
        target.liveliness().kind = AUTOMATIC_LIVELINESS_QOS;
    } else if (source.liveliness() == LivelinessPolicy::ManualByTopic) {
        target.liveliness().kind = MANUAL_BY_TOPIC_LIVELINESS_QOS;
    }
    if (source.liveliness_lease_duration().kind() != QosDuration::Kind::SystemDefault) {
        auto duration = to_duration(source.liveliness_lease_duration());
        if (!duration) return Result<void>::failure(std::move(duration.error()));
        target.liveliness().lease_duration = std::move(duration.value());
    }
    return Result<void>::success();
}

inline Result<eprosima::fastdds::dds::DataWriterQos> make_writer_qos(
    const Qos& source, CompatibilityProfile profile) {
    auto target = eprosima::fastdds::dds::DATAWRITER_QOS_DEFAULT;
    auto result = apply_common_qos(source, profile, target);
    if (!result)
        return Result<eprosima::fastdds::dds::DataWriterQos>::failure(std::move(result.error()));
    if (profile == CompatibilityProfile::Ros2FastDdsHumble) {
        target.endpoint().history_memory_policy =
            eprosima::fastrtps::rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE;
        target.publish_mode().kind = eprosima::fastrtps::SYNCHRONOUS_PUBLISH_MODE;
        target.data_sharing().off();
        // This is a frozen rmw_fastrtps compatibility value, not a public DMW QoS policy.
        target.reliability().max_blocking_time = eprosima::fastrtps::Duration_t(0, 100000000U);
    }
    return Result<eprosima::fastdds::dds::DataWriterQos>::success(std::move(target));
}

inline Result<eprosima::fastdds::dds::DataReaderQos> make_reader_qos(
    const Qos& source, CompatibilityProfile profile) {
    auto target = eprosima::fastdds::dds::DATAREADER_QOS_DEFAULT;
    auto result = apply_common_qos(source, profile, target);
    if (!result)
        return Result<eprosima::fastdds::dds::DataReaderQos>::failure(std::move(result.error()));
    if (profile == CompatibilityProfile::Ros2FastDdsHumble) {
        target.endpoint().history_memory_policy =
            eprosima::fastrtps::rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE;
        target.data_sharing().off();
    }
    return Result<eprosima::fastdds::dds::DataReaderQos>::success(std::move(target));
}

}  // namespace fastdds
}  // namespace impl
}  // namespace dmw

#endif  // DMW__IMPL__FASTDDS__QOS_HPP_
