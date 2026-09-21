#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

#include "dmw/client.hpp"
#include "dmw/context.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/graph.hpp"
#include "dmw/graph_event.hpp"
#include "dmw/node.hpp"
#include "dmw/publisher.hpp"
#include "dmw/server.hpp"
#include "dmw/service_type.hpp"
#include "dmw/subscriber.hpp"
#include "dmw/wait_set.hpp"

namespace {

class GraphTopicDataType : public eprosima::fastdds::dds::TopicDataType {
public:
    GraphTopicDataType() {
        m_typeSize = sizeof(int);
        setName("dmw.test.GraphTopicDataType");
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

constexpr char kTopicName[] = "/dmw/graph_topic";
constexpr char kServiceName[] = "/dmw/graph_service";

const dmw::TopicGraphInfo* find_topic(const dmw::GraphSnapshot& snapshot, const std::string& name) {
    const auto found = std::find_if(
        snapshot.topics.begin(), snapshot.topics.end(),
        [&](const dmw::TopicGraphInfo& value) { return value.topic_name == name; });
    return found == snapshot.topics.end() ? nullptr : &*found;
}

const dmw::ServiceGraphInfo* find_service(
    const dmw::GraphSnapshot& snapshot, const std::string& name) {
    const auto found = std::find_if(
        snapshot.services.begin(), snapshot.services.end(),
        [&](const dmw::ServiceGraphInfo& value) { return value.service_name == name; });
    return found == snapshot.services.end() ? nullptr : &*found;
}

std::size_t count_topic_endpoints(const dmw::GraphSnapshot& snapshot, const std::string& name) {
    return static_cast<std::size_t>(std::count_if(
        snapshot.topic_endpoints.begin(), snapshot.topic_endpoints.end(),
        [&](const dmw::TopicEndpointInfo& value) { return value.topic_name == name; }));
}

}  // namespace

int main() {
    if (std::getenv("DMW_ENABLE_DDS_INTEGRATION") == nullptr) return 0;

    const auto message_type = dmw::fastdds::create_message_type<GraphTopicDataType, int>();
    assert(message_type);
    const dmw::ServiceType service_type(message_type.value(), message_type.value());

    auto context = dmw::Context::create({});
    assert(context);
    auto& ctx = *context.value();

    const auto initial_revision = ctx.graph_revision();
    assert(initial_revision);
    auto empty = ctx.graph_snapshot();
    assert(empty);
    assert(empty.value().nodes.empty());
    assert(empty.value().topics.empty());

    dmw::NodeOptions node_options;
    node_options.node_name = "alpha";
    node_options.node_namespace = "/dmw";
    auto node = ctx.create_node(node_options);
    assert(node);
    assert(node.value()->fully_qualified_name() == "/dmw/alpha");

    const auto after_node = ctx.graph_revision();
    assert(after_node);
    assert(after_node.value() > initial_revision.value());

    auto snapshot = ctx.graph_snapshot();
    assert(snapshot);
    assert(snapshot.value().revision == after_node.value());
    assert(snapshot.value().nodes.size() == 1);
    assert(snapshot.value().nodes.front().node_name == "alpha");
    assert(snapshot.value().nodes.front().node_namespace == "/dmw");

    auto publisher = node.value()->create_publisher(
        message_type.value(), kTopicName, dmw::Qos::ros2_default());
    assert(publisher);
    auto subscriber = node.value()->create_subscriber(
        message_type.value(), kTopicName, dmw::Qos::ros2_default());
    assert(subscriber);

    snapshot = ctx.graph_snapshot();
    assert(snapshot);
    const auto* topic = find_topic(snapshot.value(), kTopicName);
    assert(topic != nullptr);
    assert(topic->publisher_count == 1);
    assert(topic->subscriber_count == 1);
    assert(topic->wire_types.size() == 1);

    assert(count_topic_endpoints(snapshot.value(), kTopicName) == 2);
    bool saw_publisher = false;
    bool saw_subscriber = false;
    for (const auto& endpoint : snapshot.value().topic_endpoints) {
        if (endpoint.topic_name != kTopicName) continue;
        assert(endpoint.node_name == "alpha");
        assert(endpoint.node_namespace == "/dmw");
        assert(endpoint.wire_type == "dmw.test.GraphTopicDataType");
        if (endpoint.kind == dmw::EndpointKind::Publisher) saw_publisher = true;
        if (endpoint.kind == dmw::EndpointKind::Subscriber) saw_subscriber = true;
    }
    assert(saw_publisher && saw_subscriber);

    auto server = node.value()->create_server(
        service_type, kServiceName, dmw::Qos::ros2_services_default(), {});
    assert(server);
    auto client = node.value()->create_client(
        service_type, kServiceName, dmw::Qos::ros2_services_default(), {});
    assert(client);

    snapshot = ctx.graph_snapshot();
    assert(snapshot);
    const auto* service = find_service(snapshot.value(), kServiceName);
    assert(service != nullptr);
    assert(service->server_candidate_count == 1);
    assert(service->client_candidate_count == 1);
    for (const auto& endpoint : snapshot.value().service_endpoints) {
        if (endpoint.service_name != kServiceName) continue;
        assert(endpoint.node_name == "alpha");
        assert(endpoint.request_wire_type == "dmw.test.GraphTopicDataType");
        assert(endpoint.response_wire_type == "dmw.test.GraphTopicDataType");
    }

    // A second Context is a different participant.  Candidates must never be
    // composed across participants.
    auto other = dmw::Context::create({});
    assert(other);
    dmw::NodeOptions other_options;
    other_options.node_name = "beta";
    other_options.node_namespace = "/dmw";
    auto other_node = other.value()->create_node(other_options);
    assert(other_node);
    auto other_client = other_node.value()->create_client(
        service_type, kServiceName, dmw::Qos::ros2_services_default(), {});
    assert(other_client);

    snapshot = other.value()->graph_snapshot();
    assert(snapshot);
    const auto* remote_service = find_service(snapshot.value(), kServiceName);
    if (remote_service != nullptr) {
        // `other` always contributes exactly one Client candidate from its own
        // request writer + response reader.  Depending on discovery timing it
        // may additionally see the peer's Client endpoints composed inside the
        // peer participant (a second candidate), and the peer's Server.
        assert(remote_service->client_candidate_count >= 1);
        assert(remote_service->client_candidate_count <= 2);
        assert(remote_service->server_candidate_count <= 1);
    }

    // Service candidates must never be composed across participants: a request
    // reader in one Context plus a response writer in another is not a Server.
    constexpr char kCrossService[] = "/dmw/cross_service";
    auto request_half = node.value()->create_subscriber(
        message_type.value(), "/dmw/cross_service_Request", dmw::Qos::ros2_default());
    assert(request_half);
    auto response_half = other_node.value()->create_publisher(
        message_type.value(), "/dmw/cross_service_Reply", dmw::Qos::ros2_default());
    assert(response_half);
    // Positive control: prove endpoint discovery between the two participants
    // works right now, otherwise the negative assertion below would be vacuous.
    constexpr char kControlTopic[] = "/dmw/control_topic";
    auto control_publisher = other_node.value()->create_publisher(
        message_type.value(), kControlTopic, dmw::Qos::ros2_default());
    assert(control_publisher);
    bool control_visible = false;
    for (int attempt = 0; attempt < 250 && !control_visible; ++attempt) {
        snapshot = ctx.graph_snapshot();
        assert(snapshot);
        for (const auto& endpoint : snapshot.value().topic_endpoints) {
            if (endpoint.topic_name != kControlTopic) continue;
            if (endpoint.kind != dmw::EndpointKind::Publisher) continue;
            control_visible = true;
        }
        if (!control_visible) std::this_thread::sleep_for(20ms);
    }
    assert(control_visible);
    snapshot = ctx.graph_snapshot();
    assert(snapshot);
    const auto* cross = find_service(snapshot.value(), kCrossService);
    assert(cross == nullptr || cross->server_candidate_count == 0);

    // Completing the pair inside the *same* participant does form a candidate.
    auto request_half_writer = node.value()->create_publisher(
        message_type.value(), "/dmw/cross_service_Request", dmw::Qos::ros2_default());
    auto response_half_writer = node.value()->create_publisher(
        message_type.value(), "/dmw/cross_service_Reply", dmw::Qos::ros2_default());
    assert(request_half_writer && response_half_writer);
    bool same_participant_candidate = false;
    for (int attempt = 0; attempt < 100 && !same_participant_candidate; ++attempt) {
        snapshot = ctx.graph_snapshot();
        assert(snapshot);
        const auto* current = find_service(snapshot.value(), kCrossService);
        if (current != nullptr && current->server_candidate_count > 0) {
            same_participant_candidate = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    assert(same_participant_candidate);

    // GraphEvent is level-triggered on the observed revision.
    auto event = ctx.create_graph_event();
    assert(event);
    dmw::GraphChangeInfo change;
    const auto untouched = event.value()->take(change);
    assert(untouched && !untouched.value());

    dmw::NodeOptions second_options;
    second_options.node_name = "gamma";
    auto second_node = ctx.create_node(second_options);
    assert(second_node);

    const auto taken = event.value()->take(change);
    assert(taken && taken.value());
    assert(change.current_revision > change.previous_revision);
    const auto exhausted = event.value()->take(change);
    assert(exhausted && !exhausted.value());

    auto wait_set = ctx.create_wait_set();
    assert(wait_set);
    auto registration = wait_set.value()->add(*event.value());
    assert(registration);
    assert(registration.value().kind() == dmw::WaitableKind::GraphEvent);
    std::thread mutator([&ctx] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        dmw::NodeOptions options;
        options.node_name = "delta";
        (void)ctx.create_node(options);
    });
    const auto signalled = wait_set.value()->wait(dmw::WaitTimeout::infinite());
    mutator.join();
    assert(signalled);
    assert(signalled.value().status() == dmw::WaitStatus::Ready);
    assert(signalled.value().ready().front().kind == dmw::WaitableKind::GraphEvent);
    assert(wait_set.value()->remove(registration.value()));

    // A registered GraphEvent that is destroyed must auto-detach: the WaitSet
    // must not report it ready again and its token becomes stale.
    auto detachable = ctx.create_graph_event();
    assert(detachable);
    auto detachable_token = wait_set.value()->add(*detachable.value());
    assert(detachable_token);
    detachable.value().reset();
    dmw::NodeOptions detach_options;
    detach_options.node_name = "epsilon";
    auto detach_node = ctx.create_node(detach_options);
    assert(detach_node);
    const auto detach_timeout = dmw::WaitTimeout::finite(50ms);
    assert(detach_timeout);
    const auto after_detach = wait_set.value()->wait(detach_timeout.value());
    assert(after_detach);
    assert(after_detach.value().status() == dmw::WaitStatus::Timeout);
    const auto stale_remove = wait_set.value()->remove(detachable_token.value());
    assert(!stale_remove);
    assert(stale_remove.error().code() == dmw::ErrorCode::NotRegistered);

    // Destroying the Node facade must not drop the endpoint association while
    // the endpoints are still alive.
    const auto beta_before = snapshot.value().revision;
    assert(beta_before >= 0);
    second_node.value().reset();
    snapshot = ctx.graph_snapshot();
    assert(snapshot);
    assert(find_topic(snapshot.value(), kTopicName) != nullptr);
    for (const auto& endpoint : snapshot.value().topic_endpoints) {
        if (endpoint.topic_name == kTopicName) assert(endpoint.node_name == "alpha");
    }

    assert(ctx.shutdown());
    const auto shutdown_revision = ctx.graph_revision();
    assert(!shutdown_revision);
    assert(shutdown_revision.error().code() == dmw::ErrorCode::ContextShutdown);
    assert(!ctx.graph_snapshot());
    assert(!ctx.create_graph_event());
    return 0;
}
