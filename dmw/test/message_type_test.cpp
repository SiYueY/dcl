// SPDX-License-Identifier: Apache-2.0

#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <typeindex>
#include <thread>

#include "dmw/client.hpp"
#include "dmw/context.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/publisher.hpp"
#include "dmw/server.hpp"
#include "dmw/subscriber.hpp"

namespace {

class NamedTopicDataType : public eprosima::fastdds::dds::TopicDataType {
public:
    NamedTopicDataType() {
        m_typeSize = sizeof(int);
        setName("dmw.test.NamedTopicDataType");
    }

    explicit NamedTopicDataType(const char* name) {
        m_typeSize = sizeof(int);
        setName(name);
    }

    bool serialize(void* data, eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
        payload->length = sizeof(int);
        std::memcpy(payload->data, data, sizeof(int));
        return true;
    }

    bool deserialize(eprosima::fastrtps::rtps::SerializedPayload_t* payload, void* data) override {
        if (payload->length != sizeof(int)) {
            return false;
        }
        std::memcpy(data, payload->data, sizeof(int));
        return true;
    }

    std::function<std::uint32_t()> getSerializedSizeProvider(void*) override {
        return [] { return static_cast<std::uint32_t>(sizeof(int)); };
    }

    void* createData() override { return new int(0); }

    void deleteData(void* data) override { delete static_cast<int*>(data); }

    bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override { return false; }
};

class EmptyNameTopicDataType : public NamedTopicDataType {
public:
    EmptyNameTopicDataType() { setName(""); }
};

class CollidingTopicDataType : public NamedTopicDataType {
public:
    CollidingTopicDataType() : NamedTopicDataType("dmw.test.NamedTopicDataType") {}
};

static_assert(
    std::is_same<
        decltype(dmw::ContextOptions::compatibility_profile), dmw::CompatibilityProfile>::value,
    "ContextOptions must own CompatibilityProfile");
static_assert(
    std::is_empty<dmw::PublisherOptions>::value, "PublisherOptions must not override profile");
static_assert(
    std::is_empty<dmw::SubscriberOptions>::value, "SubscriberOptions must not override profile");
static_assert(std::is_empty<dmw::ClientOptions>::value, "ClientOptions must not override profile");

}  // namespace

int main() {
    auto binding_smoke = dmw::fastdds::make_message_type<NamedTopicDataType>();
    assert(binding_smoke);
    assert(binding_smoke.value().type_name() == "dmw.test.NamedTopicDataType");

    if (std::getenv("DMW_ENABLE_DDS_INTEGRATION") == nullptr) return 0;

    dmw::ContextOptions context_options;
    context_options.participant_name = "dmw-message-type-test";
    auto context = dmw::Context::create(context_options);
    assert(context);
    assert(!context.value()->is_shutdown());
    assert(context.value()->domain_id() == 0);

    dmw::NodeOptions node_options;
    node_options.name = "message_type_test";
    node_options.ns = "/dmw";
    auto node = context.value()->create_node(node_options);
    assert(node);
    assert(node.value()->name() == "message_type_test");
    assert(node.value()->namespace_() == "/dmw");

    auto wait_set = context.value()->create_wait_set();
    auto guard = context.value()->create_guard_condition();
    assert(wait_set);
    assert(guard);
    auto guard_token = wait_set.value()->add(*guard.value());
    assert(guard_token);
    assert(
        wait_set.value()->wait(dmw::WaitTimeout::poll()).value().status() ==
        dmw::WaitStatus::Timeout);
    assert(guard.value()->trigger());
    assert(guard.value()->trigger());
    auto guard_ready = wait_set.value()->wait(dmw::WaitTimeout::poll());
    assert(guard_ready);
    assert(guard_ready.value().status() == dmw::WaitStatus::Ready);
    assert(guard_ready.value().ready().size() == 1);
    assert(guard_ready.value().ready().front() == guard_token.value());
    assert(wait_set.value()->wait(dmw::WaitTimeout::poll()).value().status() ==
           dmw::WaitStatus::Timeout);
    assert(wait_set.value()->remove(guard_token.value()));

    auto auto_detach_guard = context.value()->create_guard_condition();
    assert(auto_detach_guard);
    auto auto_detach_token = wait_set.value()->add(*auto_detach_guard.value());
    assert(auto_detach_token);
    auto_detach_guard.value().reset();
    auto detached_wait = wait_set.value()->wait(dmw::WaitTimeout::poll());
    assert(detached_wait);
    assert(detached_wait.value().status() == dmw::WaitStatus::Timeout);
    auto stale_remove = wait_set.value()->remove(auto_detach_token.value());
    assert(!stale_remove);
    assert(stale_remove.error().code() == dmw::ErrorCode::NotRegistered);

    auto reusable_guard = context.value()->create_guard_condition();
    auto first_wait_set = context.value()->create_wait_set();
    assert(reusable_guard);
    assert(first_wait_set);
    assert(first_wait_set.value()->add(*reusable_guard.value()));
    first_wait_set.value().reset();
    auto second_wait_set = context.value()->create_wait_set();
    assert(second_wait_set);
    assert(second_wait_set.value()->add(*reusable_guard.value()));

    eprosima::fastdds::dds::TypeSupport type_support(new NamedTopicDataType("dmw.test.Initial"));
    auto result = dmw::fastdds::MessageTypeAccess::create(type_support, typeid(NamedTopicDataType));
    assert(result);
    assert(result.value().type_name() == "dmw.test.Initial");

    type_support->setName("dmw.test.Mutated");
    assert(result.value().type_name() == "dmw.test.Initial");

    auto generated = dmw::fastdds::make_message_type<NamedTopicDataType>();
    assert(generated);
    assert(generated.value().type_name() == "dmw.test.NamedTopicDataType");

    {
        dmw::ContextOptions writer_context_options;
        writer_context_options.participant_name = "dmw-cross-context-writer";
        auto writer_context = dmw::Context::create(writer_context_options);
        assert(writer_context);
        dmw::ContextOptions reader_context_options;
        reader_context_options.participant_name = "dmw-cross-context-reader";
        auto reader_context = dmw::Context::create(reader_context_options);
        assert(reader_context);

        auto writer_node = writer_context.value()->create_node(node_options);
        auto reader_node = reader_context.value()->create_node(node_options);
        assert(writer_node);
        assert(reader_node);
        auto cross_context_publisher =
            writer_node.value()->create_publisher(generated.value(), "cross_context", dmw::Qos{});
        auto cross_context_subscriber =
            reader_node.value()->create_subscriber(generated.value(), "cross_context", dmw::Qos{});
        assert(cross_context_publisher);
        assert(cross_context_subscriber);

        bool cross_context_matched = false;
        for (int attempt = 0; attempt < 30 && !cross_context_matched; ++attempt) {
            auto matched = cross_context_publisher.value()->matched_subscriber_count();
            assert(matched);
            cross_context_matched = matched.value() != 0;
            if (!cross_context_matched) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        assert(cross_context_matched);
        int cross_context_message = 3;
        dmw::MessageInfo cross_context_info;
        bool cross_context_received = false;
        for (int attempt = 0; attempt < 250 && !cross_context_received; ++attempt) {
            // A matched writer is not always past the first reliable route
            // establishment cycle on loaded DDS hosts.  Re-publish the same
            // sample while waiting; success still requires a real remote
            // take, not merely a match notification.
            assert(cross_context_publisher.value()->publish(&cross_context_message));
            auto take =
                cross_context_subscriber.value()->take(&cross_context_message, cross_context_info);
            assert(take);
            cross_context_received = take.value() == dmw::TakeStatus::Taken;
            if (!cross_context_received) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        assert(cross_context_received);

        dmw::ServiceType cross_context_service_type(generated.value(), generated.value());
        auto cross_context_client = writer_node.value()->create_client(
            cross_context_service_type, "cross_context_service", dmw::Qos{});
        auto cross_context_server = reader_node.value()->create_server(
            cross_context_service_type, "cross_context_service", dmw::Qos{});
        assert(cross_context_client);
        assert(cross_context_server);

        bool cross_context_service_available = false;
        for (int attempt = 0; attempt < 30 && !cross_context_service_available; ++attempt) {
            auto available = cross_context_client.value()->service_is_available();
            assert(available);
            cross_context_service_available = available.value();
            if (!cross_context_service_available) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        assert(cross_context_service_available);

        int cross_context_request = 31;
        auto cross_context_request_id =
            cross_context_client.value()->send_request(&cross_context_request);
        assert(cross_context_request_id);
        dmw::RequestId received_cross_context_request_id;
        bool cross_context_request_received = false;
        for (int attempt = 0; attempt < 30 && !cross_context_request_received; ++attempt) {
            auto take = cross_context_server.value()->take_request(
                &cross_context_request, received_cross_context_request_id);
            assert(take);
            cross_context_request_received = take.value() == dmw::TakeStatus::Taken;
            if (!cross_context_request_received) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        assert(cross_context_request_received);
        assert(received_cross_context_request_id == cross_context_request_id.value());

        int cross_context_response = 32;
        assert(cross_context_server.value()->send_response(
            received_cross_context_request_id, &cross_context_response));
        dmw::RequestId received_cross_context_response_id;
        bool cross_context_response_received = false;
        for (int attempt = 0; attempt < 30 && !cross_context_response_received; ++attempt) {
            auto take = cross_context_client.value()->take_response(
                &cross_context_response, received_cross_context_response_id);
            assert(take);
            cross_context_response_received = take.value() == dmw::TakeStatus::Taken;
            if (!cross_context_response_received) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        assert(cross_context_response_received);
        assert(received_cross_context_response_id == cross_context_request_id.value());
    }

    {
        dmw::ContextOptions domain_a_options;
        domain_a_options.domain_id = 41;
        domain_a_options.participant_name = "dmw-domain-a";
        auto domain_a = dmw::Context::create(domain_a_options);
        assert(domain_a);
        dmw::ContextOptions domain_b_options;
        domain_b_options.domain_id = 42;
        domain_b_options.participant_name = "dmw-domain-b";
        auto domain_b = dmw::Context::create(domain_b_options);
        assert(domain_b);

        auto domain_a_node = domain_a.value()->create_node(node_options);
        auto domain_b_node = domain_b.value()->create_node(node_options);
        assert(domain_a_node);
        assert(domain_b_node);
        auto domain_a_publisher = domain_a_node.value()->create_publisher(
            generated.value(), "domain_isolation", dmw::Qos{});
        auto domain_b_subscriber = domain_b_node.value()->create_subscriber(
            generated.value(), "domain_isolation", dmw::Qos{});
        assert(domain_a_publisher);
        assert(domain_b_subscriber);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto matched = domain_a_publisher.value()->matched_subscriber_count();
        assert(matched);
        assert(matched.value() == 0);
        int isolated_message = 4;
        assert(domain_a_publisher.value()->publish(&isolated_message));
        dmw::MessageInfo isolated_info;
        auto isolated_take = domain_b_subscriber.value()->take(&isolated_message, isolated_info);
        assert(isolated_take);
        assert(isolated_take.value() == dmw::TakeStatus::NoData);
    }

    {
        dmw::ContextOptions ros_writer_options;
        ros_writer_options.participant_name = "dmw-ros-profile-writer";
        ros_writer_options.compatibility_profile = dmw::CompatibilityProfile::Ros2FastDdsHumble;
        auto ros_writer_context = dmw::Context::create(ros_writer_options);
        assert(ros_writer_context);
        dmw::ContextOptions ros_reader_options;
        ros_reader_options.participant_name = "dmw-ros-profile-reader";
        ros_reader_options.compatibility_profile = dmw::CompatibilityProfile::Ros2FastDdsHumble;
        auto ros_reader_context = dmw::Context::create(ros_reader_options);
        assert(ros_reader_context);
        auto ros_writer_node = ros_writer_context.value()->create_node(node_options);
        auto ros_reader_node = ros_reader_context.value()->create_node(node_options);
        assert(ros_writer_node);
        assert(ros_reader_node);
        auto ros_publisher = ros_writer_node.value()->create_publisher(
            generated.value(), "ros_profile_topic", dmw::Qos{});
        auto ros_subscriber = ros_reader_node.value()->create_subscriber(
            generated.value(), "ros_profile_topic", dmw::Qos{});
        assert(ros_publisher);
        assert(ros_subscriber);

        bool ros_profile_matched = false;
        for (int attempt = 0; attempt < 30 && !ros_profile_matched; ++attempt) {
            auto matched = ros_publisher.value()->matched_subscriber_count();
            assert(matched);
            ros_profile_matched = matched.value() != 0;
            if (!ros_profile_matched) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        assert(ros_profile_matched);
        int ros_profile_message = 5;
        assert(ros_publisher.value()->publish(&ros_profile_message));
        dmw::MessageInfo ros_profile_info;
        bool ros_profile_received = false;
        for (int attempt = 0; attempt < 30 && !ros_profile_received; ++attempt) {
            auto take = ros_subscriber.value()->take(&ros_profile_message, ros_profile_info);
            assert(take);
            ros_profile_received = take.value() == dmw::TakeStatus::Taken;
            if (!ros_profile_received) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        assert(ros_profile_received);

        dmw::ServiceType ros_service_type(generated.value(), generated.value());
        auto ros_client = ros_writer_node.value()->create_client(
            ros_service_type, "ros_profile_service", dmw::Qos{});
        auto ros_server = ros_reader_node.value()->create_server(
            ros_service_type, "ros_profile_service", dmw::Qos{});
        assert(ros_client);
        assert(ros_server);
        bool ros_service_available = false;
        for (int attempt = 0; attempt < 30 && !ros_service_available; ++attempt) {
            auto available = ros_client.value()->service_is_available();
            assert(available);
            ros_service_available = available.value();
            if (!ros_service_available) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        assert(ros_service_available);
        int ros_request = 6;
        auto ros_request_id = ros_client.value()->send_request(&ros_request);
        assert(ros_request_id);
        dmw::RequestId ros_server_request_id;
        bool ros_request_received = false;
        for (int attempt = 0; attempt < 30 && !ros_request_received; ++attempt) {
            auto take = ros_server.value()->take_request(&ros_request, ros_server_request_id);
            assert(take);
            ros_request_received = take.value() == dmw::TakeStatus::Taken;
            if (!ros_request_received) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        assert(ros_request_received);
        int ros_response = 7;
        assert(ros_server.value()->send_response(ros_server_request_id, &ros_response));
        dmw::RequestId ros_response_id;
        bool ros_response_received = false;
        for (int attempt = 0; attempt < 30 && !ros_response_received; ++attempt) {
            auto take = ros_client.value()->take_response(&ros_response, ros_response_id);
            assert(take);
            ros_response_received = take.value() == dmw::TakeStatus::Taken;
            if (!ros_response_received) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        assert(ros_response_received);
        assert(ros_response_id == ros_request_id.value());
    }

    dmw::ContextOptions surviving_endpoint_context_options;
    surviving_endpoint_context_options.participant_name = "dmw-surviving-endpoint-test";
    auto surviving_endpoint_context = dmw::Context::create(surviving_endpoint_context_options);
    assert(surviving_endpoint_context);
    auto surviving_endpoint_node = surviving_endpoint_context.value()->create_node(node_options);
    assert(surviving_endpoint_node);
    auto surviving_endpoint_publisher = surviving_endpoint_node.value()->create_publisher(
        generated.value(), "survives", dmw::Qos{});
    assert(surviving_endpoint_publisher);
    surviving_endpoint_node.value().reset();
    surviving_endpoint_context.value().reset();
    int surviving_message = 0;
    auto after_context_destruction =
        surviving_endpoint_publisher.value()->publish(&surviving_message);
    assert(!after_context_destruction);
    assert(after_context_destruction.error().code() == dmw::ErrorCode::ContextShutdown);
    surviving_endpoint_publisher.value().reset();

    auto event_parent_publisher =
        node.value()->create_publisher(generated.value(), "event_parent", dmw::Qos{});
    assert(event_parent_publisher);
    auto parent_event =
        event_parent_publisher.value()->create_event(dmw::EventType::OfferedDeadlineMissed);
    assert(parent_event);
    auto parent_event_wait_set = context.value()->create_wait_set();
    assert(parent_event_wait_set);
    auto parent_event_token = parent_event_wait_set.value()->add(*parent_event.value());
    assert(parent_event_token);
    event_parent_publisher.value().reset();
    dmw::EventInfo parent_event_info;
    auto after_parent_destruction = parent_event.value()->take(parent_event_info);
    assert(!after_parent_destruction);
    assert(after_parent_destruction.error().code() == dmw::ErrorCode::ParentDestroyed);
    auto parent_event_wait = parent_event_wait_set.value()->wait(dmw::WaitTimeout::poll());
    assert(parent_event_wait);
    assert(parent_event_wait.value().status() == dmw::WaitStatus::Timeout);
    auto stale_event_remove = parent_event_wait_set.value()->remove(parent_event_token.value());
    assert(!stale_event_remove);
    assert(stale_event_remove.error().code() == dmw::ErrorCode::NotRegistered);

    auto publisher = node.value()->create_publisher(generated.value(), "messages", dmw::Qos{});
    auto subscriber = node.value()->create_subscriber(generated.value(), "messages", dmw::Qos{});
    assert(publisher);
    assert(subscriber);
    assert(publisher.value()->topic_name() == "/dmw/messages");
    assert(subscriber.value()->topic_name() == "/dmw/messages");

    dmw::NodeOptions transient_node_options;
    transient_node_options.name = "transient_node";
    transient_node_options.ns = "/dmw";
    auto transient_node = context.value()->create_node(transient_node_options);
    assert(transient_node);
    auto node_surviving_publisher =
        transient_node.value()->create_publisher(generated.value(), "node_survives", dmw::Qos{});
    auto node_surviving_subscriber =
        transient_node.value()->create_subscriber(generated.value(), "node_survives", dmw::Qos{});
    assert(node_surviving_publisher);
    assert(node_surviving_subscriber);
    transient_node.value().reset();
    int node_surviving_message = 11;
    assert(node_surviving_publisher.value()->publish(&node_surviving_message));
    dmw::MessageInfo node_surviving_info;
    bool node_surviving_received = false;
    for (int attempt = 0; attempt < 30 && !node_surviving_received; ++attempt) {
        auto take =
            node_surviving_subscriber.value()->take(&node_surviving_message, node_surviving_info);
        assert(take);
        node_surviving_received = take.value() == dmw::TakeStatus::Taken;
        if (!node_surviving_received) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(node_surviving_received);

    auto colliding_type = dmw::fastdds::make_message_type<CollidingTopicDataType>();
    assert(colliding_type);
    auto colliding_publisher =
        node.value()->create_publisher(colliding_type.value(), "messages", dmw::Qos{});
    assert(!colliding_publisher);
    assert(colliding_publisher.error().code() == dmw::ErrorCode::TypeMismatch);

    auto auto_detach_subscriber =
        node.value()->create_subscriber(generated.value(), "auto_detach_reader", dmw::Qos{});
    assert(auto_detach_subscriber);
    auto auto_detach_reader_token = wait_set.value()->add(*auto_detach_subscriber.value());
    assert(auto_detach_reader_token);
    auto finite_wait = dmw::WaitTimeout::finite(std::chrono::milliseconds(50));
    assert(finite_wait);
    std::optional<dmw::Result<dmw::WaitResult>> auto_detach_wait_result;
    std::thread auto_detach_wait_thread(
        [&] { auto_detach_wait_result.emplace(wait_set.value()->wait(finite_wait.value())); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto_detach_subscriber.value().reset();
    auto_detach_wait_thread.join();
    assert(auto_detach_wait_result && *auto_detach_wait_result);
    assert(auto_detach_wait_result->value().status() == dmw::WaitStatus::Timeout);
    auto stale_reader_remove = wait_set.value()->remove(auto_detach_reader_token.value());
    assert(!stale_reader_remove);
    assert(stale_reader_remove.error().code() == dmw::ErrorCode::NotRegistered);

    bool topic_matched = false;
    for (int attempt = 0; attempt < 30 && !topic_matched; ++attempt) {
        auto matched = publisher.value()->matched_subscriber_count();
        if (!matched) {
            std::cerr << "matched_subscriber_count failed: " << matched.error().message() << '\n';
            return 1;
        }
        topic_matched = matched.value() != 0;
        if (!topic_matched) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(topic_matched);

    auto data_wait_set = context.value()->create_wait_set();
    assert(data_wait_set);
    auto data_token = data_wait_set.value()->add(*subscriber.value());
    assert(data_token);
    auto data_wait_timeout = dmw::WaitTimeout::finite(std::chrono::seconds(1));
    assert(data_wait_timeout);
    std::optional<dmw::Result<dmw::WaitResult>> data_wait_result;
    std::thread data_wait_thread(
        [&] { data_wait_result.emplace(data_wait_set.value()->wait(data_wait_timeout.value())); });

    int message = 0;
    assert(publisher.value()->publish(&message));
    data_wait_thread.join();
    assert(data_wait_result && *data_wait_result);
    assert(data_wait_result->value().status() == dmw::WaitStatus::Ready);
    assert(data_wait_result->value().ready().size() == 1);
    assert(data_wait_result->value().ready().front() == data_token.value());
    assert(data_wait_set.value()->remove(data_token.value()));

    dmw::MessageInfo info;
    bool received = false;
    for (int attempt = 0; attempt < 30 && !received; ++attempt) {
        auto take = subscriber.value()->take(&message, info);
        assert(take);
        received = take.value() == dmw::TakeStatus::Taken;
        if (!received) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(received);

    // A Topic lease belongs to each endpoint.  Releasing the last two
    // endpoints must delete that Topic without unregistering the wire type
    // while another Topic still uses it, and later creation must work again.
    auto retained_type_publisher =
        node.value()->create_publisher(generated.value(), "retained_type", dmw::Qos{});
    assert(retained_type_publisher);
    publisher.value().reset();
    subscriber.value().reset();
    auto recreated_publisher =
        node.value()->create_publisher(generated.value(), "messages", dmw::Qos{});
    auto recreated_subscriber =
        node.value()->create_subscriber(generated.value(), "messages", dmw::Qos{});
    assert(recreated_publisher);
    assert(recreated_subscriber);

    eprosima::fastdds::dds::TypeSupport releasable_support(
        new NamedTopicDataType("dmw.test.ReleasableTopicDataType"));
    auto releasable_type =
        dmw::fastdds::MessageTypeAccess::create(releasable_support, typeid(NamedTopicDataType));
    assert(releasable_type);
    auto releasable_publisher =
        node.value()->create_publisher(releasable_type.value(), "releasable", dmw::Qos{});
    auto releasable_subscriber =
        node.value()->create_subscriber(releasable_type.value(), "releasable", dmw::Qos{});
    assert(releasable_publisher);
    assert(releasable_subscriber);
    releasable_publisher.value().reset();
    releasable_subscriber.value().reset();
    auto re_registered_publisher =
        node.value()->create_publisher(releasable_type.value(), "releasable", dmw::Qos{});
    assert(re_registered_publisher);

    std::optional<dmw::Result<std::unique_ptr<dmw::Publisher>>> first_concurrent_publisher;
    std::optional<dmw::Result<std::unique_ptr<dmw::Publisher>>> second_concurrent_publisher;
    std::thread first_creator([&] {
        first_concurrent_publisher.emplace(
            node.value()->create_publisher(generated.value(), "concurrent_registry", dmw::Qos{}));
    });
    std::thread second_creator([&] {
        second_concurrent_publisher.emplace(
            node.value()->create_publisher(generated.value(), "concurrent_registry", dmw::Qos{}));
    });
    first_creator.join();
    second_creator.join();
    assert(first_concurrent_publisher && *first_concurrent_publisher);
    assert(second_concurrent_publisher && *second_concurrent_publisher);

    dmw::ServiceType service_type(generated.value(), generated.value());
    auto client = node.value()->create_client(service_type, "echo", dmw::Qos{});
    auto server = node.value()->create_server(service_type, "echo", dmw::Qos{});
    assert(client);
    assert(server);

    bool available = false;
    for (int attempt = 0; attempt < 30 && !available; ++attempt) {
        auto result = client.value()->service_is_available();
        assert(result);
        available = result.value();
        if (!available) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(available);

    auto late_client = node.value()->create_client(service_type, "echo", dmw::Qos{});
    assert(late_client);
    bool late_available = false;
    for (int attempt = 0; attempt < 30 && !late_available; ++attempt) {
        auto result = late_client.value()->service_is_available();
        assert(result);
        late_available = result.value();
        if (!late_available) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(late_available);

    int request = 42;
    auto server_wait_set = context.value()->create_wait_set();
    assert(server_wait_set);
    auto server_token = server_wait_set.value()->add(*server.value());
    assert(server_token);
    auto service_wait_timeout = dmw::WaitTimeout::finite(std::chrono::seconds(1));
    assert(service_wait_timeout);
    std::optional<dmw::Result<dmw::WaitResult>> server_wait_result;
    std::thread server_wait_thread([&] {
        server_wait_result.emplace(server_wait_set.value()->wait(service_wait_timeout.value()));
    });
    auto sent_request = client.value()->send_request(&request);
    assert(sent_request);
    server_wait_thread.join();
    assert(server_wait_result && *server_wait_result);
    assert(server_wait_result->value().status() == dmw::WaitStatus::Ready);
    assert(server_wait_result->value().ready().size() == 1);
    assert(server_wait_result->value().ready().front() == server_token.value());
    assert(server_wait_set.value()->remove(server_token.value()));

    dmw::RequestId request_id;
    bool request_received = false;
    for (int attempt = 0; attempt < 30 && !request_received; ++attempt) {
        auto take = server.value()->take_request(&request, request_id);
        assert(take);
        request_received = take.value() == dmw::TakeStatus::Taken;
        if (!request_received) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(request_received);
    assert(request_id == sent_request.value());

    int response = 7;
    auto client_wait_set = context.value()->create_wait_set();
    assert(client_wait_set);
    auto client_token = client_wait_set.value()->add(*client.value());
    assert(client_token);
    std::optional<dmw::Result<dmw::WaitResult>> client_wait_result;
    std::thread client_wait_thread([&] {
        client_wait_result.emplace(client_wait_set.value()->wait(service_wait_timeout.value()));
    });
    assert(server.value()->send_response(request_id, &response));
    client_wait_thread.join();
    assert(client_wait_result && *client_wait_result);
    assert(client_wait_result->value().status() == dmw::WaitStatus::Ready);
    assert(client_wait_result->value().ready().size() == 1);
    assert(client_wait_result->value().ready().front() == client_token.value());
    assert(client_wait_set.value()->remove(client_token.value()));

    dmw::RequestId response_id;
    bool response_received = false;
    for (int attempt = 0; attempt < 30 && !response_received; ++attempt) {
        auto take = client.value()->take_response(&response, response_id);
        assert(take);
        response_received = take.value() == dmw::TakeStatus::Taken;
        if (!response_received) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(response_received);
    assert(response_id == sent_request.value());

    int second_request = 84;
    auto second_sent_request = late_client.value()->send_request(&second_request);
    assert(second_sent_request);

    dmw::RequestId second_request_id;
    bool second_request_received = false;
    for (int attempt = 0; attempt < 30 && !second_request_received; ++attempt) {
        auto take = server.value()->take_request(&second_request, second_request_id);
        assert(take);
        second_request_received = take.value() == dmw::TakeStatus::Taken;
        if (!second_request_received) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(second_request_received);
    assert(second_request_id == second_sent_request.value());

    int second_response = 9;
    assert(server.value()->send_response(second_request_id, &second_response));

    dmw::RequestId unexpected_response_id;
    auto unexpected_response = client.value()->take_response(&response, unexpected_response_id);
    assert(unexpected_response);
    assert(unexpected_response.value() == dmw::TakeStatus::NoData);

    dmw::RequestId second_response_id;
    bool second_response_received = false;
    for (int attempt = 0; attempt < 30 && !second_response_received; ++attempt) {
        auto take = late_client.value()->take_response(&second_response, second_response_id);
        assert(take);
        second_response_received = take.value() == dmw::TakeStatus::Taken;
        if (!second_response_received) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(second_response_received);
    assert(second_response_id == second_sent_request.value());

    dmw::ServerOptions single_pending_options;
    single_pending_options.max_pending_requests = 1;
    auto capacity_client = node.value()->create_client(service_type, "capacity", dmw::Qos{});
    auto capacity_server =
        node.value()->create_server(service_type, "capacity", dmw::Qos{}, single_pending_options);
    assert(capacity_client);
    assert(capacity_server);
    auto capacity_wait_set = context.value()->create_wait_set();
    assert(capacity_wait_set);
    auto capacity_wait_token = capacity_wait_set.value()->add(*capacity_server.value());
    assert(capacity_wait_token);
    bool capacity_service_available = false;
    for (int attempt = 0; attempt < 30 && !capacity_service_available; ++attempt) {
        auto available = capacity_client.value()->service_is_available();
        assert(available);
        capacity_service_available = available.value();
        if (!capacity_service_available) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    assert(capacity_service_available);

    int capacity_request = 101;
    auto first_capacity_request = capacity_client.value()->send_request(&capacity_request);
    assert(first_capacity_request);
    dmw::RequestId first_capacity_request_id;
    bool first_capacity_taken = false;
    for (int attempt = 0; attempt < 30 && !first_capacity_taken; ++attempt) {
        auto take =
            capacity_server.value()->take_request(&capacity_request, first_capacity_request_id);
        assert(take);
        first_capacity_taken = take.value() == dmw::TakeStatus::Taken;
        if (!first_capacity_taken) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(first_capacity_taken);
    assert(first_capacity_request_id == first_capacity_request.value());

    int next_capacity_request = 102;
    auto second_capacity_request = capacity_client.value()->send_request(&next_capacity_request);
    assert(second_capacity_request);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    dmw::RequestId next_capacity_request_id;
    auto capacity_exhausted =
        capacity_server.value()->take_request(&next_capacity_request, next_capacity_request_id);
    assert(!capacity_exhausted);
    assert(capacity_exhausted.error().code() == dmw::ErrorCode::ResourceExhausted);
    // The second request remains unread in Fast DDS history, but a full
    // Server must remove its reader condition from the blocking topology.
    auto full_wait = capacity_wait_set.value()->wait(dmw::WaitTimeout::poll());
    assert(full_wait);
    assert(full_wait.value().status() == dmw::WaitStatus::Timeout);

    int capacity_response = 201;
    assert(capacity_server.value()->send_response(first_capacity_request_id, &capacity_response));
    auto available_wait = capacity_wait_set.value()->wait(dmw::WaitTimeout::poll());
    assert(available_wait);
    assert(available_wait.value().status() == dmw::WaitStatus::Ready);
    bool second_capacity_taken = false;
    for (int attempt = 0; attempt < 30 && !second_capacity_taken; ++attempt) {
        auto take =
            capacity_server.value()->take_request(&next_capacity_request, next_capacity_request_id);
        assert(take);
        second_capacity_taken = take.value() == dmw::TakeStatus::Taken;
        if (!second_capacity_taken) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(second_capacity_taken);
    assert(next_capacity_request_id == second_capacity_request.value());

    auto shutdown_wait_set = context.value()->create_wait_set();
    assert(shutdown_wait_set);
    std::optional<dmw::Result<dmw::WaitResult>> shutdown_wait_result;
    std::thread shutdown_wait_thread([&] {
        shutdown_wait_result.emplace(shutdown_wait_set.value()->wait(dmw::WaitTimeout::infinite()));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(context.value()->shutdown());
    shutdown_wait_thread.join();
    assert(shutdown_wait_result);
    assert(!*shutdown_wait_result);
    assert(shutdown_wait_result->error().code() == dmw::ErrorCode::ContextShutdown);
    assert(context.value()->is_shutdown());

    auto rejected_node = context.value()->create_node(node_options);
    assert(!rejected_node);
    assert(rejected_node.error().code() == dmw::ErrorCode::ContextShutdown);

    dmw::ContextOptions conflict_context_options;
    conflict_context_options.domain_id = 79;
    auto conflict_context = dmw::Context::create(conflict_context_options);
    assert(conflict_context);
    auto conflict_node = conflict_context.value()->create_node(node_options);
    assert(conflict_node);
    auto conflict_publisher = conflict_node.value()->create_publisher(
        generated.value(), "fingerprint", dmw::Qos{});
    assert(conflict_publisher);
    dmw::Qos conflicting_topic_qos;
    conflicting_topic_qos.reliable();
    auto incompatible_subscriber = conflict_node.value()->create_subscriber(
        generated.value(), "fingerprint", conflicting_topic_qos);
    assert(!incompatible_subscriber);
    assert(incompatible_subscriber.error().code() == dmw::ErrorCode::DdsError);

    auto null_result = dmw::fastdds::MessageTypeAccess::create({}, typeid(NamedTopicDataType));
    assert(!null_result);
    assert(null_result.error().code() == dmw::ErrorCode::InvalidArgument);

    auto empty_result = dmw::fastdds::make_message_type<EmptyNameTopicDataType>();
    assert(!empty_result);
    assert(empty_result.error().code() == dmw::ErrorCode::InvalidArgument);

    return 0;
}
