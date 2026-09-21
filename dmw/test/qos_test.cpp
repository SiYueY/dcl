#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>

#include <fastdds/dds/core/policy/QosPolicies.hpp>

#include "dmw/qos.hpp"
#include "impl/qos.hpp"
#include "impl/return_code.hpp"

int main() {
    using ReturnCode = eprosima::fastrtps::types::ReturnCode_t;
    assert(
        dmw::impl::to_return_code(ReturnCode::RETCODE_OUT_OF_RESOURCES) ==
        dmw::ErrorCode::ResourceExhausted);
    assert(
        dmw::impl::to_return_code(ReturnCode::RETCODE_INCONSISTENT_POLICY) ==
        dmw::ErrorCode::IncompatibleQos);
    assert(dmw::impl::to_return_code(ReturnCode::RETCODE_TIMEOUT) == dmw::ErrorCode::Timeout);
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

    const auto writer = dmw::impl::to_writer_qos(
        qos, dmw::RuntimeMode::DDS, eprosima::fastdds::dds::DATAWRITER_QOS_DEFAULT);
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

    const auto reader = dmw::impl::to_reader_qos(
        qos, dmw::RuntimeMode::DDS, eprosima::fastdds::dds::DATAREADER_QOS_DEFAULT);
    assert(reader);
    assert(reader.value().history().depth == 7);

    dmw::Qos keep_all;
    keep_all.keep_all().best_effort().volatile_();
    const auto keep_all_writer = dmw::impl::to_writer_qos(
        keep_all, dmw::RuntimeMode::DDS, eprosima::fastdds::dds::DATAWRITER_QOS_DEFAULT);
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
    const auto invalid_depth = dmw::impl::to_reader_qos(
        too_deep, dmw::RuntimeMode::DDS, eprosima::fastdds::dds::DATAREADER_QOS_DEFAULT);
    assert(!invalid_depth);
    assert(invalid_depth.error().code() == dmw::ErrorCode::Unsupported);

    const auto ros_writer = dmw::impl::to_writer_qos(
        dmw::Qos{}, dmw::RuntimeMode::ROS2, eprosima::fastdds::dds::DATAWRITER_QOS_DEFAULT);
    assert(ros_writer);
    assert(ros_writer.value().history().kind == eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS);
    assert(ros_writer.value().history().depth == 10);
    assert(
        ros_writer.value().reliability().kind == eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS);
    assert(ros_writer.value().durability().kind == eprosima::fastdds::dds::VOLATILE_DURABILITY_QOS);
    assert(
        ros_writer.value().endpoint().history_memory_policy ==
        eprosima::fastrtps::rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE);
    assert(ros_writer.value().publish_mode().kind == eprosima::fastrtps::SYNCHRONOUS_PUBLISH_MODE);
    assert(
        ros_writer.value().data_sharing().kind() == eprosima::fastdds::dds::DataSharingKind::OFF);
    assert(ros_writer.value().reliability().max_blocking_time.seconds == 0);
    assert(ros_writer.value().reliability().max_blocking_time.nanosec == 100000000U);
    const auto ros_reader = dmw::impl::to_reader_qos(
        dmw::Qos{}, dmw::RuntimeMode::ROS2, eprosima::fastdds::dds::DATAREADER_QOS_DEFAULT);
    assert(ros_reader);
    assert(
        ros_reader.value().endpoint().history_memory_policy ==
        eprosima::fastrtps::rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE);
    assert(
        ros_reader.value().data_sharing().kind() == eprosima::fastdds::dds::DataSharingKind::OFF);

    auto captured_writer_baseline = eprosima::fastdds::dds::DATAWRITER_QOS_DEFAULT;
    captured_writer_baseline.reliability().kind =
        eprosima::fastdds::dds::BEST_EFFORT_RELIABILITY_QOS;
    const auto baseline_writer = dmw::impl::to_writer_qos(
        dmw::Qos::system_default(), dmw::RuntimeMode::DDS, captured_writer_baseline);
    assert(baseline_writer);
    assert(
        baseline_writer.value().reliability().kind ==
        eprosima::fastdds::dds::BEST_EFFORT_RELIABILITY_QOS);

    const auto sensor_data = dmw::Qos::ros2_sensor_data();
    assert(sensor_data.history() == dmw::HistoryPolicy::KeepLast);
    assert(sensor_data.depth() == 5);
    assert(sensor_data.reliability() == dmw::ReliabilityPolicy::BestEffort);
    assert(sensor_data.durability() == dmw::DurabilityPolicy::Volatile);

    const auto parameters = dmw::Qos::ros2_parameters();
    assert(parameters.history() == dmw::HistoryPolicy::KeepLast);
    assert(parameters.depth() == 1000);
    assert(parameters.reliability() == dmw::ReliabilityPolicy::Reliable);
    assert(parameters.durability() == dmw::DurabilityPolicy::Volatile);

    const auto parameter_events = dmw::Qos::ros2_parameter_events();
    assert(parameter_events.history() == dmw::HistoryPolicy::KeepLast);
    assert(parameter_events.depth() == 1000);
    assert(parameter_events.reliability() == dmw::ReliabilityPolicy::Reliable);
    assert(parameter_events.durability() == dmw::DurabilityPolicy::Volatile);

    const auto action_status = dmw::Qos::ros2_action_status_default();
    assert(action_status.history() == dmw::HistoryPolicy::KeepLast);
    assert(action_status.depth() == 1);
    assert(action_status.reliability() == dmw::ReliabilityPolicy::Reliable);
    assert(action_status.durability() == dmw::DurabilityPolicy::TransientLocal);

    auto reliable_publisher = dmw::Qos::ros2_default();
    auto best_effort_subscriber = dmw::Qos::ros2_sensor_data();
    const auto compatible =
        dmw::check_qos_compatibility(reliable_publisher, best_effort_subscriber);
    assert(compatible);
    assert(compatible.value().compatibility == dmw::QosCompatibility::Compatible);

    const auto incompatible =
        dmw::check_qos_compatibility(best_effort_subscriber, reliable_publisher);
    assert(incompatible);
    assert(incompatible.value().compatibility == dmw::QosCompatibility::Incompatible);

    const auto uncertain = dmw::check_qos_compatibility(dmw::Qos{}, reliable_publisher);
    assert(uncertain);
    assert(uncertain.value().compatibility == dmw::QosCompatibility::Warning);

    return 0;
}
