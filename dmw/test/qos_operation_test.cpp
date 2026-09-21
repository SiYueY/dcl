#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>

#include "fastdds/dds/topic/TopicDataType.hpp"

#include "dmw/context.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/node.hpp"
#include "dmw/publisher.hpp"
#include "dmw/qos.hpp"
#include "dmw/subscriber.hpp"
#include "dmw/wait_timeout.hpp"

namespace {

using namespace std::chrono_literals;

class IntTopicDataType final : public eprosima::fastdds::dds::TopicDataType {
public:
    IntTopicDataType() {
        m_typeSize = sizeof(int);
        setName("dmw.test.QosOperationInt");
    }
    bool serialize(void* data, eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
        payload->length = sizeof(int);
        std::memcpy(payload->data, data, sizeof(int));
        return true;
    }
    bool deserialize(eprosima::fastrtps::rtps::SerializedPayload_t* payload, void* data) override {
        if (payload->length != sizeof(int)) return false;
        std::memcpy(data, payload->data, sizeof(int));
        return true;
    }
    std::function<std::uint32_t()> getSerializedSizeProvider(void*) override {
        return [] { return static_cast<std::uint32_t>(sizeof(int)); };
    }
    void* createData() override { return new int(0); }
    void deleteData(void* data) override { delete static_cast<int*>(data); }
    bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override {
        return false;
    }
};

}  // namespace

int main() {
    if (std::getenv("DMW_ENABLE_DDS_INTEGRATION") == nullptr) return 0;

    const auto message_type = dmw::fastdds::create_message_type<IntTopicDataType, int>();
    assert(message_type);

    dmw::ContextOptions options;
    options.domain_id = 190U;
    options.participant_name = "dmw-qos-operation";
    auto context = dmw::Context::create(options);
    assert(context);
    dmw::NodeOptions node_options;
    node_options.node_name = "qos_operation";
    auto node = context.value()->create_node(node_options);
    assert(node);

    // --- wait_for_all_acked: nothing to acknowledge --------------------------
    auto lonely = node.value()->create_publisher(
        message_type.value(), "/qos_lonely", dmw::Qos::ros2_default());
    assert(lonely);
    const int sample = 1;
    assert(lonely.value()->write(&sample));
    const auto acked = lonely.value()->wait_for_all_acked(dmw::WaitTimeout::infinite());
    assert(acked && acked.value());
    const auto polled = lonely.value()->wait_for_all_acked(dmw::WaitTimeout::poll());
    assert(polled && polled.value());

    // --- wait_for_all_acked: matched reliable reader --------------------------
    auto publisher = node.value()->create_publisher(
        message_type.value(), "/qos_matched", dmw::Qos::ros2_default());
    auto subscriber = node.value()->create_subscriber(
        message_type.value(), "/qos_matched", dmw::Qos::ros2_default());
    assert(publisher && subscriber);
    for (int attempt = 0; attempt < 1500; ++attempt) {
        const auto matched = publisher.value()->matched_subscriber_count();
        assert(matched);
        if (matched.value() > 0) break;
        std::this_thread::sleep_for(10ms);
    }
    assert(publisher.value()->matched_subscriber_count().value() > 0);
    assert(publisher.value()->write(&sample));
    const auto matched_acked =
        publisher.value()->wait_for_all_acked(dmw::WaitTimeout::infinite());
    assert(matched_acked && matched_acked.value());

    // --- wait_for_all_acked: finite deadline that has already expired ---------
    assert(publisher.value()->write(&sample));
    const auto immediate = dmw::WaitTimeout::finite(1ns);
    assert(immediate);
    const auto timed_out = publisher.value()->wait_for_all_acked(immediate.value());
    assert(timed_out);
    assert(!timed_out.value());

    // --- assert_liveliness: supported with ManualByTopic ---------------------
    const auto lease = dmw::QosDuration::finite(5s);
    assert(lease);
    dmw::Qos manual;
    const auto kept = manual.keep_last(10);
    assert(kept);
    manual.reliable().liveliness(dmw::LivelinessPolicy::ManualByTopic);
    manual.liveliness_lease_duration(lease.value());
    auto manual_publisher =
        node.value()->create_publisher(message_type.value(), "/qos_liveliness", manual);
    assert(manual_publisher);
    assert(manual_publisher.value()->write(&sample));
    const auto manual_assert = manual_publisher.value()->assert_liveliness();
    assert(manual_assert);

    // --- assert_liveliness: rejected when liveliness is Automatic ------------
    auto automatic_publisher = node.value()->create_publisher(
        message_type.value(), "/qos_liveliness_auto", dmw::Qos::ros2_default());
    assert(automatic_publisher);
    const auto rejected = automatic_publisher.value()->assert_liveliness();
    assert(!rejected);
    assert(rejected.error().code() == dmw::ErrorCode::InvalidState);

    // --- QoS-incompatible endpoints never match -------------------------------
    // A best-effort writer cannot satisfy a reliable reader.  A compatible
    // reader on the same topic proves discovery is settled before the negative
    // assertion is made.
    auto best_effort_publisher = node.value()->create_publisher(
        message_type.value(), "/qos_incompatible", dmw::Qos::ros2_sensor_data());
    auto reliable_subscriber = node.value()->create_subscriber(
        message_type.value(), "/qos_incompatible", dmw::Qos::ros2_default());
    assert(best_effort_publisher && reliable_subscriber);
    std::this_thread::sleep_for(500ms);
    assert(reliable_subscriber.value()->matched_publisher_count().value() == 0);
    assert(best_effort_publisher.value()->matched_subscriber_count().value() == 0);

    auto compatible_subscriber = node.value()->create_subscriber(
        message_type.value(), "/qos_incompatible", dmw::Qos::ros2_sensor_data());
    assert(compatible_subscriber);
    for (int attempt = 0; attempt < 1500; ++attempt) {
        const auto matched = best_effort_publisher.value()->matched_subscriber_count();
        assert(matched);
        if (matched.value() > 0) break;
        std::this_thread::sleep_for(10ms);
    }
    assert(best_effort_publisher.value()->matched_subscriber_count().value() == 1);
    // Discovery for this topic is now settled, so the reliable reader is still
    // unmatched and the publisher still sees exactly one compatible reader.
    assert(reliable_subscriber.value()->matched_publisher_count().value() == 0);
    assert(best_effort_publisher.value()->matched_subscriber_count().value() == 1);

    // --- both operations observe Context shutdown ----------------------------
    assert(context.value()->shutdown());
    const auto shutdown_ack = publisher.value()->wait_for_all_acked(dmw::WaitTimeout::infinite());
    assert(!shutdown_ack);
    assert(shutdown_ack.error().code() == dmw::ErrorCode::ContextShutdown);
    const auto shutdown_assert = manual_publisher.value()->assert_liveliness();
    assert(!shutdown_assert);
    assert(shutdown_assert.error().code() == dmw::ErrorCode::ContextShutdown);
    return 0;
}
