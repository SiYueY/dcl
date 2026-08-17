#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "impl/response.hpp"

namespace {

eprosima::fastdds::dds::InstanceHandle_t make_handle(std::uint8_t participant_id) {
    eprosima::fastdds::dds::InstanceHandle_t handle;
    for (std::size_t index = 0; index < 12; ++index) {
        handle.value[index] = participant_id;
    }
    for (std::size_t index = 12; index < 16; ++index) {
        handle.value[index] = static_cast<std::uint8_t>(index);
    }
    return handle;
}

}  // namespace

int main() {
    dmw::impl::RequestState state;
    const auto participant_a = make_handle(1);
    const auto participant_b = make_handle(2);

    state.observe_request_reader(participant_a, 1);
    assert(!state.is_available());

    state.observe_response_writer(participant_b, 1);
    assert(!state.is_available());

    state.observe_response_writer(participant_a, 1);
    assert(state.is_available());

    state.observe_request_reader(participant_a, -1);
    assert(!state.is_available());

    state.observe_request_reader(participant_a, 2);
    state.observe_response_writer(participant_a, 2);
    state.observe_response_writer(participant_a, -1);
    assert(state.is_available());
    state.observe_response_writer(participant_a, -1);
    state.observe_response_writer(participant_a, -1);
    assert(!state.is_available());

    auto observations = std::make_shared<dmw::impl::ParticipantObservationRegistry>();
    dmw::impl::RequestState tombstone_state(observations);
    tombstone_state.observe_request_reader(participant_a, 1);
    tombstone_state.observe_response_writer(participant_a, 1);
    assert(tombstone_state.is_available());
    const auto prefix = eprosima::fastrtps::rtps::iHandle2GUID(participant_a).guidPrefix;
    assert(observations->observe(prefix, true));
    assert(!tombstone_state.is_available());

    // GuidPrefix reuse is outside the V1 deployment contract.  A tombstone
    // never becomes Active again during one Context lifetime; attempting to
    // recreate it makes discovery unusable rather than reviving stale
    // endpoint/response-target observations.
    auto reuse_observations = std::make_shared<dmw::impl::ParticipantObservationRegistry>();
    const auto reuse_entry = reuse_observations->observe(prefix, true);
    assert(reuse_entry);
    const auto reused_entry = reuse_observations->observe(prefix, false);
    assert(reused_entry == reuse_entry);
    assert(
        reused_entry->lifecycle.load(std::memory_order_acquire) ==
        dmw::impl::ParticipantLifecycle::Removed);
    assert(reuse_observations->capability() == dmw::impl::DiscoveryCapability::Degraded);

    auto exact_observations = std::make_shared<dmw::impl::ParticipantObservationRegistry>();
    auto endpoints = std::make_shared<dmw::impl::RemoteEndpointRegistry>();
    dmw::impl::RequestState exact_state(
        exact_observations, endpoints, "rq", "RequestType", "rs", "ResponseType");
    exact_state.observe_request_reader(participant_a, 1);
    exact_state.observe_response_writer(participant_a, 1);
    assert(!exact_state.is_available());
    const auto exact_prefix = eprosima::fastrtps::rtps::iHandle2GUID(participant_a).guidPrefix;
    const auto exact_participant = exact_observations->observe(exact_prefix, false);
    auto request_reader = eprosima::fastrtps::rtps::iHandle2GUID(participant_a);
    request_reader.entityId.value[3] = 4;
    auto response_writer = request_reader;
    response_writer.entityId.value[3] = 3;
    endpoints->observe(
        request_reader, dmw::impl::RemoteEndpointKind::Reader, "rq", "RequestType",
        exact_participant, false);
    assert(!exact_state.is_available());
    endpoints->observe(
        response_writer, dmw::impl::RemoteEndpointKind::Writer, "rs", "ResponseType",
        exact_participant, false);
    assert(exact_state.is_available());
    endpoints->observe(
        response_writer, dmw::impl::RemoteEndpointKind::Writer, "rs", "ResponseType",
        exact_participant, true);
    assert(!exact_state.is_available());
    endpoints->observe(
        response_writer, dmw::impl::RemoteEndpointKind::Writer, "rs", "ResponseType",
        exact_participant, false);
    assert(exact_state.is_degraded());

    state.degrade();
    assert(state.is_degraded());
    assert(!state.is_available());

    dmw::impl::ResponseState response_state;
    const auto reader = make_handle(3);
    const auto reader_guid = eprosima::fastrtps::rtps::iHandle2GUID(reader);
    response_state.observe_reader(reader, 1);
    assert(
        response_state.wait_for_target(reader_guid, std::chrono::steady_clock::now()) ==
        dmw::impl::ResponseState::TargetStatus::Ready);
    response_state.observe_reader(reader, -1);
    assert(
        response_state.wait_for_target(reader_guid, std::chrono::steady_clock::now()) ==
        dmw::impl::ResponseState::TargetStatus::TimedOut);
    response_state.degrade();
    assert(
        response_state.wait_for_target(reader_guid, std::chrono::steady_clock::now()) ==
        dmw::impl::ResponseState::TargetStatus::Unavailable);

    auto response_observations = std::make_shared<dmw::impl::ParticipantObservationRegistry>();
    dmw::impl::ResponseState participant_removed_state(response_observations);
    const auto removed_reader = eprosima::fastrtps::rtps::iHandle2GUID(make_handle(4));
    assert(response_observations->observe(removed_reader.guidPrefix, true));
    assert(
        participant_removed_state.wait_for_target(
            removed_reader, std::chrono::steady_clock::now()) ==
        dmw::impl::ResponseState::TargetStatus::Removed);

    auto target_observations = std::make_shared<dmw::impl::ParticipantObservationRegistry>();
    auto target_readers = std::make_shared<dmw::impl::TargetReaderObservationRegistry>();
    dmw::impl::ResponseState endpoint_removed_state(target_observations, target_readers);
    const auto target_reader = eprosima::fastrtps::rtps::iHandle2GUID(make_handle(6));
    const auto target_participant = target_observations->observe(target_reader.guidPrefix, false);
    target_readers->observe(target_reader, target_participant, false);
    assert(
        endpoint_removed_state.wait_for_target(target_reader, std::chrono::steady_clock::now()) ==
        dmw::impl::ResponseState::TargetStatus::TimedOut);
    target_readers->observe(target_reader, target_participant, true);
    assert(
        endpoint_removed_state.wait_for_target(target_reader, std::chrono::steady_clock::now()) ==
        dmw::impl::ResponseState::TargetStatus::Removed);

    auto target_wake_observations = std::make_shared<dmw::impl::ParticipantObservationRegistry>();
    auto target_wake_readers = std::make_shared<dmw::impl::TargetReaderObservationRegistry>();
    auto target_wake_state =
        std::make_shared<dmw::impl::ResponseState>(target_wake_observations, target_wake_readers);
    target_wake_readers->add_dependency_wake([weak = std::weak_ptr(target_wake_state)] {
        if (const auto state = weak.lock()) state->notify_dependency_change();
    });
    const auto target_wake_reader = eprosima::fastrtps::rtps::iHandle2GUID(make_handle(7));
    const auto target_wake_participant =
        target_wake_observations->observe(target_wake_reader.guidPrefix, false);
    dmw::impl::ResponseState::TargetStatus target_wake_status{
        dmw::impl::ResponseState::TargetStatus::TimedOut};
    std::thread target_waiter([&] {
        target_wake_status = target_wake_state->wait_for_target(
            target_wake_reader, std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    target_wake_readers->observe_noexcept(target_wake_reader, target_wake_participant, true);
    target_waiter.join();
    assert(target_wake_status == dmw::impl::ResponseState::TargetStatus::Removed);
    target_wake_readers->observe(target_wake_reader, target_wake_participant, false);
    assert(
        target_wake_state->wait_for_target(target_wake_reader, std::chrono::steady_clock::now()) ==
        dmw::impl::ResponseState::TargetStatus::Unavailable);

    auto waking_observations = std::make_shared<dmw::impl::ParticipantObservationRegistry>();
    auto waking_state = std::make_shared<dmw::impl::ResponseState>(waking_observations);
    waking_observations->add_dependency_wake([weak = std::weak_ptr(waking_state)] {
        if (const auto state = weak.lock()) state->notify_dependency_change();
    });
    const auto waking_reader = eprosima::fastrtps::rtps::iHandle2GUID(make_handle(5));
    dmw::impl::ResponseState::TargetStatus waking_status{
        dmw::impl::ResponseState::TargetStatus::TimedOut};
    std::thread waiter([&] {
        waking_status = waking_state->wait_for_target(
            waking_reader, std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    waking_observations->observe_noexcept(waking_reader.guidPrefix, true);
    waiter.join();
    assert(waking_status == dmw::impl::ResponseState::TargetStatus::Removed);
    return 0;
}
