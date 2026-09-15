#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <typeindex>

#include "fastdds/dds/topic/TopicDataType.hpp"

#include "dmw/context.hpp"
#include "dmw/fastdds/message_type.hpp"

namespace {

class IntTopicDataType final : public eprosima::fastdds::dds::TopicDataType {
public:
    IntTopicDataType() {
        m_typeSize = sizeof(int);
        setName("dmw.test.LifecycleInt");
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

    bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override { return false; }
};

}  // namespace

int main() {
    if (std::getenv("DMW_ENABLE_DDS_INTEGRATION") == nullptr) return 0;

    auto message_type = dmw::fastdds::create_message_type<IntTopicDataType>();
    assert(message_type);
    const dmw::ServiceType service_type(message_type.value(), message_type.value());

    for (std::uint32_t iteration = 0; iteration < 20; ++iteration) {
        dmw::ContextOptions context_options;
        context_options.domain_id = 100U + iteration;
        context_options.participant_name = "dmw-lifecycle-stress";
        auto context = dmw::Context::create(context_options);
        assert(context);
        dmw::NodeOptions node_options;
        node_options.node_name = "lifecycle_stress";
        auto node = context.value()->create_node(node_options);
        assert(node);
        auto publisher = node.value()->create_publisher(message_type.value(), "topic", dmw::Qos{});
        auto subscriber = node.value()->create_subscriber(message_type.value(), "topic", dmw::Qos{});
        auto client = node.value()->create_client(service_type, "service", dmw::Qos{});
        auto server = node.value()->create_server(service_type, "service", dmw::Qos{});
        assert(publisher);
        assert(subscriber);
        assert(client);
        assert(server);
    }

    return 0;
}
