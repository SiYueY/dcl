#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fastdds/dds/topic/TopicDataType.hpp"

#include "dmw/action_client.hpp"
#include "dmw/action_common.hpp"
#include "dmw/action_server.hpp"
#include "dmw/action_type.hpp"
#include "dmw/client.hpp"
#include "dmw/context.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/graph.hpp"
#include "dmw/node.hpp"
#include "dmw/parameter.hpp"
#include "dmw/publisher.hpp"
#include "dmw/server.hpp"
#include "dmw/service_type.hpp"
#include "dmw/subscriber.hpp"
#include "dmw/timer.hpp"
#include "dmw/wait_set.hpp"

// This test is a consumer of the DMW public API: it implements the six
// language-layer patterns dclcpp/dclpy need using nothing but DMW, proving that
// a Client Library never has to rebuild common runtime capability.

namespace {

using namespace std::chrono_literals;

class IntTopicDataType final : public eprosima::fastdds::dds::TopicDataType {
public:
    IntTopicDataType() {
        m_typeSize = sizeof(int);
        setName("dmw.test.PrototypeInt");
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

/// (1) typed topic wrappers: presentation only, no runtime logic.
template <typename Sample>
class TypedPublisher {
public:
    explicit TypedPublisher(dmw::Publisher& publisher) noexcept : publisher_(publisher) {}
    dmw::Result<void> publish(const Sample& sample) const { return publisher_.write(&sample); }
private:
    dmw::Publisher& publisher_;
};

template <typename Sample>
class TypedSubscription {
public:
    explicit TypedSubscription(dmw::Subscriber& subscriber) noexcept : subscriber_(subscriber) {}
    dmw::Result<bool> take(Sample& sample, dmw::MessageInfo& info) const {
        return subscriber_.read(&sample, info);
    }
private:
    dmw::Subscriber& subscriber_;
};

/// (3) minimal executor: DMW WaitSet snapshot to language-layer callback.
class MiniExecutor {
public:
    explicit MiniExecutor(dmw::WaitSet& wait_set) noexcept : wait_set_(wait_set) {}
    template <typename Waitable>
    dmw::Result<dmw::WaitableRegistration> add(
        Waitable& waitable, std::function<void(const dmw::ReadyWaitable&)> handler) {
        auto registration = wait_set_.add(waitable);
        if (!registration) {
            return dmw::Result<dmw::WaitableRegistration>::failure(
                std::move(registration.error()));
        }
        bindings_.push_back(Binding{registration.value(), std::move(handler)});
        return registration;
    }
    bool spin_once(dmw::WaitTimeout timeout) {
        auto result = wait_set_.wait(timeout);
        assert(result);
        if (result.value().status() != dmw::WaitStatus::Ready) return false;
        bool handled = false;
        for (const auto& ready : result.value().ready()) {
            for (const auto& binding : bindings_) {
                if (!(binding.registration == ready.registration)) continue;
                binding.handler(ready);
                handled = true;
            }
        }
        return handled;
    }
private:
    struct Binding {
        dmw::WaitableRegistration registration;
        std::function<void(const dmw::ReadyWaitable&)> handler;
    };
    dmw::WaitSet& wait_set_;
    std::vector<Binding> bindings_;
};

/// (2) service RequestId to Future, the rclcpp/rclpy client shape.
template <typename Response>
class FutureServiceClient {
public:
    explicit FutureServiceClient(dmw::Client& client) noexcept : client_(client) {}
    std::future<Response> async_send_request(const void* request) {
        auto written = client_.write_request(request);
        std::promise<Response> promise;
        auto future = promise.get_future();
        if (!written) {
            promise.set_value(Response{});
            return future;
        }
        std::lock_guard lock(mutex_);
        pending_.emplace(written.value(), std::move(promise));
        return future;
    }
    void pump() {
        while (true) {
            Response response{};
            dmw::RequestId request_id;
            auto read = client_.read_response(&response, request_id);
            assert(read);
            if (!read.value()) return;
            std::promise<Response> promise;
            {
                std::lock_guard lock(mutex_);
                const auto found = pending_.find(request_id);
                if (found == pending_.end()) continue;
                promise = std::move(found->second);
                pending_.erase(found);
            }
            promise.set_value(response);
        }
    }
    std::size_t pending() const {
        std::lock_guard lock(mutex_);
        return pending_.size();
    }
private:
    dmw::Client& client_;
    mutable std::mutex mutex_;
    std::unordered_map<dmw::RequestId, std::promise<Response>, dmw::RequestIdHash> pending_;
};

/// (4) graph wrapper: language-facing container conversion only.
inline std::vector<std::string> node_names(dmw::Context& context) {
    std::vector<std::string> names;
    const auto snapshot = context.graph_snapshot();
    if (!snapshot) return names;
    for (const auto& node : snapshot.value().nodes) {
        names.push_back(
            node.node_namespace == "/" ? "/" + node.node_name
                                       : node.node_namespace + "/" + node.node_name);
    }
    return names;
}

inline std::vector<std::string> topic_names(dmw::Context& context) {
    std::vector<std::string> names;
    const auto snapshot = context.graph_snapshot();
    if (!snapshot) return names;
    for (const auto& topic : snapshot.value().topics) names.push_back(topic.topic_name);
    return names;
}

/// (5) parameter validate, user callback, atomic commit.
class ParameterNode {
public:
    explicit ParameterNode(dmw::Node& node) noexcept : node_(node) {}
    bool set_parameters(
        const std::vector<dmw::Parameter>& parameters,
        const std::function<bool(const std::vector<dmw::Parameter>&)>& on_set) {
        const auto validated = node_.validate_parameters(parameters);
        if (!validated) return false;
        if (!on_set(parameters)) return false;
        const auto committed = node_.set_parameters_atomically(parameters);
        return static_cast<bool>(committed);
    }
    dmw::Result<dmw::Parameter> get(std::string_view name) const {
        return node_.get_parameter(name);
    }
private:
    dmw::Node& node_;
};

/// (6) action GoalHandle/Future wrapper: goal id to result future.
class FutureActionClient {
public:
    explicit FutureActionClient(dmw::ActionClient& client) noexcept : client_(client) {}
    /// The typed adapter extracts the goal id from its own goal payload; this
    /// prototype uses the int payload itself as the goal id.
    std::future<int> async_send_goal(const void* goal, int goal_id) {
        auto written = client_.write_goal_request(goal);
        std::promise<int> promise;
        auto future = promise.get_future();
        if (!written) {
            promise.set_value(-1);
            return future;
        }
        std::lock_guard lock(mutex_);
        goal_requests_.emplace(written.value(), goal_id);
        return future;
    }
    void pump_goal_responses() {
        while (true) {
            int accepted = 0;
            dmw::RequestId request_id;
            auto read = client_.read_goal_response(&accepted, request_id);
            assert(read);
            if (!read.value()) return;
            std::lock_guard lock(mutex_);
            const auto found = goal_requests_.find(request_id);
            if (found == goal_requests_.end()) continue;
            if (accepted != 0) accepted_goals_.push_back(found->second);
            goal_requests_.erase(found);
        }
    }
    std::future<int> async_get_result(int goal_id) {
        std::promise<int> promise;
        auto future = promise.get_future();
        auto written = client_.write_result_request(&goal_id);
        if (!written) {
            promise.set_value(-1);
            return future;
        }
        std::lock_guard lock(mutex_);
        result_requests_.emplace(written.value(), std::move(promise));
        return future;
    }
    void pump_result_responses() {
        while (true) {
            int result = 0;
            dmw::RequestId request_id;
            auto read = client_.read_result_response(&result, request_id);
            assert(read);
            if (!read.value()) return;
            std::promise<int> promise;
            {
                std::lock_guard lock(mutex_);
                const auto found = result_requests_.find(request_id);
                if (found == result_requests_.end()) continue;
                promise = std::move(found->second);
                result_requests_.erase(found);
            }
            promise.set_value(result);
        }
    }
    std::vector<int> accepted_goals() const {
        std::lock_guard lock(mutex_);
        return accepted_goals_;
    }
private:
    dmw::ActionClient& client_;
    mutable std::mutex mutex_;
    std::unordered_map<dmw::RequestId, int, dmw::RequestIdHash> goal_requests_;
    std::unordered_map<dmw::RequestId, std::promise<int>, dmw::RequestIdHash> result_requests_;
    std::vector<int> accepted_goals_;
};

}  // namespace

int main() {
    if (std::getenv("DMW_ENABLE_DDS_INTEGRATION") == nullptr) return 0;
    const auto message_type = dmw::fastdds::create_message_type<IntTopicDataType, int>();
    assert(message_type);
    const dmw::ServiceType service_type(message_type.value(), message_type.value());
    const dmw::ActionType action_type(
        service_type, service_type, service_type, message_type.value(), message_type.value());

    dmw::ContextOptions server_options;
    server_options.domain_id = 150U;
    // ROS2 mode so the graph wrapper can see the peer Node identity, which is
    // published over the graph metadata transport.
    server_options.runtime_mode = dmw::RuntimeMode::ROS2;
    server_options.participant_name = "dmw-prototype-server";
    auto server_context = dmw::Context::create(server_options);
    assert(server_context);
    dmw::NodeOptions server_node_options;
    server_node_options.node_name = "prototype_server";
    auto server_node = server_context.value()->create_node(server_node_options);
    assert(server_node);
    auto publisher = server_node.value()->create_publisher(
        message_type.value(), "/prototype_topic", dmw::Qos::ros2_default());
    assert(publisher);
    auto server = server_node.value()->create_server(
        service_type, "/prototype_service", dmw::Qos::ros2_services_default(), {});
    assert(server);
    auto action_server = server_node.value()->create_action_server(
        action_type, "/prototype_action", dmw::ActionServerOptions{});
    assert(action_server);
    assert(server_node.value()->declare_parameter(
        "gain", dmw::ParameterValue::make_integer(1), dmw::ParameterDescriptor{}, false));

    dmw::ContextOptions client_options;
    client_options.domain_id = 150U;
    client_options.runtime_mode = dmw::RuntimeMode::ROS2;
    client_options.participant_name = "dmw-prototype-client";
    auto client_context = dmw::Context::create(client_options);
    assert(client_context);
    dmw::NodeOptions client_node_options;
    client_node_options.node_name = "prototype_client";
    auto client_node = client_context.value()->create_node(client_node_options);
    assert(client_node);
    auto subscriber = client_node.value()->create_subscriber(
        message_type.value(), "/prototype_topic", dmw::Qos::ros2_default());
    assert(subscriber);
    auto client = client_node.value()->create_client(
        service_type, "/prototype_service", dmw::Qos::ros2_services_default(), {});
    assert(client);
    auto action_client =
        client_node.value()->create_action_client(action_type, "/prototype_action");
    assert(action_client);

    auto wait_set = client_context.value()->create_wait_set();
    assert(wait_set);
    MiniExecutor executor(*wait_set.value());
    TypedPublisher<int> typed_publisher(*publisher.value());
    TypedSubscription<int> typed_subscription(*subscriber.value());
    FutureServiceClient<int> service_client(*client.value());
    FutureActionClient action_goal_client(*action_client.value());
    ParameterNode parameter_node(*server_node.value());

    // (1) typed topic.
    // A Client Library waits for the peer before its first volatile write.
    for (int attempt = 0; attempt < 500; ++attempt) {
        const auto matched = publisher.value()->matched_subscriber_count();
        assert(matched);
        if (matched.value() > 0) break;
        std::this_thread::sleep_for(10ms);
    }
    assert(publisher.value()->matched_subscriber_count().value() > 0);
    assert(typed_publisher.publish(41));
    std::vector<int> received;
    assert(executor.add(*subscriber.value(), [&](const dmw::ReadyWaitable&) {
        int sample = 0;
        dmw::MessageInfo info;
        const auto read = typed_subscription.take(sample, info);
        assert(read);
        if (read.value()) received.push_back(sample);
    }));

    // (2) service RequestId to Future.
    const auto availability_timeout = dmw::WaitTimeout::finite(15s);
    assert(availability_timeout);
    const auto service_available =
        client.value()->wait_for_service(availability_timeout.value());
    assert(service_available && service_available.value());
    assert(executor.add(*client.value(), [&](const dmw::ReadyWaitable&) {
        service_client.pump();
    }));

    // (3) timer to callback through the same executor.
    auto clock = client_context.value()->create_clock(dmw::ClockType::Steady);
    assert(clock);
    auto timer = client_context.value()->create_timer(*clock.value(), dmw::TimerOptions{5ms, true});
    assert(timer);
    int timer_callbacks = 0;
    assert(executor.add(*timer.value(), [&](const dmw::ReadyWaitable& ready) {
        assert(ready.detail_mask == dmw::kWaitableReadyBit);
        dmw::TimerInfo info;
        const auto consumed = timer.value()->consume(info);
        assert(consumed);
        if (consumed.value()) ++timer_callbacks;
    }));

    // (6) action goal and result futures, driven by the readiness detail mask.
    const auto action_available =
        action_client.value()->wait_for_server(availability_timeout.value());
    assert(action_available && action_available.value());
    assert(executor.add(*action_client.value(), [&](const dmw::ReadyWaitable& ready) {
        const auto detail = dmw::action_client_ready_set(ready.detail_mask);
        if (detail.goal_response) action_goal_client.pump_goal_responses();
        if (detail.result_response) action_goal_client.pump_result_responses();
    }));

    // Server side stays plain DMW too.
    auto server_wait_set = server_context.value()->create_wait_set();
    assert(server_wait_set);
    MiniExecutor server_executor(*server_wait_set.value());
    int service_request = 41;
    int goal_payload = 3;
    assert(server_executor.add(*server.value(), [&](const dmw::ReadyWaitable&) {
        dmw::RequestId request_id;
        const auto read = server.value()->read_request(&service_request, request_id);
        assert(read);
        if (!read.value()) return;
        const int response = service_request + 1;
        assert(server.value()->write_response(request_id, &response));
    }));
    std::map<int, int> goal_results;
    assert(server_executor.add(*action_server.value(), [&](const dmw::ReadyWaitable& ready) {
        const auto detail = dmw::action_server_ready_set(ready.detail_mask);
        if (detail.goal_request) {
            int goal = 0;
            dmw::RequestId request_id;
            const auto read = action_server.value()->read_goal_request(&goal, request_id);
            assert(read);
            if (!read.value()) return;
            dmw::GoalInfo goal_info;
            goal_info.goal_id.data[0] = static_cast<std::uint8_t>(goal);
            const int accepted = 1;
            const auto transition = action_server.value()->accept_goal(
                request_id, goal_info, &accepted, dmw::GoalAcceptMode::Execute);
            assert(transition);
            assert(action_server.value()->update_goal_state(
                goal_info.goal_id, dmw::GoalEvent::Succeed));
            goal_results.emplace(goal, goal * 10);
        }
        if (detail.result_request) {
            int goal = 0;
            dmw::RequestId request_id;
            const auto read = action_server.value()->read_result_request(&goal, request_id);
            assert(read);
            if (!read.value()) return;
            dmw::GoalId goal_id;
            goal_id.data[0] = static_cast<std::uint8_t>(goal);
            const auto disposition =
                action_server.value()->register_result_request(goal_id, request_id);
            assert(disposition);
            if (disposition.value() == dmw::ResultRequestDisposition::Terminal) {
                const int result = goal_results.count(goal) != 0 ? goal_results[goal] : -1;
                assert(action_server.value()->write_result_response(request_id, &result));
            }
        }
    }));

    auto service_future = service_client.async_send_request(&service_request);
    assert(service_client.pending() == 1);
    auto goal_future = action_goal_client.async_send_goal(&goal_payload, 3);
    assert(goal_future.valid());

    const auto deadline = std::chrono::steady_clock::now() + 15s;
    const auto spin_timeout = dmw::WaitTimeout::finite(10ms);
    assert(spin_timeout);
    while (std::chrono::steady_clock::now() < deadline) {
        server_executor.spin_once(dmw::WaitTimeout::poll());
        executor.spin_once(spin_timeout.value());
        const bool ready = !received.empty() &&
                           service_future.wait_for(0ms) == std::future_status::ready &&
                           timer_callbacks > 0;
        if (ready) break;
    }
    assert(!received.empty() && received.front() == 41);
    assert(service_future.wait_for(0ms) == std::future_status::ready);
    assert(service_future.get() == 42);
    assert(timer_callbacks > 0);

    const auto accepted_goals = action_goal_client.accepted_goals();
    assert(accepted_goals.size() == 1);
    assert(accepted_goals.front() == 3);
    auto result_future = action_goal_client.async_get_result(3);
    const auto result_deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < result_deadline &&
           result_future.wait_for(0ms) != std::future_status::ready) {
        server_executor.spin_once(spin_timeout.value());
        executor.spin_once(spin_timeout.value());
    }
    assert(result_future.wait_for(0ms) == std::future_status::ready);
    assert(result_future.get() == 30);

    // (5) parameter validate, user callback, atomic commit.
    bool callback_ran = false;
    const std::vector<dmw::Parameter> rejected{
        dmw::Parameter{"gain", dmw::ParameterValue::make_integer(9)}};
    assert(!parameter_node.set_parameters(rejected, [&](const std::vector<dmw::Parameter>&) {
        callback_ran = true;
        return false;
    }));
    assert(callback_ran);
    assert(parameter_node.get("gain").value().value.as_integer() == 1);
    const std::vector<dmw::Parameter> accepted_parameters{
        dmw::Parameter{"gain", dmw::ParameterValue::make_integer(9)}};
    assert(parameter_node.set_parameters(
        accepted_parameters, [](const std::vector<dmw::Parameter>&) { return true; }));
    assert(parameter_node.get("gain").value().value.as_integer() == 9);
    const auto changes = server_node.value()->take_parameter_changes();
    assert(changes);
    assert(changes.value().changed_parameters.size() == 1);

    // (4) graph wrapper sees the peer node (learned over graph metadata) and the
    // user topics, never the internal metadata transport.
    std::vector<std::string> nodes;
    std::vector<std::string> topics;
    const auto node_deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < node_deadline) {
        nodes = node_names(*client_context.value());
        if (std::find(nodes.begin(), nodes.end(), "/prototype_server") != nodes.end()) break;
        std::this_thread::sleep_for(20ms);
    }
    assert(std::find(nodes.begin(), nodes.end(), "/prototype_server") != nodes.end());
    assert(std::find(nodes.begin(), nodes.end(), "/prototype_client") != nodes.end());
    topics = topic_names(*client_context.value());
    assert(std::find(topics.begin(), topics.end(), "/prototype_topic") != topics.end());
    assert(std::find(topics.begin(), topics.end(), "/ros_discovery_info") == topics.end());
    assert(std::find(topics.begin(), topics.end(), "ros_discovery_info") == topics.end());

    assert(client_context.value()->shutdown());
    assert(server_context.value()->shutdown());
    return 0;
}
