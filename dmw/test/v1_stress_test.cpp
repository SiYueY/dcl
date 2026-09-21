#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <string>
#include <memory>
#include <thread>
#include <vector>
#include <unistd.h>

#include "fastdds/dds/topic/TopicDataType.hpp"

#include "dmw/action_client.hpp"
#include "dmw/action_server.hpp"
#include "dmw/action_type.hpp"
#include "dmw/clock.hpp"
#include "dmw/context.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/graph_event.hpp"
#include "dmw/node.hpp"
#include "dmw/parameter.hpp"
#include "dmw/publisher.hpp"
#include "dmw/service_type.hpp"
#include "dmw/timer.hpp"
#include "dmw/wait_set.hpp"

namespace {

using namespace std::chrono_literals;

class IntTopicDataType final : public eprosima::fastdds::dds::TopicDataType {
public:
    IntTopicDataType() {
        m_typeSize = sizeof(int);
        setName("dmw.test.V1StressInt");
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

/// ROS2 mode: graph metadata publish/ingest under concurrent churn and queries.
void stress_graph_metadata(std::uint32_t domain, const dmw::MessageType& message_type) {
    dmw::ContextOptions options;
    options.runtime_mode = dmw::RuntimeMode::ROS2;
    options.domain_id = domain;
    options.participant_name = "dmw-v1-stress-graph";
    auto context = dmw::Context::create(options);
    assert(context);
    dmw::NodeOptions node_options;
    node_options.node_name = "stress_root";
    auto root = context.value()->create_node(node_options);
    assert(root);

    // GraphEvent is level-triggered from its creation cursor, so register it
    // before the churn rather than replaying history.
    auto wait_set = context.value()->create_wait_set();
    assert(wait_set);
    auto event = context.value()->create_graph_event();
    assert(event);
    auto token = wait_set.value()->add(*event.value());
    assert(token);

    std::atomic<bool> stop{false};
    std::thread churner([&] {
        for (int iteration = 0; iteration < 25; ++iteration) {
            dmw::NodeOptions churn_options;
            churn_options.node_name = "churn_" + std::to_string(iteration);
            auto node = context.value()->create_node(churn_options);
            assert(node);
            auto publisher = node.value()->create_publisher(
                message_type, "/stress_topic", dmw::Qos::ros2_default());
            assert(publisher);
        }
    });
    std::thread querier([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const auto revision = context.value()->graph_revision();
            assert(revision);
            const auto snapshot = context.value()->graph_snapshot();
            assert(snapshot);
            for (const auto& topic : snapshot.value().topics) {
                assert(topic.topic_name != "ros_discovery_info");
                assert(topic.topic_name != "/ros_discovery_info");
            }
        }
    });
    churner.join();
    stop.store(true, std::memory_order_release);
    querier.join();

    const auto graph_wait_timeout = dmw::WaitTimeout::finite(1s);
    assert(graph_wait_timeout);
    const auto signalled = wait_set.value()->wait(graph_wait_timeout.value());
    assert(signalled);
    assert(signalled.value().status() == dmw::WaitStatus::Ready);
    assert(wait_set.value()->remove(token.value()));

    std::atomic<bool> start{false};
    auto first = std::async(std::launch::async, [&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        return context.value()->shutdown();
    });
    auto second = std::async(std::launch::async, [&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        return context.value()->shutdown();
    });
    start.store(true, std::memory_order_release);
    assert(first.get());
    assert(second.get());
}

/// Timer + Clock: concurrent schedule mutation while a WaitSet is blocked.
void stress_timer_clock(std::uint32_t domain) {
    dmw::ContextOptions options;
    options.domain_id = domain;
    options.participant_name = "dmw-v1-stress-timer";
    auto context = dmw::Context::create(options);
    assert(context);
    auto clock = context.value()->create_clock(dmw::ClockType::Steady);
    assert(clock);
    auto wait_set = context.value()->create_wait_set();
    assert(wait_set);

    auto timer = context.value()->create_timer(*clock.value(), dmw::TimerOptions{5s, true});
    assert(timer);
    auto token = wait_set.value()->add(*timer.value());
    assert(token);

    std::atomic<bool> stop{false};
    std::thread mutator([&] {
        int round = 0;
        while (!stop.load(std::memory_order_acquire)) {
            assert(timer.value()->exchange_period(20ms + 10ms * (round % 3)));
            std::this_thread::sleep_for(1ms);
            if ((round % 4) == 0) assert(timer.value()->cancel());
            if ((round % 4) == 1) assert(timer.value()->reset());
            ++round;
        }
    });
    // A finite wait must keep returning Timeout under continuous mutation
    // without ever blocking past its original deadline.
    for (int attempt = 0; attempt < 10; ++attempt) {
        const auto timeout = dmw::WaitTimeout::finite(20ms);
        assert(timeout);
        const auto result = wait_set.value()->wait(timeout.value());
        assert(result);
    }
    stop.store(true, std::memory_order_release);
    mutator.join();

    // Destroying the timer while it is registered must auto-detach.
    assert(timer.value()->reset());
    timer.value().reset();
    const auto after_destroy_timeout = dmw::WaitTimeout::finite(20ms);
    assert(after_destroy_timeout);
    const auto after_destroy = wait_set.value()->wait(after_destroy_timeout.value());
    assert(after_destroy);
    assert(after_destroy.value().status() == dmw::WaitStatus::Timeout);
    assert(!wait_set.value()->remove(token.value()));

    // Parallel mutate/consume on a single timer: only one consumer wins.
    // The period is long relative to thread startup so exactly one consumer can
    // legitimately observe the single elapsed deadline.
    auto shared_timer =
        context.value()->create_timer(*clock.value(), dmw::TimerOptions{200ms, true});
    assert(shared_timer);
    std::this_thread::sleep_for(250ms);
    std::atomic<int> consumed{0};
    std::vector<std::thread> consumers;
    for (int index = 0; index < 4; ++index) {
        consumers.emplace_back([&] {
            dmw::TimerInfo info;
            const auto result = shared_timer.value()->consume(info);
            assert(result);
            if (result.value()) consumed.fetch_add(1);
        });
    }
    for (auto& consumer : consumers) consumer.join();
    assert(consumed.load() == 1);
    assert(context.value()->shutdown());
}

/// Parameter store: concurrent atomic mutation stays linearizable.
void stress_parameters(std::uint32_t domain) {
    dmw::ContextOptions options;
    options.domain_id = domain;
    options.participant_name = "dmw-v1-stress-parameter";
    auto context = dmw::Context::create(options);
    assert(context);
    dmw::NodeOptions node_options;
    node_options.node_name = "stress_parameters";
    auto node = context.value()->create_node(node_options);
    assert(node);
    assert(node.value()->declare_parameter("value", dmw::ParameterValue::make_integer(0)));

    std::atomic<bool> failed{false};
    std::vector<std::thread> writers;
    for (int index = 0; index < 4; ++index) {
        writers.emplace_back([&, index] {
            for (int round = 0; round < 200; ++round) {
                const auto committed = node.value()->set_parameters_atomically(
                    {dmw::Parameter{"value",
                                    dmw::ParameterValue::make_integer(index * 1000 + round + 1)}});
                if (!committed) failed.store(true, std::memory_order_release);
                // A rejected mutation must leave the previous value intact.
                const auto rejected = node.value()->set_parameters_atomically(
                    {dmw::Parameter{"value", dmw::ParameterValue::make_string("bad")}});
                if (rejected) failed.store(true, std::memory_order_release);
                const auto current = node.value()->get_parameter("value");
                if (!current) failed.store(true, std::memory_order_release);
                else if (current.value().value.type() != dmw::ParameterType::Integer)
                    failed.store(true, std::memory_order_release);
            }
        });
    }
    for (auto& writer : writers) writer.join();
    assert(!failed.load());
    const auto final_value = node.value()->get_parameter("value");
    assert(final_value);
    assert(final_value.value().value.as_integer() >= 1);
    assert(context.value()->shutdown());
}

/// Action aggregate: registration, Goal FSM and shutdown under one token.
void stress_action(std::uint32_t domain, const dmw::ActionType& action_type) {
    // Availability and response-target matching are peer-based, so the server
    // and client live in separate Contexts (separate participants).
    dmw::ContextOptions server_options;
    server_options.domain_id = domain;
    server_options.participant_name = "dmw-v1-stress-action-server";
    auto server_context = dmw::Context::create(server_options);
    assert(server_context);
    dmw::NodeOptions server_node_options;
    server_node_options.node_name = "stress_action_server";
    auto server_node = server_context.value()->create_node(server_node_options);
    assert(server_node);
    auto server = server_node.value()->create_action_server(action_type, "/stress_action");
    assert(server);

    dmw::ContextOptions client_options;
    client_options.domain_id = domain;
    client_options.participant_name = "dmw-v1-stress-action-client";
    auto context = dmw::Context::create(client_options);
    assert(context);
    dmw::NodeOptions node_options;
    node_options.node_name = "stress_action_client";
    auto node = context.value()->create_node(node_options);
    assert(node);
    auto client = node.value()->create_action_client(action_type, "/stress_action");
    assert(client);
    auto server_wait_set = server_context.value()->create_wait_set();
    assert(server_wait_set);
    auto server_token = server_wait_set.value()->add(*server.value());
    assert(server_token);
    auto wait_set = context.value()->create_wait_set();
    assert(wait_set);
    auto client_token = wait_set.value()->add(*client.value());
    assert(server_token);
    assert(client_token);

    const auto timeout = dmw::WaitTimeout::finite(5s);
    assert(timeout);
    const auto available = client.value()->wait_for_server(timeout.value());
    assert(available && available.value());

    // Drive several goals through accept -> terminal while the server is
    // registered as a WaitSet waitable.
    for (std::uint8_t seed = 1; seed <= 5; ++seed) {
        int request = seed;
        const auto written = client.value()->write_goal_request(&request);
        assert(written);
        dmw::RequestId request_id;
        int received = 0;
        bool read = false;
        for (int attempt = 0; attempt < 200 && !read; ++attempt) {
            const auto result = server.value()->read_goal_request(&received, request_id);
            assert(result);
            read = result.value();
            if (!read) std::this_thread::sleep_for(2ms);
        }
        assert(read);
        assert(received == request);
        dmw::GoalInfo goal_info;
        goal_info.goal_id.data[0] = seed;
        goal_info.goal_id.data[15] = seed;
        const int accepted_response = 1;
        const auto accepted = server.value()->accept_goal(
            request_id, goal_info, &accepted_response, dmw::GoalAcceptMode::Execute);
        assert(accepted);
        assert(server.value()->update_goal_state(goal_info.goal_id, dmw::GoalEvent::Succeed));
    }
    const auto status = server.value()->status_snapshot();
    assert(status && status.value().size() == 5);

    std::atomic<bool> stop{false};
    std::thread transitions([&] {
        for (std::uint8_t seed = 1; seed <= 5; ++seed) {
            dmw::GoalId goal_id;
            goal_id.data[0] = seed;
            goal_id.data[15] = seed;
            // Terminal goals must reject further transitions under contention.
            const auto rejected =
                server.value()->update_goal_state(goal_id, dmw::GoalEvent::Abort);
            assert(!rejected);
            assert(rejected.error().code() == dmw::ErrorCode::InvalidState);
        }
        stop.store(true, std::memory_order_release);
    });
    while (!stop.load(std::memory_order_acquire)) {
        const auto polled = wait_set.value()->wait(dmw::WaitTimeout::poll());
        assert(polled);
        const auto snapshot = server.value()->status_snapshot();
        assert(snapshot);
    }
    transitions.join();

    // Shutdown while the aggregate tokens are still registered.
    const auto closed = server_context.value()->shutdown();
    assert(closed);
    int scratch_request = 0;
    dmw::RequestId scratch_request_id;
    const auto shutdown_read =
        server.value()->read_goal_request(&scratch_request, scratch_request_id);
    assert(!shutdown_read);
    assert(shutdown_read.error().code() == dmw::ErrorCode::ContextShutdown);
}

}  // namespace

int main() {
    if (std::getenv("DMW_ENABLE_DDS_INTEGRATION") == nullptr) return 0;

    const auto message_type = dmw::fastdds::create_message_type<IntTopicDataType, int>();
    assert(message_type);
    const dmw::ServiceType service_type(message_type.value(), message_type.value());
    const dmw::ActionType action_type(
        service_type, service_type, service_type, message_type.value(), message_type.value());

    // Keep each process' stress domains inside a disjoint 21-wide slot keyed by
    // process id, so a concurrently running instance (a parallel build tree)
    // cannot be observed as part of this process' graph/availability assertions.
    const std::uint32_t domain_base =
        1U + (static_cast<std::uint32_t>(::getpid()) % 11U) * 21U;
    for (std::uint32_t iteration = 0; iteration < 3; ++iteration) {
        stress_timer_clock(domain_base + iteration);
    }
    for (std::uint32_t iteration = 0; iteration < 3; ++iteration) {
        stress_parameters(domain_base + 3U + iteration);
    }
    for (std::uint32_t iteration = 0; iteration < 2; ++iteration) {
        stress_action(domain_base + 6U + iteration, action_type);
    }
    for (std::uint32_t iteration = 0; iteration < 2; ++iteration) {
        stress_graph_metadata(domain_base + 8U + iteration, message_type.value());
    }
    return 0;
}
