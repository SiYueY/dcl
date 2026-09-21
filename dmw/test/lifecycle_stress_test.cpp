#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <future>
#include <thread>
#include <typeindex>
#include <unistd.h>

#include "fastdds/dds/topic/TopicDataType.hpp"

#include "dmw/context.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/wait_set.hpp"

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

    auto message_type = dmw::fastdds::create_message_type<IntTopicDataType, int>();
    assert(message_type);
    const dmw::ServiceType service_type(message_type.value(), message_type.value());

    // Partition the DDS domain space into disjoint 21-wide slots keyed by
    // process id, so two concurrent instances of this test (for example two
    // build trees running in parallel) never share a domain.  Without this the
    // infinite shutdown wait could legitimately observe a foreign sample and
    // return Ready, which is indistinguishable from a spurious wakeup.
    const std::uint32_t domain_base =
        1U + (static_cast<std::uint32_t>(::getpid()) % 11U) * 21U;
    for (std::uint32_t iteration = 0; iteration < 20; ++iteration) {
        dmw::ContextOptions context_options;
        context_options.domain_id = domain_base + iteration;
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
        auto unavailable_client =
            node.value()->create_client(service_type, "service_unavailable", dmw::Qos{});
        assert(publisher);
        assert(subscriber);
        assert(client);
        assert(server);
        assert(unavailable_client);
        auto wait_set = context.value()->create_wait_set();
        assert(wait_set);
        assert(wait_set.value()->add(*subscriber.value()));

        bool service_available = false;
        for (int attempt = 0; attempt < 100 && !service_available; ++attempt) {
            const auto available = client.value()->service_is_available();
            assert(available);
            service_available = available.value();
            if (!service_available) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        assert(service_available);
        const int pending_request = static_cast<int>(iteration) + 1000;
        const auto written_request = client.value()->write_request(&pending_request);
        assert(written_request);
        dmw::RequestId pending_request_id;
        bool request_received = false;
        for (int attempt = 0; attempt < 100 && !request_received; ++attempt) {
            int received_request = 0;
            const auto read_request =
                server.value()->read_request(&received_request, pending_request_id);
            assert(read_request);
            request_received = read_request.value();
            if (request_received) {
                assert(received_request == pending_request);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        assert(request_received);
        assert(pending_request_id == written_request.value());

        // Destroy a reader while its matched writer is actively publishing.
        // This drives listener teardown against in-flight DDS delivery rather
        // than only after the transport has gone idle.
        auto transient_publisher =
            node.value()->create_publisher(message_type.value(), "transient_topic", dmw::Qos{});
        auto transient_subscriber =
            node.value()->create_subscriber(message_type.value(), "transient_topic", dmw::Qos{});
        assert(transient_publisher && transient_subscriber);
        bool transient_matched = false;
        for (int attempt = 0; attempt < 100 && !transient_matched; ++attempt) {
            const auto matched = transient_publisher.value()->matched_subscriber_count();
            assert(matched);
            transient_matched = matched.value() != 0;
            if (!transient_matched) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        assert(transient_matched);
        std::atomic<bool> keep_publishing{true};
        std::thread writer([&] {
            int value = 0;
            while (keep_publishing.load(std::memory_order_acquire)) {
                assert(transient_publisher.value()->write(&value));
                ++value;
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        transient_subscriber.value().reset();
        keep_publishing.store(false, std::memory_order_release);
        writer.join();

        // Exercise active data flow, then shutdown an infinite reader wait
        // before endpoint destruction.  This catches teardown paths that a
        // create/destroy-only loop cannot reach.
        const int sample = static_cast<int>(iteration);
        assert(publisher.value()->write(&sample));
        auto ready_timeout = dmw::WaitTimeout::finite(std::chrono::seconds(1));
        assert(ready_timeout);
        auto ready_wait = wait_set.value()->wait(ready_timeout.value());
        assert(ready_wait && ready_wait.value().status() == dmw::WaitStatus::Ready);
        int received = -1;
        dmw::MessageInfo message_info;
        const auto read = subscriber.value()->read(&received, message_info);
        assert(read && read.value() && received == sample);
        auto shutdown_wait = std::async(std::launch::async, [&] {
            return wait_set.value()->wait(dmw::WaitTimeout::infinite());
        });
        auto service_shutdown_wait = std::async(std::launch::async, [&] {
            return unavailable_client.value()->wait_for_service(dmw::WaitTimeout::infinite());
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::atomic<bool> start_shutdown{false};
        auto first_shutdown = std::async(std::launch::async, [&] {
            while (!start_shutdown.load(std::memory_order_acquire)) std::this_thread::yield();
            return context.value()->shutdown();
        });
        auto second_shutdown = std::async(std::launch::async, [&] {
            while (!start_shutdown.load(std::memory_order_acquire)) std::this_thread::yield();
            return context.value()->shutdown();
        });
        start_shutdown.store(true, std::memory_order_release);
        assert(first_shutdown.get());
        assert(second_shutdown.get());
        assert(shutdown_wait.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        const auto shutdown_result = shutdown_wait.get();
        assert(!shutdown_result);
        assert(shutdown_result.error().code() == dmw::ErrorCode::ContextShutdown);
        assert(
            service_shutdown_wait.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        const auto service_shutdown_result = service_shutdown_wait.get();
        assert(!service_shutdown_result);
        assert(service_shutdown_result.error().code() == dmw::ErrorCode::ContextShutdown);
    }

    return 0;
}
