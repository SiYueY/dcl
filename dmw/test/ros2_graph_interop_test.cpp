#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <typeindex>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"

#include "dmw/context.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/graph.hpp"
#include "dmw/node.hpp"
#include "dmw/publisher.hpp"

namespace {

using namespace std::chrono_literals;

/// Name-only binding: this test validates graph metadata interoperability, not
/// payload codecs.
class NamedTypeSupport final : public eprosima::fastdds::dds::TopicDataType {
public:
    explicit NamedTypeSupport(const char* name) {
        m_typeSize = 256;
        m_isGetKeyDefined = false;
        setName(name);
    }

    bool serialize(void*, eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
        payload->length = 0;
        return true;
    }

    bool deserialize(eprosima::fastrtps::rtps::SerializedPayload_t*, void*) override {
        return true;
    }

    std::function<std::uint32_t()> getSerializedSizeProvider(void*) override {
        return [] { return 0U; };
    }

    void* createData() override { return nullptr; }

    void deleteData(void*) override {}

    bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override {
        return false;
    }
};

std::uint32_t ros_domain_id() {
    const char* value = std::getenv("ROS_DOMAIN_ID");
    assert(value != nullptr);
    char* end = nullptr;
    const auto parsed = std::strtoul(value, &end, 10);
    assert(*value != '\0' && *end == '\0' && parsed <= 232U);
    return static_cast<std::uint32_t>(parsed);
}

dmw::MessageType named_type(const char* name) {
    eprosima::fastdds::dds::TypeSupport support(new NamedTypeSupport(name));
    auto type = dmw::fastdds::MessageTypeAdapter::create(
        std::move(support), std::type_index(typeid(NamedTypeSupport)));
    assert(type);
    return type.value();
}

bool snapshot_has_node(const dmw::GraphSnapshot& snapshot, const std::string& name) {
    for (const auto& node : snapshot.nodes) {
        if (node.node_name == name) return true;
    }
    return false;
}

}  // namespace

int main() {
    rclcpp::init(0, nullptr);
    auto ros_node = std::make_shared<rclcpp::Node>("ros_graph_interop");
    auto ros_publisher = ros_node->create_publisher<std_msgs::msg::String>("/ros_graph_topic", 10);
    (void)ros_publisher;

    dmw::ContextOptions options;
    options.runtime_mode = dmw::RuntimeMode::ROS2;
    options.domain_id = ros_domain_id();
    options.participant_name = "dmw-ros2-graph-interop";
    auto context = dmw::Context::create(options);
    assert(context);
    assert(context.value()->runtime_mode() == dmw::RuntimeMode::ROS2);

    dmw::NodeOptions node_options;
    node_options.node_name = "dmw_graph_interop";
    auto node = context.value()->create_node(node_options);
    assert(node);
    auto publisher = node.value()->create_publisher(
        named_type("std_msgs::msg::dds_::String_"), "/dmw_graph_topic",
        dmw::Qos::ros2_default());
    assert(publisher);

    // --- the DMW graph learns the ROS 2 node and its endpoints ----------------
    bool saw_ros_node = false;
    bool associated_endpoint = false;
    dmw::GraphSnapshot snapshot;
    for (int attempt = 0; attempt < 100 && (!saw_ros_node || !associated_endpoint); ++attempt) {
        std::this_thread::sleep_for(100ms);
        const auto current = context.value()->graph_snapshot();
        assert(current);
        snapshot = current.value();
        saw_ros_node = snapshot_has_node(snapshot, "ros_graph_interop");
        associated_endpoint = false;
        for (const auto& endpoint : snapshot.topic_endpoints) {
            if (endpoint.topic_name != "/ros_graph_topic") continue;
            if (endpoint.node_name != "ros_graph_interop") continue;
            if (endpoint.kind != dmw::EndpointKind::Publisher) continue;
            if (endpoint.node_namespace != "/") continue;
            associated_endpoint = true;
        }
    }
    assert(saw_ros_node);
    assert(associated_endpoint);

    // Local metadata is published too, so the local Node/endpoint association
    // stays visible for this participant.
    bool saw_dmw_node = false;
    for (const auto& graph_node : snapshot.nodes) {
        if (graph_node.node_name == "dmw_graph_interop") saw_dmw_node = true;
    }
    assert(saw_dmw_node);

    // The internal transport never shows up as a user topic.
    for (const auto& topic : snapshot.topics) {
        assert(topic.topic_name != "/ros_discovery_info");
        assert(topic.topic_name != "ros_discovery_info");
    }
    for (const auto& endpoint : snapshot.topic_endpoints) {
        assert(endpoint.topic_name != "/ros_discovery_info");
        assert(endpoint.topic_name != "ros_discovery_info");
    }

    // --- the ROS 2 graph learns the DMW node ----------------------------------
    bool ros_saw_dmw_node = false;
    for (int attempt = 0; attempt < 100 && !ros_saw_dmw_node; ++attempt) {
        std::this_thread::sleep_for(100ms);
        for (const auto& name : ros_node->get_node_names()) {
            if (name == "/dmw_graph_interop") ros_saw_dmw_node = true;
        }
    }
    assert(ros_saw_dmw_node);

    assert(context.value()->shutdown());
    rclcpp::shutdown();
    return 0;
}
