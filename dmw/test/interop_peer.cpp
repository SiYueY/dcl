#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "fastcdr/Cdr.h"
#include "fastcdr/FastBuffer.h"
#include "fastcdr/config.h"
#include "fastdds/dds/topic/TopicDataType.hpp"

#include "dmw/context.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/publisher.hpp"
#include "dmw/subscriber.hpp"

namespace {

constexpr char kTopic[] = "/dmw_fastdds_cross_version_interop";
constexpr char kType[] = "std_msgs::msg::dds_::String_";

#if FASTCDR_VERSION_MAJOR >= 2
constexpr auto kCdrVersion = eprosima::fastcdr::DDS_CDR;
#else
constexpr auto kCdrVersion = eprosima::fastcdr::Cdr::DDS_CDR;
#endif

class StringType final : public eprosima::fastdds::dds::TopicDataType {
public:
    StringType() {
        m_typeSize = 1024;
        m_isGetKeyDefined = false;
        setName(kType);
    }

    bool serialize(void* data, eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
        eprosima::fastcdr::FastBuffer buffer(reinterpret_cast<char*>(payload->data), payload->max_size);
        eprosima::fastcdr::Cdr cdr(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, kCdrVersion);
        cdr.serialize_encapsulation();
        cdr << *static_cast<std::string*>(data);
#if FASTCDR_VERSION_MAJOR >= 2
        payload->length = static_cast<std::uint32_t>(cdr.get_serialized_data_length());
#else
        payload->length = static_cast<std::uint32_t>(cdr.getSerializedDataLength());
#endif
        return true;
    }

    bool deserialize(eprosima::fastrtps::rtps::SerializedPayload_t* payload, void* data) override {
        eprosima::fastcdr::FastBuffer buffer(
            reinterpret_cast<char*>(payload->data), static_cast<std::size_t>(payload->length));
        eprosima::fastcdr::Cdr cdr(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, kCdrVersion);
        cdr.read_encapsulation();
        cdr >> *static_cast<std::string*>(data);
        return true;
    }

    std::function<std::uint32_t()> getSerializedSizeProvider(void* data) override {
        const auto size = static_cast<std::uint32_t>(static_cast<std::string*>(data)->size() + 9U);
        return [size] { return size; };
    }
    void* createData() override { return new std::string(); }
    void deleteData(void* data) override { delete static_cast<std::string*>(data); }
    bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override { return false; }
};

std::uint32_t domain_id() {
    const char* value = std::getenv("ROS_DOMAIN_ID");
    if (value == nullptr) return 23;
    char* end = nullptr;
    const auto parsed = std::strtoul(value, &end, 10);
    return *value != '\0' && *end == '\0' && parsed <= 232U ? static_cast<std::uint32_t>(parsed) : 23;
}

template<class Predicate>
bool wait_until(Predicate predicate) {
    for (int attempt = 0; attempt != 1000; ++attempt) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 || (std::string(argv[1]) != "pub" && std::string(argv[1]) != "sub")) return 2;
    const std::string role(argv[1]);
    const std::string expected(argv[2]);
    auto type = dmw::fastdds::create_message_type<StringType>();
    if (!type) return 3;
    dmw::ContextOptions options;
    options.domain_id = domain_id();
    options.participant_name = "dmw-cross-version-" + role;
    options.runtime_mode = dmw::RuntimeMode::ROS2;
    auto context = dmw::Context::create(options);
    if (!context) return 4;
    dmw::NodeOptions node_options;
    node_options.node_name = "cross_version_" + role;
    auto node = context.value()->create_node(node_options);
    if (!node) return 5;
    if (role == "pub") {
        auto publisher = node.value()->create_publisher(type.value(), kTopic, dmw::Qos{});
        if (!publisher || !wait_until([&] { auto count = publisher.value()->matched_subscriber_count(); return count && count.value() != 0; })) return 6;
        if (!publisher.value()->write(&expected)) return 7;
        // Give the reliable writer one acknowledgement cycle before its
        // participant is destroyed; this is essential for process-per-role
        // interoperability probes.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } else {
        auto subscriber = node.value()->create_subscriber(type.value(), kTopic, dmw::Qos{});
        if (!subscriber || !wait_until([&] { auto count = subscriber.value()->matched_publisher_count(); return count && count.value() != 0; })) return 8;
        std::string received;
        dmw::MessageInfo info;
        if (!wait_until([&] { auto read = subscriber.value()->read(&received, info); return read && read.value() && received == expected; })) return 9;
    }
    return 0;
}
