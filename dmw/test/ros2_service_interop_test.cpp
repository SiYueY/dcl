#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <thread>
#include <typeindex>

#include "example_interfaces/srv/add_two_ints.hpp"
#include "fastcdr/Cdr.h"
#include "fastcdr/FastBuffer.h"
#include "fastcdr/config.h"
#include "fastdds/dds/topic/TopicDataType.hpp"
#include "rclcpp/rclcpp.hpp"

#include "dmw/client.hpp"
#include "dmw/context.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/server.hpp"

namespace {

using AddTwoInts = example_interfaces::srv::AddTwoInts;
using Request = AddTwoInts::Request;
using Response = AddTwoInts::Response;

constexpr char kDmwClientService[] = "/dmw_ros2_client_service";
constexpr char kDmwServerService[] = "/dmw_ros2_server_service";
constexpr char kRequestTypeName[] = "example_interfaces::srv::dds_::AddTwoInts_Request_";
constexpr char kResponseTypeName[] = "example_interfaces::srv::dds_::AddTwoInts_Response_";

#if FASTCDR_VERSION_MAJOR >= 2
constexpr auto kRosCdrVersion = eprosima::fastcdr::DDS_CDR;
#else
constexpr auto kRosCdrVersion = eprosima::fastcdr::Cdr::DDS_CDR;
#endif

std::uint32_t ros_domain_id() {
    const char* value = std::getenv("ROS_DOMAIN_ID");
    assert(value != nullptr);
    char* end = nullptr;
    const auto parsed = std::strtoul(value, &end, 10);
    assert(*value != '\0' && *end == '\0' && parsed <= 232U);
    return static_cast<std::uint32_t>(parsed);
}

template <typename Message>
struct CdrTraits;

template <>
struct CdrTraits<Request> {
    static void serialize(eprosima::fastcdr::Cdr& cdr, const Request& message) {
        cdr << message.a;
        cdr << message.b;
    }

    static void deserialize(eprosima::fastcdr::Cdr& cdr, Request& message) {
        cdr >> message.a;
        cdr >> message.b;
    }
};

template <>
struct CdrTraits<Response> {
    static void serialize(eprosima::fastcdr::Cdr& cdr, const Response& message) {
        cdr << message.sum;
    }

    static void deserialize(eprosima::fastcdr::Cdr& cdr, Response& message) {
        cdr >> message.sum;
    }
};

template <typename Message>
class RosServiceTypeSupport final : public eprosima::fastdds::dds::TopicDataType {
public:
    explicit RosServiceTypeSupport(const char* type_name) {
        m_typeSize = 64;
        m_isGetKeyDefined = false;
        setName(type_name);
    }

    bool serialize(void* data, eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
        eprosima::fastcdr::FastBuffer buffer(
            reinterpret_cast<char*>(payload->data), payload->max_size);
        eprosima::fastcdr::Cdr cdr(
            buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, kRosCdrVersion);
        cdr.serialize_encapsulation();
        CdrTraits<Message>::serialize(cdr, *static_cast<Message*>(data));
#if FASTCDR_VERSION_MAJOR >= 2
        payload->length = static_cast<std::uint32_t>(cdr.get_serialized_data_length());
#else
        payload->length = static_cast<std::uint32_t>(cdr.getSerializedDataLength());
#endif
        return true;
    }

    bool deserialize(
        eprosima::fastrtps::rtps::SerializedPayload_t* payload, void* data) override {
        eprosima::fastcdr::FastBuffer buffer(
            reinterpret_cast<char*>(payload->data), static_cast<std::size_t>(payload->length));
        eprosima::fastcdr::Cdr cdr(
            buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, kRosCdrVersion);
        cdr.read_encapsulation();
        CdrTraits<Message>::deserialize(cdr, *static_cast<Message*>(data));
        return true;
    }

    std::function<std::uint32_t()> getSerializedSizeProvider(void*) override {
        return [] { return 20U; };
    }

    void* createData() override { return new Message(); }

    void deleteData(void* data) override { delete static_cast<Message*>(data); }

    bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override { return false; }
};

template <typename Predicate>
bool wait_until(Predicate&& predicate) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

}  // namespace

int main() {
    assert(setenv("RMW_IMPLEMENTATION", "rmw_fastrtps_cpp", 1) == 0);
    rclcpp::init(0, nullptr);

    {
        eprosima::fastdds::dds::TypeSupport request_support(
            new RosServiceTypeSupport<Request>(kRequestTypeName));
        eprosima::fastdds::dds::TypeSupport response_support(
            new RosServiceTypeSupport<Response>(kResponseTypeName));
        auto request_type = dmw::fastdds::MessageTypeAdapter::create(
            request_support, typeid(Request));
        auto response_type = dmw::fastdds::MessageTypeAdapter::create(
            response_support, typeid(Response));
        assert(request_type);
        assert(response_type);

        dmw::ContextOptions context_options;
        context_options.participant_name = "dmw-ros2-service-peer";
        context_options.domain_id = ros_domain_id();
        context_options.runtime_mode = dmw::RuntimeMode::ROS2;
        auto context = dmw::Context::create(context_options);
        assert(context);
        dmw::NodeOptions node_options;
        node_options.node_name = "dmw_ros2_service_node";
        auto node = context.value()->create_node(node_options);
        assert(node);
        const dmw::ServiceType service_type(request_type.value(), response_type.value());

        auto ros_node = std::make_shared<rclcpp::Node>("dmw_ros2_service_peer");
        auto ros_service = ros_node->create_service<AddTwoInts>(
            kDmwClientService,
            [](const std::shared_ptr<Request> request, std::shared_ptr<Response> response) {
                response->sum = request->a + request->b;
            });
        (void)ros_service;
        auto dmw_client = node.value()->create_client(service_type, kDmwClientService, dmw::Qos{});
        assert(dmw_client);
        auto service_timeout = dmw::WaitTimeout::finite(std::chrono::seconds(5));
        assert(service_timeout);
        auto available = dmw_client.value()->wait_for_service(service_timeout.value());
        assert(available);
        assert(available.value());

        Request request;
        request.a = 3;
        request.b = 4;
        auto request_id = dmw_client.value()->write_request(&request);
        assert(request_id);
        Response response;
        dmw::RequestId response_id;
        assert(wait_until([&] {
            rclcpp::spin_some(ros_node);
            auto take = dmw_client.value()->read_response(&response, response_id);
            assert(take);
            return take.value();
        }));
        assert(response.sum == 7);
        assert(response_id == request_id.value());

        auto dmw_server = node.value()->create_server(service_type, kDmwServerService, dmw::Qos{});
        assert(dmw_server);
        auto ros_client = ros_node->create_client<AddTwoInts>(kDmwServerService);
        assert(wait_until([&] { return ros_client->service_is_ready(); }));

        auto ros_request = std::make_shared<Request>();
        ros_request->a = 10;
        ros_request->b = 20;
        auto future = ros_client->async_send_request(ros_request);
        Request received_request;
        Response server_response;
        dmw::RequestId received_request_id;
        bool request_received = false;
        assert(wait_until([&] {
            rclcpp::spin_some(ros_node);
            if (!request_received) {
                auto take = dmw_server.value()->read_request(&received_request, received_request_id);
                assert(take);
                if (take.value()) {
                    request_received = true;
                    server_response.sum = received_request.a + received_request.b;
                    assert(dmw_server.value()->write_response(received_request_id, &server_response));
                }
            }
            return future.wait_for(std::chrono::seconds::zero()) == std::future_status::ready;
        }));
        assert(future.get()->sum == 30);
    }

    rclcpp::shutdown();
    return 0;
}
