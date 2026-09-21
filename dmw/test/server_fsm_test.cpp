#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>

#include "fastdds/dds/topic/TopicDataType.hpp"

#include "dmw/client.hpp"
#include "dmw/context.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/node.hpp"
#include "dmw/request_id.hpp"
#include "dmw/server.hpp"
#include "dmw/service_type.hpp"

// Regression for the Server pending-request FSM error paths (dmw.md §5.8):
// an unknown or already-answered RequestId must surface as NotFound instead of
// silently succeeding.

namespace {

using namespace std::chrono_literals;

class IntTopicDataType final : public eprosima::fastdds::dds::TopicDataType {
public:
    IntTopicDataType() {
        m_typeSize = sizeof(int);
        setName("dmw.test.ServerFsmInt");
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

dmw::RequestId unknown_request_id(std::uint8_t seed) {
    dmw::RequestId request_id;
    for (auto& byte : request_id.client_gid.data) byte = seed;
    request_id.sequence_number = 999;
    return request_id;
}

}  // namespace

int main() {
    if (std::getenv("DMW_ENABLE_DDS_INTEGRATION") == nullptr) return 0;

    const auto message_type = dmw::fastdds::create_message_type<IntTopicDataType, int>();
    assert(message_type);
    const dmw::ServiceType service_type(message_type.value(), message_type.value());

    dmw::ContextOptions options;
    options.domain_id = 196U;
    options.participant_name = "dmw-server-fsm";
    auto context = dmw::Context::create(options);
    assert(context);
    dmw::NodeOptions node_options;
    node_options.node_name = "server_fsm";
    auto node = context.value()->create_node(node_options);
    assert(node);
    auto client = node.value()->create_client(service_type, "/fsm_service", dmw::Qos{});
    auto server = node.value()->create_server(service_type, "/fsm_service", dmw::Qos{}, {});
    assert(client && server);
    for (int attempt = 0; attempt < 1500; ++attempt) {
        const auto available = client.value()->service_is_available();
        assert(available);
        if (available.value()) break;
        std::this_thread::sleep_for(10ms);
    }
    assert(client.value()->service_is_available().value());

    const int request = 7;
    const auto written = client.value()->write_request(&request);
    assert(written);
    int received = 0;
    dmw::RequestId request_id;
    bool taken = false;
    for (int attempt = 0; attempt < 2000 && !taken; ++attempt) {
        const auto read = server.value()->read_request(&received, request_id);
        assert(read);
        taken = read.value();
        if (!taken) std::this_thread::sleep_for(2ms);
    }
    assert(taken);
    assert(received == request);
    assert(request_id == written.value());

    // A RequestId that this Server never registered is not answerable.
    const int response = request + 1;
    const auto unknown = server.value()->write_response(unknown_request_id(0xAB), &response);
    assert(!unknown);
    assert(unknown.error().code() == dmw::ErrorCode::NotFound);

    // A null response is rejected before any state transition.
    const auto null_response = server.value()->write_response(request_id, nullptr);
    assert(!null_response);
    assert(null_response.error().code() == dmw::ErrorCode::InvalidArgument);

    // The request is still pending: the null-response attempt must not have
    // moved it into the Responding phase.
    const auto answered = server.value()->write_response(request_id, &response);
    assert(answered);

    // Answering the same RequestId twice is NotFound: the pending entry is
    // removed once the response is committed.
    const auto repeated = server.value()->write_response(request_id, &response);
    assert(!repeated);
    assert(repeated.error().code() == dmw::ErrorCode::NotFound);

    // The client observes exactly one response for the RequestId.
    int client_response = 0;
    dmw::RequestId response_id;
    bool response_received = false;
    for (int attempt = 0; attempt < 2000 && !response_received; ++attempt) {
        const auto read = client.value()->read_response(&client_response, response_id);
        assert(read);
        response_received = read.value();
        if (!response_received) std::this_thread::sleep_for(2ms);
    }
    assert(response_received);
    assert(client_response == response);
    assert(response_id == written.value());

    assert(context.value()->shutdown());
    const auto after_shutdown = server.value()->write_response(request_id, &response);
    assert(!after_shutdown);
    assert(after_shutdown.error().code() == dmw::ErrorCode::ContextShutdown);
    return 0;
}
