// SPDX-License-Identifier: Apache-2.0

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <typeindex>

#include "fastcdr/Cdr.h"
#include "fastcdr/FastBuffer.h"
#include "fastdds/dds/topic/TopicDataType.hpp"
#include "rclcpp/rclcpp.hpp"
#include "rosidl_typesupport_fastrtps_cpp/message_type_support.h"
#include "std_msgs/msg/detail/string__rosidl_typesupport_fastrtps_cpp.hpp"
#include "std_msgs/msg/string.hpp"

#include "dmw/context.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/publisher.hpp"
#include "dmw/subscriber.hpp"

namespace {

constexpr char kTopicName[] = "/dmw_ros2_topic_interop";
constexpr char kWireTypeName[] = "std_msgs::msg::dds_::String_";

class RosStringTypeSupport final : public eprosima::fastdds::dds::TopicDataType {
public:
    RosStringTypeSupport() {
        m_typeSize = 255;
        m_isGetKeyDefined = false;
        setName(kWireTypeName);
    }

    bool serialize(void* data, eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
        eprosima::fastcdr::FastBuffer buffer(
            reinterpret_cast<char*>(payload->data), payload->max_size);
        eprosima::fastcdr::Cdr cdr(
            buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, eprosima::fastcdr::Cdr::DDS_CDR);
        cdr.serialize_encapsulation();
        if (!callbacks()->cdr_serialize(data, cdr)) {
            return false;
        }
        payload->length = static_cast<std::uint32_t>(cdr.getSerializedDataLength());
        return true;
    }

    bool deserialize(eprosima::fastrtps::rtps::SerializedPayload_t* payload, void* data) override {
        eprosima::fastcdr::FastBuffer buffer(
            reinterpret_cast<char*>(payload->data), static_cast<std::size_t>(payload->length));
        eprosima::fastcdr::Cdr cdr(
            buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, eprosima::fastcdr::Cdr::DDS_CDR);
        cdr.read_encapsulation();
        return callbacks()->cdr_deserialize(cdr, data);
    }

    std::function<std::uint32_t()> getSerializedSizeProvider(void* data) override {
        const auto serialized_size = callbacks()->get_serialized_size(data);
        return [serialized_size] { return serialized_size + 4U; };
    }

    void* createData() override { return new std_msgs::msg::String(); }

    void deleteData(void* data) override { delete static_cast<std_msgs::msg::String*>(data); }

    bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override { return false; }

private:
    static const message_type_support_callbacks_t* callbacks() {
        const auto* support = ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
            rosidl_typesupport_fastrtps_cpp, std_msgs, msg, String)();
        assert(support != nullptr);
        assert(support->data != nullptr);
        return static_cast<const message_type_support_callbacks_t*>(support->data);
    }
};

template <typename Predicate>
bool wait_until(Predicate&& predicate) {
    // Cross-participant discovery and the first reliable delivery may share
    // the same Fast DDS discovery cycle on loaded CI hosts.  Five seconds is
    // deliberately a deadline, not a polling policy: success normally
    // returns on the first 10 ms iteration after the event.
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

}  // namespace

int main() {
    assert(setenv("RMW_IMPLEMENTATION", "rmw_fastrtps_cpp", 1) == 0);
    rclcpp::init(0, nullptr);

    {
        auto ros_node = std::make_shared<rclcpp::Node>("dmw_ros2_topic_peer");
        auto ros_publisher = ros_node->create_publisher<std_msgs::msg::String>(kTopicName, 10);
        std::string ros_received;
        auto ros_subscriber = ros_node->create_subscription<std_msgs::msg::String>(
            kTopicName, 10, [&ros_received](std_msgs::msg::String::ConstSharedPtr message) {
                ros_received = message->data;
            });
        (void)ros_subscriber;

        eprosima::fastdds::dds::TypeSupport type_support(new RosStringTypeSupport());
        auto message_type =
            dmw::fastdds::MessageTypeAccess::create(type_support, typeid(std_msgs::msg::String));
        assert(message_type);
        assert(message_type.value().type_name() == kWireTypeName);

        dmw::ContextOptions context_options;
        context_options.participant_name = "dmw-ros2-topic-peer";
        context_options.compatibility_profile = dmw::CompatibilityProfile::Ros2FastDdsHumble;
        auto context = dmw::Context::create(context_options);
        assert(context);

        dmw::NodeOptions node_options;
        node_options.name = "dmw_ros2_topic_node";
        auto node = context.value()->create_node(node_options);
        assert(node);
        auto publisher =
            node.value()->create_publisher(message_type.value(), kTopicName, dmw::Qos{});
        auto subscriber =
            node.value()->create_subscriber(message_type.value(), kTopicName, dmw::Qos{});
        assert(publisher);
        assert(subscriber);

        assert(wait_until([&publisher] {
            auto matched = publisher.value()->matched_subscriber_count();
            return matched && matched.value() != 0;
        }));
        assert(wait_until([&subscriber] {
            auto matched = subscriber.value()->matched_publisher_count();
            return matched && matched.value() != 0;
        }));

        std_msgs::msg::String from_ros;
        from_ros.data = "from ros2";
        std_msgs::msg::String dmw_received;
        dmw::MessageInfo message_info;
        assert(wait_until([&ros_publisher, &from_ros, &subscriber, &dmw_received, &message_info] {
            // Endpoint matching precedes every fully established reliable
            // writer-reader route on some Fast DDS discovery cycles.  Keep
            // publishing the identical sample until the route accepts it.
            ros_publisher->publish(from_ros);
            auto take = subscriber.value()->take(&dmw_received, message_info);
            assert(take);
            return take.value() == dmw::TakeStatus::Taken;
        }));
        assert(dmw_received.data == from_ros.data);

        std_msgs::msg::String from_dmw;
        from_dmw.data = "from dmw";
        assert(wait_until([&publisher, &from_dmw, &ros_node, &ros_received] {
            assert(publisher.value()->publish(&from_dmw));
            rclcpp::spin_some(ros_node);
            return ros_received == "from dmw";
        }));
    }

    rclcpp::shutdown();
    return 0;
}
