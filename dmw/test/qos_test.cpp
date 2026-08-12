// SPDX-License-Identifier: Apache-2.0

#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>

#include <fastdds/dds/core/policy/QosPolicies.hpp>

#include "dmw/qos.hpp"
#include "impl/fastdds/qos.hpp"

int main() {
    dmw::Qos qos;
    assert(qos.keep_last(7));
    qos.reliable().transient_local();
    const auto deadline = dmw::QosDuration::finite(std::chrono::milliseconds(250));
    const auto lifespan = dmw::QosDuration::finite(std::chrono::seconds(2));
    assert(deadline);
    assert(lifespan);
    qos.deadline(deadline.value())
        .lifespan(lifespan.value())
        .liveliness(dmw::LivelinessPolicy::ManualByTopic);
    qos.liveliness_lease_duration(dmw::QosDuration::infinite());

    const auto writer =
        dmw::impl::fastdds::make_writer_qos(qos, dmw::CompatibilityProfile::NativeDds);
    assert(writer);
    assert(writer.value().history().kind == eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS);
    assert(writer.value().history().depth == 7);
    assert(writer.value().reliability().kind == eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS);
    assert(
        writer.value().durability().kind == eprosima::fastdds::dds::TRANSIENT_LOCAL_DURABILITY_QOS);
    assert(writer.value().deadline().period.seconds == 0);
    assert(writer.value().deadline().period.nanosec == 250000000U);
    assert(writer.value().lifespan().duration.seconds == 2);
    assert(
        writer.value().liveliness().kind == eprosima::fastdds::dds::MANUAL_BY_TOPIC_LIVELINESS_QOS);

    const auto reader =
        dmw::impl::fastdds::make_reader_qos(qos, dmw::CompatibilityProfile::NativeDds);
    assert(reader);
    assert(reader.value().history().depth == 7);

    dmw::Qos keep_all;
    keep_all.keep_all().best_effort().volatile_();
    const auto keep_all_writer =
        dmw::impl::fastdds::make_writer_qos(keep_all, dmw::CompatibilityProfile::NativeDds);
    assert(keep_all_writer);
    assert(keep_all_writer.value().history().kind == eprosima::fastdds::dds::KEEP_ALL_HISTORY_QOS);
    assert(
        keep_all_writer.value().reliability().kind ==
        eprosima::fastdds::dds::BEST_EFFORT_RELIABILITY_QOS);
    assert(
        keep_all_writer.value().durability().kind ==
        eprosima::fastdds::dds::VOLATILE_DURABILITY_QOS);

    dmw::Qos too_deep;
    assert(too_deep.keep_last(
        static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) + 1U));
    const auto invalid_depth =
        dmw::impl::fastdds::make_reader_qos(too_deep, dmw::CompatibilityProfile::NativeDds);
    assert(!invalid_depth);
    assert(invalid_depth.error().code() == dmw::ErrorCode::Unsupported);

    const auto ros_writer = dmw::impl::fastdds::make_writer_qos(
        dmw::Qos{}, dmw::CompatibilityProfile::Ros2FastDdsHumble);
    assert(ros_writer);
    assert(ros_writer.value().history().kind == eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS);
    assert(ros_writer.value().history().depth == 10);
    assert(
        ros_writer.value().reliability().kind == eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS);
    assert(ros_writer.value().durability().kind == eprosima::fastdds::dds::VOLATILE_DURABILITY_QOS);

    return 0;
}
