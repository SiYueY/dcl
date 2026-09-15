#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <typeindex>
#include <vector>

#include "fastdds/dds/topic/TopicDataType.hpp"

#include "dmw/context.hpp"
#include "dmw/fastdds/message_type.hpp"
#include "dmw/guard_condition.hpp"
#include "dmw/publisher.hpp"
#include "dmw/subscriber.hpp"
#include "dmw/wait_set.hpp"

namespace {

constexpr std::size_t kSamples = 200;
constexpr std::size_t kWaitIterations = 1000;

class IntTopicDataType final : public eprosima::fastdds::dds::TopicDataType {
public:
    IntTopicDataType() {
        m_typeSize = sizeof(std::int32_t);
        m_isGetKeyDefined = false;
        setName("dmw.benchmark.Int");
    }

    bool serialize(void* data, eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
        payload->length = sizeof(std::int32_t);
        std::memcpy(payload->data, data, sizeof(std::int32_t));
        return true;
    }

    bool deserialize(eprosima::fastrtps::rtps::SerializedPayload_t* payload, void* data) override {
        if (payload->length != sizeof(std::int32_t)) return false;
        std::memcpy(data, payload->data, sizeof(std::int32_t));
        return true;
    }

    std::function<std::uint32_t()> getSerializedSizeProvider(void*) override {
        return [] { return static_cast<std::uint32_t>(sizeof(std::int32_t)); };
    }

    void* createData() override { return new std::int32_t(0); }
    void deleteData(void* data) override { delete static_cast<std::int32_t*>(data); }
    bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override { return false; }
};

template <class Predicate>
bool wait_until(Predicate&& predicate) {
    for (std::size_t attempt = 0; attempt < 1000; ++attempt) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

double percentile(std::vector<double> values, double fraction) {
    assert(!values.empty());
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1));
    return values[index];
}

template <class Operation>
double mean_nanoseconds(std::size_t iterations, Operation&& operation) {
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < iterations; ++index) operation();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
           static_cast<double>(iterations);
}

}  // namespace

int main() {
    auto type = dmw::fastdds::create_message_type<IntTopicDataType>();
    assert(type);
    dmw::ContextOptions context_options;
    context_options.participant_name = "dmw-foundation-benchmark";
    auto context = dmw::Context::create(context_options);
    assert(context);
    dmw::NodeOptions node_options;
    node_options.node_name = "foundation_benchmark";
    auto node = context.value()->create_node(node_options);
    assert(node);
    auto publisher = node.value()->create_publisher(type.value(), "/dmw_foundation_benchmark", dmw::Qos{});
    auto subscriber = node.value()->create_subscriber(type.value(), "/dmw_foundation_benchmark", dmw::Qos{});
    assert(publisher);
    assert(subscriber);
    assert(wait_until([&] {
        const auto count = publisher.value()->matched_subscriber_count();
        return count && count.value() != 0;
    }));

    std::vector<double> latencies_us;
    latencies_us.reserve(kSamples);
    for (std::int32_t value = 0; value < static_cast<std::int32_t>(kSamples); ++value) {
        const auto start = std::chrono::steady_clock::now();
        assert(publisher.value()->write(&value));
        std::int32_t received = -1;
        dmw::MessageInfo info;
        assert(wait_until([&] {
            const auto read = subscriber.value()->read(&received, info);
            return read && read.value() && received == value;
        }));
        const auto elapsed = std::chrono::steady_clock::now() - start;
        latencies_us.push_back(
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
            1000.0);
    }

    auto wait_set = context.value()->create_wait_set();
    auto guard = context.value()->create_guard_condition();
    assert(wait_set);
    assert(guard);
    assert(wait_set.value()->add(*guard.value()));
    const auto idle_poll_ns = mean_nanoseconds(kWaitIterations, [&] {
        const auto result = wait_set.value()->wait(dmw::WaitTimeout::poll());
        assert(result && result.value().status() == dmw::WaitStatus::Timeout);
    });
    const auto ready_guard_ns = mean_nanoseconds(kWaitIterations, [&] {
        assert(guard.value()->trigger());
        const auto result = wait_set.value()->wait(dmw::WaitTimeout::poll());
        assert(result && result.value().status() == dmw::WaitStatus::Ready);
    });

    std::cout << "dmw_benchmark pub_sub_latency_us p50=" << percentile(latencies_us, 0.50)
              << " p95=" << percentile(latencies_us, 0.95)
              << " samples=" << kSamples << '\n'
              << "dmw_benchmark wait_set_ns idle_poll_mean=" << idle_poll_ns
              << " ready_guard_mean=" << ready_guard_ns
              << " iterations=" << kWaitIterations << '\n';
    return 0;
}
