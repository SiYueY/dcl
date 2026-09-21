#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <typeindex>

#include "fastcdr/Cdr.h"
#include "fastcdr/FastBuffer.h"
#include "fastcdr/config.h"
#include "fastdds/dds/topic/TopicDataType.hpp"
#include "rclcpp/rclcpp.hpp"
#include "std_srvs/srv/set_bool.hpp"

#include "dmw/client.hpp"
#include "dmw/context.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/server.hpp"

// Service interoperability using std_srvs/srv/SetBool: unlike
// example_interfaces it is available in every supported ROS 2 distribution, so
// this test runs on both the Humble and the Rolling/Jazzy-era stacks.

namespace {

using SetBool = std_srvs::srv::SetBool;
using Request = SetBool::Request;
using Response = SetBool::Response;

constexpr char kDmwClientService[] = "/dmw_ros2_setbool_client_service";
constexpr char kDmwServerService[] = "/dmw_ros2_setbool_server_service";
constexpr char kRequestTypeName[] = "std_srvs::srv::dds_::SetBool_Request_";
constexpr char kResponseTypeName[] = "std_srvs::srv::dds_::SetBool_Response_";

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
        cdr << message.data;
    }
    static void deserialize(eprosima::fastcdr::Cdr& cdr, Request& message) {
        cdr >> message.data;
    }
};

template <>
struct CdrTraits<Response> {
    static void serialize(eprosima::fastcdr::Cdr& cdr, const Response& message) {
        cdr << message.success;
        cdr << message.message;
    }
    static void deserialize(eprosima::fastcdr::Cdr& cdr, Response& message) {
        cdr >> message.success;
        cdr >> message.message;
    }
};

template <typename Message>
class RosServiceTypeSupport final : public eprosima::fastdds::dds::TopicDataType {
public:
    explicit RosServiceTypeSupport(const char* type_name) {
        m_typeSize = 512;
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
        return [] { return 512U; };
    }

    void* createData() override { return new Message(); }

    void deleteData(void* data) override { delete static_cast<Message*>(data); }

    bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override {
        return false;
    }
};

template <typename F>
bool wait_until(F&& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
    }
    return condition();
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
        auto request_type =
            dmw::fastdds::MessageTypeAdapter::create(request_support, typeid(Request));
        auto response_type =
            dmw::fastdds::MessageTypeAdapter::create(response_support, typeid(Response));
        assert(request_type && response_type);

        dmw::ContextOptions context_options;
        context_options.participant_name = "dmw-ros2-setbool-peer";
        context_options.domain_id = ros_domain_id();
        context_options.runtime_mode = dmw::RuntimeMode::ROS2;
        auto context = dmw::Context::create(context_options);
        assert(context);
        dmw::NodeOptions node_options;
        node_options.node_name = "dmw_ros2_setbool_node";
        auto node = context.value()->create_node(node_options);
        assert(node);
        const dmw::ServiceType service_type(request_type.value(), response_type.value());

        auto ros_node = std::make_shared<rclcpp::Node>("dmw_ros2_setbool_peer");
        auto ros_service = ros_node->create_service<SetBool>(
            kDmwClientService,
            [](const std::shared_ptr<Request> request, std::shared_ptr<Response> response) {
                response->success = request->data;
                response->message = "ros2 peer";
            });
        (void)ros_service;

        // DMW client -> ROS 2 service
        auto dmw_client = node.value()->create_client(service_type, kDmwClientService, dmw::Qos{});
        assert(dmw_client);
        const auto service_timeout = dmw::WaitTimeout::finite(std::chrono::seconds(5));
        assert(service_timeout);
        const auto available = dmw_client.value()->wait_for_service(service_timeout.value());
        assert(available && available.value());

        Request request;
        request.data = true;
        const auto request_id = dmw_client.value()->write_request(&request);
        assert(request_id);
        Response response;
        dmw::RequestId response_id;
        assert(wait_until([&] {
            rclcpp::spin_some(ros_node);
            const auto take = dmw_client.value()->read_response(&response, response_id);
            assert(take);
            return take.value();
        }));
        assert(response.success);
        assert(response.message == "ros2 peer");
        assert(response_id == request_id.value());

        // ROS 2 client -> DMW server
        auto dmw_server = node.value()->create_server(service_type, kDmwServerService, dmw::Qos{});
        assert(dmw_server);
        auto ros_client = ros_node->create_client<SetBool>(kDmwServerService);
        assert(wait_until([&] {
            rclcpp::spin_some(ros_node);
            return ros_client->service_is_ready();
        }));

        auto ros_request = std::make_shared<Request>();
        ros_request->data = false;
        auto future = ros_client->async_send_request(ros_request);
        Request received_request;
        dmw::RequestId received_request_id;
        bool request_received = false;
        assert(wait_until([&] {
            rclcpp::spin_some(ros_node);
            if (!request_received) {
                const auto take = dmw_server.value()->read_request(&received_request,
                                                                    received_request_id);
                assert(take);
                if (take.value()) {
                    request_received = true;
                    Response server_response;
                    server_response.success = !received_request.data;
                    server_response.message = "dmw peer";
                    const auto written =
                        dmw_server.value()->write_response(received_request_id, &server_response);
                    assert(written);
                }
            }
            return future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
        }));
        assert(request_received);
        assert(received_request.data == false);
        const auto ros_response = future.get();
        assert(ros_response->success);
        assert(ros_response->message == "dmw peer");

        assert(context.value()->shutdown());
    }

    rclcpp::shutdown();
    return 0;
}
