#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "fastdds/dds/topic/TopicDataType.hpp"

#include "dmw/arguments.hpp"
#include "dmw/client.hpp"
#include "dmw/context.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/node.hpp"
#include "dmw/publisher.hpp"
#include "dmw/server.hpp"
#include "dmw/service_type.hpp"
#include "dmw/subscriber.hpp"

namespace {

using namespace std::chrono_literals;

class IntTopicDataType final : public eprosima::fastdds::dds::TopicDataType {
public:
    IntTopicDataType() {
        m_typeSize = sizeof(int);
        setName("dmw.test.RemapInt");
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

dmw::Arguments parse(const std::vector<std::string>& argv) {
    const auto arguments = dmw::parse_arguments(argv);
    assert(arguments);
    return arguments.value();
}

void wait_for_service(dmw::Client& client) {
    // Discovery is asynchronous; allow a generous budget so a loaded machine
    // cannot turn this into a flaky failure.
    for (int attempt = 0; attempt < 1500; ++attempt) {
        const auto available = client.service_is_available();
        assert(available);
        if (available.value()) return;
        std::this_thread::sleep_for(10ms);
    }
    assert(false && "service never became available");
}

}  // namespace

int main() {
    if (std::getenv("DMW_ENABLE_DDS_INTEGRATION") == nullptr) return 0;

    const auto message_type = dmw::fastdds::create_message_type<IntTopicDataType, int>();
    assert(message_type);
    const dmw::ServiceType service_type(message_type.value(), message_type.value());

    // --- node identity remap: global arguments then node-local override -------
    dmw::ContextOptions context_options;
    context_options.domain_id = 180U;
    context_options.participant_name = "dmw-remapping";
    context_options.arguments = parse(
        {"app", "--ros-args", "-r", "__node:=global_node", "-r", "__ns:=/global_ns", "--"});
    auto context = dmw::Context::create(context_options);
    assert(context);

    dmw::NodeOptions global_node_options;
    global_node_options.node_name = "ignored_name";
    auto global_node = context.value()->create_node(global_node_options);
    assert(global_node);
    assert(global_node.value()->name() == "global_node");
    assert(global_node.value()->node_namespace() == "/global_ns");
    assert(global_node.value()->fully_qualified_name() == "/global_ns/global_node");

    // Global `__node` identity remap applies to every Node of the Context
    // (matching rcl), while a Node-local rule can still override the namespace.
    dmw::NodeOptions local_node_options;
    local_node_options.node_name = "local_node";
    local_node_options.arguments = parse({"--ros-args", "-r", "__ns:=/local_ns", "--"});
    auto local_node = context.value()->create_node(local_node_options);
    assert(local_node);
    assert(local_node.value()->name() == "global_node");
    assert(local_node.value()->node_namespace() == "/local_ns");
    assert(local_node.value()->fully_qualified_name() == "/local_ns/global_node");

    dmw::NodeOptions isolated_options;
    isolated_options.node_name = "isolated_node";
    isolated_options.use_global_arguments = false;
    auto isolated_node = context.value()->create_node(isolated_options);
    assert(isolated_node);
    assert(isolated_node.value()->name() == "isolated_node");
    assert(isolated_node.value()->node_namespace() == "/");

    // --- topic/service remap and rule precedence ------------------------------
    dmw::ContextOptions rule_context_options;
    rule_context_options.domain_id = 181U;
    rule_context_options.participant_name = "dmw-remapping-rules";
    rule_context_options.arguments =
        parse({"app", "--ros-args", "-r", "shared_in:=shared_global", "--"});
    auto rule_context = dmw::Context::create(rule_context_options);
    assert(rule_context);

    // A Context-level rule applies to every Node of that Context.
    dmw::NodeOptions global_rule_options;
    global_rule_options.node_name = "global_rule_node";
    global_rule_options.node_namespace = "/global_rule_ns";
    auto global_rule_node = rule_context.value()->create_node(global_rule_options);
    assert(global_rule_node);
    auto global_rule_publisher = global_rule_node.value()->create_publisher(
        message_type.value(), "shared_in", dmw::Qos::ros2_default());
    assert(global_rule_publisher);
    assert(global_rule_publisher.value()->topic_name() == "/global_rule_ns/shared_global");

    // A Node-local rule for the same source wins because it is applied later.
    dmw::NodeOptions local_rule_options;
    local_rule_options.node_name = "local_rule_node";
    local_rule_options.node_namespace = "/local_rule_ns";
    local_rule_options.arguments =
        parse({"app", "--ros-args", "-r", "shared_in:=shared_local", "--"});
    auto local_rule_node = rule_context.value()->create_node(local_rule_options);
    assert(local_rule_node);
    auto local_rule_publisher = local_rule_node.value()->create_publisher(
        message_type.value(), "shared_in", dmw::Qos::ros2_default());
    assert(local_rule_publisher);
    assert(local_rule_publisher.value()->topic_name() == "/local_rule_ns/shared_local");

    // One Context-level rule set addresses two different Nodes through a
    // node-scoped rule.
    const std::vector<std::string> remap_rules{
        "app", "--ros-args",
        "-r", "input:=output",
        "-r", "/absolute_in:=/absolute_out",
        "-r", "old_service:=new_service",
        "-r", "other_node:input:=wrong_output",
        "-r", "remap_node:scoped_in:=scoped_out",
        "--"};
    dmw::NodeOptions remap_options;
    remap_options.node_name = "remap_node";
    remap_options.node_namespace = "/remap";
    remap_options.use_global_arguments = false;
    remap_options.arguments = parse(remap_rules);
    auto remap_node = rule_context.value()->create_node(remap_options);
    assert(remap_node);

    auto publisher = remap_node.value()->create_publisher(
        message_type.value(), "input", dmw::Qos::ros2_default());
    assert(publisher);
    assert(publisher.value()->topic_name() == "/remap/output");
    auto absolute = remap_node.value()->create_publisher(
        message_type.value(), "/absolute_in", dmw::Qos::ros2_default());
    assert(absolute);
    assert(absolute.value()->topic_name() == "/absolute_out");
    auto scoped = remap_node.value()->create_publisher(
        message_type.value(), "scoped_in", dmw::Qos::ros2_default());
    assert(scoped);
    assert(scoped.value()->topic_name() == "/remap/scoped_out");
    auto unscoped_for_other = remap_node.value()->create_publisher(
        message_type.value(), "input", dmw::Qos::ros2_default());
    assert(unscoped_for_other);
    assert(unscoped_for_other.value()->topic_name() == "/remap/output");

    auto server = remap_node.value()->create_server(
        service_type, "old_service", dmw::Qos::ros2_services_default(), {});
    assert(server);
    assert(server.value()->service_name() == "/remap/new_service");

    // --- the remap is a wire-level remap: a peer using the target name matches
    dmw::NodeOptions peer_options;
    peer_options.node_name = "peer_node";
    peer_options.node_namespace = "/remap";
    peer_options.use_global_arguments = false;
    auto peer_node = rule_context.value()->create_node(peer_options);
    assert(peer_node);
    auto peer_subscriber = peer_node.value()->create_subscriber(
        message_type.value(), "output", dmw::Qos::ros2_default());
    assert(peer_subscriber);
    assert(peer_subscriber.value()->topic_name() == "/remap/output");
    auto peer_client = peer_node.value()->create_client(
        service_type, "new_service", dmw::Qos::ros2_services_default(), {});
    assert(peer_client);
    assert(peer_client.value()->service_name() == "/remap/new_service");
    wait_for_service(*peer_client.value());

    // --- a node-scoped rule for a matching node applies on that node ----------
    dmw::NodeOptions other_options;
    other_options.node_name = "other_node";
    other_options.node_namespace = "/remap";
    other_options.use_global_arguments = false;
    other_options.arguments = parse(remap_rules);
    auto other_node = rule_context.value()->create_node(other_options);
    assert(other_node);
    auto other_publisher = other_node.value()->create_publisher(
        message_type.value(), "input", dmw::Qos::ros2_default());
    assert(other_publisher);
    assert(other_publisher.value()->topic_name() == "/remap/wrong_output");

    // --- invalid remap rules surface as Errors, not silent success ------------
    dmw::NodeOptions invalid_options;
    invalid_options.node_name = "invalid_node";
    invalid_options.use_global_arguments = false;
    invalid_options.arguments = parse({"--ros-args", "-r", "bad/:=target", "--"});
    auto invalid_node = rule_context.value()->create_node(invalid_options);
    assert(invalid_node);
    const auto invalid_publisher = invalid_node.value()->create_publisher(
        message_type.value(), "ok_name", dmw::Qos::ros2_default());
    assert(!invalid_publisher);
    assert(invalid_publisher.error().code() == dmw::ErrorCode::InvalidName);

    assert(context.value()->shutdown());
    assert(rule_context.value()->shutdown());
    return 0;
}
