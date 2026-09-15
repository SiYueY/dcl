#include <cassert>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <thread>
#include <cstring>
#include <functional>
#include <string>
#include <typeindex>

#include "fastdds/dds/topic/TopicDataType.hpp"
#include "dmw/context.hpp"
#include "dmw/client.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/guard_condition.hpp"
#include "dmw/publisher.hpp"
#include "dmw/server.hpp"
#include "dmw/service_type.hpp"
#include "dmw/subscriber.hpp"
#include "dmw/wait_set.hpp"

namespace {

using namespace std::chrono_literals;

class IntTopicDataType final : public eprosima::fastdds::dds::TopicDataType {
public:
    IntTopicDataType() {
        m_typeSize = sizeof(int);
        setName("dmw.test.WaitSetTopologyInt");
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

template <typename Predicate>
bool wait_until(Predicate&& predicate) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

void assert_ready_for(
    std::future<dmw::Result<dmw::WaitResult>>& result,
    const dmw::WaitableRegistration& registration) {
    assert(result.wait_for(2s) == std::future_status::ready);
    auto wait_result = result.get();
    assert(wait_result);
    assert(wait_result.value().status() == dmw::WaitStatus::Ready);
    assert(wait_result.value().ready().size() == 1);
    assert(wait_result.value().ready().front() == registration);
}

}  // namespace

int main() {
    if (std::getenv("DMW_ENABLE_DDS_INTEGRATION") == nullptr) return 0;

    dmw::ContextOptions options;
    options.participant_name = "dmw-wait-set-test";
    auto context = dmw::Context::create(options);
    assert(context);

    auto wait_set = context.value()->create_wait_set();
    auto guard = context.value()->create_guard_condition();
    assert(wait_set);
    assert(guard);
    auto registration = wait_set.value()->add(*guard.value());
    assert(registration);

    auto guard_wait = std::async(std::launch::async, [&] {
        return wait_set.value()->wait(dmw::WaitTimeout::infinite());
    });
    std::this_thread::sleep_for(20ms);
    assert(guard.value()->trigger());
    assert_ready_for(guard_wait, registration.value());

    // A WaitSet blocks while empty. Adding a waitable and triggering it must
    // wake that wait without requiring a polling interval.
    auto topology_wait_set = context.value()->create_wait_set();
    auto topology_guard = context.value()->create_guard_condition();
    assert(topology_wait_set);
    assert(topology_guard);
    auto topology_wait = std::async(std::launch::async, [&] {
        return topology_wait_set.value()->wait(dmw::WaitTimeout::infinite());
    });
    std::this_thread::sleep_for(20ms);
    auto topology_registration = topology_wait_set.value()->add(*topology_guard.value());
    assert(topology_registration);
    assert(topology_guard.value()->trigger());
    assert_ready_for(topology_wait, topology_registration.value());

    // Repeatedly add a real reader while another thread is in an infinite
    // native wait.  The add must hand off the control wake to native topology
    // reconciliation, then the first sample must wake the original wait.
    auto type = dmw::fastdds::create_message_type<IntTopicDataType, int>();
    assert(type);
    auto node = context.value()->create_node(dmw::NodeOptions{"wait_set_topology"});
    assert(node);
    for (int iteration = 0; iteration < 32; ++iteration) {
        const auto topic = "/dmw_wait_set_topology_" + std::to_string(iteration);
        auto publisher = node.value()->create_publisher(type.value(), topic, dmw::Qos{});
        auto subscriber = node.value()->create_subscriber(type.value(), topic, dmw::Qos{});
        auto reader_wait_set = context.value()->create_wait_set();
        assert(publisher && subscriber && reader_wait_set);
        assert(wait_until([&] {
            const auto matched = publisher.value()->matched_subscriber_count();
            return matched && matched.value() != 0;
        }));
        auto reader_wait = std::async(std::launch::async, [&] {
            return reader_wait_set.value()->wait(dmw::WaitTimeout::infinite());
        });
        std::this_thread::sleep_for(20ms);
        auto reader_registration = reader_wait_set.value()->add(*subscriber.value());
        assert(reader_registration);
        const int value = iteration;
        assert(publisher.value()->write(&value));
        assert_ready_for(reader_wait, reader_registration.value());
        assert(reader_wait_set.value()->remove(reader_registration.value()));
    }

    // A reader may close itself while another thread is blocked in an
    // infinite wait.  Its auto-detach must reconcile the native topology;
    // the remaining guard must still be able to wake that same wait.
    auto closing_publisher =
        node.value()->create_publisher(type.value(), "/dmw_wait_set_close_reader", dmw::Qos{});
    auto closing_subscriber =
        node.value()->create_subscriber(type.value(), "/dmw_wait_set_close_reader", dmw::Qos{});
    auto closing_wait_set = context.value()->create_wait_set();
    auto closing_guard = context.value()->create_guard_condition();
    assert(closing_publisher && closing_subscriber && closing_wait_set && closing_guard);
    const auto closing_guard_token = closing_wait_set.value()->add(*closing_guard.value());
    const auto closing_reader_token = closing_wait_set.value()->add(*closing_subscriber.value());
    assert(closing_guard_token && closing_reader_token);
    auto closing_wait = std::async(std::launch::async, [&] {
        return closing_wait_set.value()->wait(dmw::WaitTimeout::infinite());
    });
    std::this_thread::sleep_for(20ms);
    closing_subscriber.value().reset();
    assert(closing_guard.value()->trigger());
    assert_ready_for(closing_wait, closing_guard_token.value());
    const auto stale_closing_reader =
        closing_wait_set.value()->remove(closing_reader_token.value());
    assert(!stale_closing_reader);
    assert(stale_closing_reader.error().code() == dmw::ErrorCode::NotRegistered);

    // Client and Server readers use the same native reconciliation path as a
    // Subscriber.  Exercise both add and remove while an infinite wait owns
    // the native WaitSet, using a guard solely to complete the wait.
    const dmw::ServiceType service_type(type.value(), type.value());
    auto client = node.value()->create_client(service_type, "wait_set_client", dmw::Qos{});
    auto server = node.value()->create_server(service_type, "wait_set_server", dmw::Qos{});
    assert(client && server);
    for (int iteration = 0; iteration < 8; ++iteration) {
        auto endpoint_wait_set = context.value()->create_wait_set();
        auto endpoint_guard = context.value()->create_guard_condition();
        assert(endpoint_wait_set && endpoint_guard);
        const auto guard_token = endpoint_wait_set.value()->add(*endpoint_guard.value());
        assert(guard_token);
        auto endpoint_wait = std::async(std::launch::async, [&] {
            return endpoint_wait_set.value()->wait(dmw::WaitTimeout::infinite());
        });
        std::this_thread::sleep_for(20ms);
        auto endpoint_token = iteration % 2 == 0 ? endpoint_wait_set.value()->add(*client.value())
                                                 : endpoint_wait_set.value()->add(*server.value());
        assert(endpoint_token);
        assert(endpoint_guard.value()->trigger());
        assert_ready_for(endpoint_wait, guard_token.value());
        assert(endpoint_wait_set.value()->remove(endpoint_token.value()));
    }

    auto active_wait = std::async(std::launch::async, [&] {
        return wait_set.value()->wait(dmw::WaitTimeout::infinite());
    });
    std::this_thread::sleep_for(20ms);
    auto concurrent_wait = wait_set.value()->wait(dmw::WaitTimeout::poll());
    assert(!concurrent_wait);
    assert(concurrent_wait.error().code() == dmw::ErrorCode::Busy);
    assert(guard.value()->trigger());
    assert_ready_for(active_wait, registration.value());

    auto shutdown_wait_set = context.value()->create_wait_set();
    assert(shutdown_wait_set);
    auto shutdown_wait = std::async(std::launch::async, [&] {
        return shutdown_wait_set.value()->wait(dmw::WaitTimeout::infinite());
    });
    std::this_thread::sleep_for(20ms);
    assert(context.value()->shutdown());
    assert(shutdown_wait.wait_for(2s) == std::future_status::ready);
    auto shutdown_result = shutdown_wait.get();
    assert(!shutdown_result);
    assert(shutdown_result.error().code() == dmw::ErrorCode::ContextShutdown);

    return 0;
}
