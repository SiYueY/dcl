#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "impl/response.hpp"

namespace {
eprosima::fastdds::dds::InstanceHandle_t make_handle(std::uint8_t participant_id) {
    eprosima::fastdds::dds::InstanceHandle_t handle;
    for (std::size_t index = 0; index < 12; ++index) handle.value[index] = participant_id;
    for (std::size_t index = 12; index < 16; ++index) handle.value[index] = index;
    return handle;
}
}  // namespace

int main() {
    const auto participant_a = make_handle(1);
    const auto participant_b = make_handle(2);
    dmw::impl::RequestState local_state;
    local_state.observe_request_reader(participant_a, 1);
    local_state.observe_response_writer(participant_b, 1);
    assert(!local_state.is_available());
    local_state.observe_response_writer(participant_a, 1);
    assert(local_state.is_available());

    const auto prefix = eprosima::fastrtps::rtps::iHandle2GUID(participant_a).guidPrefix;
    auto graph = std::make_shared<dmw::impl::DiscoveryGraph>();
    dmw::impl::RequestState request_state(graph, "rq", "RequestType", "rs", "ResponseType");
    request_state.observe_request_reader(participant_a, 1);
    request_state.observe_response_writer(participant_a, 1);
    assert(!request_state.is_available());
    graph->apply_participant(prefix, dmw::impl::DiscoveryChange::Added);
    auto request_reader = eprosima::fastrtps::rtps::iHandle2GUID(participant_a);
    request_reader.entityId.value[3] = 4;
    auto response_writer = request_reader;
    response_writer.entityId.value[3] = 3;
    graph->apply_endpoint(
        request_reader, dmw::impl::EndpointKind::Reader, "rq", "RequestType",
        dmw::impl::DiscoveryChange::Added);
    assert(!request_state.is_available());
    graph->apply_endpoint(
        response_writer, dmw::impl::EndpointKind::Writer, "rs", "ResponseType",
        dmw::impl::DiscoveryChange::Added);
    assert(request_state.is_available());
    graph->apply_endpoint(
        response_writer, dmw::impl::EndpointKind::Writer, "rs", "ResponseType",
        dmw::impl::DiscoveryChange::Removed);
    assert(!request_state.is_available());
    graph->apply_endpoint(
        response_writer, dmw::impl::EndpointKind::Writer, "rs", "ResponseType",
        dmw::impl::DiscoveryChange::Added);
    assert(graph->health() == dmw::impl::DiscoveryHealth::Unavailable);

    auto response_graph = std::make_shared<dmw::impl::DiscoveryGraph>();
    auto response_state = std::make_shared<dmw::impl::ResponseState>(response_graph);
    response_state->subscribe_to_graph();
    const auto reader = make_handle(3);
    const auto reader_guid = eprosima::fastrtps::rtps::iHandle2GUID(reader);
    response_graph->apply_participant(reader_guid.guidPrefix, dmw::impl::DiscoveryChange::Added);
    response_graph->apply_endpoint(
        reader_guid, dmw::impl::EndpointKind::Reader, "rr", "ResponseType",
        dmw::impl::DiscoveryChange::Added);
    response_state->observe_reader(reader, 1);
    assert(
        response_state->wait_for_target(reader_guid, std::chrono::steady_clock::now()) ==
        dmw::impl::ResponseState::TargetStatus::Ready);
    response_graph->apply_endpoint(
        reader_guid, dmw::impl::EndpointKind::Reader, "rr", "ResponseType",
        dmw::impl::DiscoveryChange::Removed);
    assert(
        response_state->check_target(reader_guid) ==
        dmw::impl::ResponseState::TargetStatus::Removed);

    auto wake_graph = std::make_shared<dmw::impl::DiscoveryGraph>();
    auto wake_state = std::make_shared<dmw::impl::ResponseState>(wake_graph);
    wake_state->subscribe_to_graph();
    const auto wake_reader = eprosima::fastrtps::rtps::iHandle2GUID(make_handle(4));
    wake_graph->apply_participant(wake_reader.guidPrefix, dmw::impl::DiscoveryChange::Added);
    wake_graph->apply_endpoint(
        wake_reader, dmw::impl::EndpointKind::Reader, "rr", "ResponseType",
        dmw::impl::DiscoveryChange::Added);
    dmw::impl::ResponseState::TargetStatus status{dmw::impl::ResponseState::TargetStatus::TimedOut};
    std::thread waiter([&] {
        status = wake_state->wait_for_target(
            wake_reader, std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    wake_graph->apply_endpoint(
        wake_reader, dmw::impl::EndpointKind::Reader, "rr", "ResponseType",
        dmw::impl::DiscoveryChange::Removed);
    waiter.join();
    assert(status == dmw::impl::ResponseState::TargetStatus::Removed);

    auto subscription_graph = std::make_shared<dmw::impl::DiscoveryGraph>();
    std::uint64_t observed_revision = 0;
    auto subscription = subscription_graph->subscribe([&](std::uint64_t revision) {
        assert(revision > observed_revision);
        observed_revision = revision;
    });
    subscription_graph->apply_participant(prefix, dmw::impl::DiscoveryChange::Added);
    subscription.close_and_drain();
    subscription_graph->apply_participant(prefix, dmw::impl::DiscoveryChange::Removed);
    assert(observed_revision == 1);
    auto failing_subscription = subscription_graph->subscribe([](std::uint64_t) { throw 1; });
    const auto failure_reader = eprosima::fastrtps::rtps::iHandle2GUID(make_handle(9));
    subscription_graph->apply_endpoint(
        failure_reader, dmw::impl::EndpointKind::Reader, "rq", "RequestType",
        dmw::impl::DiscoveryChange::Added);
    assert(subscription_graph->health() == dmw::impl::DiscoveryHealth::Unavailable);
    return 0;
}
