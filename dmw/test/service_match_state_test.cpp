#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "impl/service_match_state.hpp"

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
    dmw::impl::ServiceMatchState matches;
    const auto participant_a = make_handle(1);
    const auto participant_b = make_handle(2);

    matches.update_request(participant_a, 1);
    assert(!matches.has_candidate());

    matches.update_response(participant_b, 1);
    assert(!matches.has_candidate());

    matches.update_response(participant_a, 1);
    assert(matches.has_candidate());

    matches.update_request(participant_a, -1);
    assert(!matches.has_candidate());

    matches.update_request(participant_a, 2);
    matches.update_response(participant_a, 2);
    matches.update_response(participant_a, -1);
    assert(matches.has_candidate());
    matches.update_response(participant_a, -1);
    matches.update_response(participant_a, -1);
    assert(!matches.has_candidate());

    auto observations = std::make_shared<dmw::impl::ParticipantObservationRegistry>();
    dmw::impl::ServiceMatchState tombstone_matches(observations);
    tombstone_matches.update_request(participant_a, 1);
    tombstone_matches.update_response(participant_a, 1);
    assert(tombstone_matches.has_candidate());
    const auto prefix = eprosima::fastrtps::rtps::iHandle2GUID(participant_a).guidPrefix;
    assert(observations->observe(prefix, true));
    assert(!tombstone_matches.has_candidate());

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
    dmw::impl::ServiceMatchState exact_matches(
        exact_observations, endpoints, "rq", "RequestType", "rs", "ResponseType");
    exact_matches.update_request(participant_a, 1);
    exact_matches.update_response(participant_a, 1);
    assert(!exact_matches.has_candidate());
    const auto exact_prefix = eprosima::fastrtps::rtps::iHandle2GUID(participant_a).guidPrefix;
    const auto exact_participant = exact_observations->observe(exact_prefix, false);
    auto request_reader = eprosima::fastrtps::rtps::iHandle2GUID(participant_a);
    request_reader.entityId.value[3] = 4;
    auto response_writer = request_reader;
    response_writer.entityId.value[3] = 3;
    endpoints->observe(
        request_reader, dmw::impl::RemoteEndpointKind::Reader, "rq", "RequestType",
        exact_participant, false);
    assert(!exact_matches.has_candidate());
    endpoints->observe(
        response_writer, dmw::impl::RemoteEndpointKind::Writer, "rs", "ResponseType",
        exact_participant, false);
    assert(exact_matches.has_candidate());
    endpoints->observe(
        response_writer, dmw::impl::RemoteEndpointKind::Writer, "rs", "ResponseType",
        exact_participant, true);
    assert(!exact_matches.has_candidate());
    endpoints->observe(
        response_writer, dmw::impl::RemoteEndpointKind::Writer, "rs", "ResponseType",
        exact_participant, false);
    assert(exact_matches.is_degraded());

    matches.degrade();
    assert(matches.is_degraded());
    assert(!matches.has_candidate());

    dmw::impl::ResponseWriterMatchState response_matches;
    const auto reader = make_handle(3);
    const auto reader_guid = eprosima::fastrtps::rtps::iHandle2GUID(reader);
    response_matches.update(reader, 1);
    assert(
        response_matches.wait_for_match(reader_guid, std::chrono::steady_clock::now()) ==
        dmw::impl::ResponseWriterMatchState::WaitStatus::Matched);
    response_matches.update(reader, -1);
    assert(
        response_matches.wait_for_match(reader_guid, std::chrono::steady_clock::now()) ==
        dmw::impl::ResponseWriterMatchState::WaitStatus::Timeout);
    response_matches.degrade();
    assert(
        response_matches.wait_for_match(reader_guid, std::chrono::steady_clock::now()) ==
        dmw::impl::ResponseWriterMatchState::WaitStatus::Degraded);

    auto response_observations = std::make_shared<dmw::impl::ParticipantObservationRegistry>();
    dmw::impl::ResponseWriterMatchState participant_removed_matches(response_observations);
    const auto removed_reader = eprosima::fastrtps::rtps::iHandle2GUID(make_handle(4));
    assert(response_observations->observe(removed_reader.guidPrefix, true));
    assert(
        participant_removed_matches.wait_for_match(
            removed_reader, std::chrono::steady_clock::now()) ==
        dmw::impl::ResponseWriterMatchState::WaitStatus::Removed);

    auto target_observations = std::make_shared<dmw::impl::ParticipantObservationRegistry>();
    auto target_readers = std::make_shared<dmw::impl::TargetReaderObservationRegistry>();
    dmw::impl::ResponseWriterMatchState endpoint_removed_matches(
        target_observations, target_readers);
    const auto target_reader = eprosima::fastrtps::rtps::iHandle2GUID(make_handle(6));
    const auto target_participant = target_observations->observe(target_reader.guidPrefix, false);
    target_readers->observe(target_reader, target_participant, false);
    assert(
        endpoint_removed_matches.wait_for_match(target_reader, std::chrono::steady_clock::now()) ==
        dmw::impl::ResponseWriterMatchState::WaitStatus::Timeout);
    target_readers->observe(target_reader, target_participant, true);
    assert(
        endpoint_removed_matches.wait_for_match(target_reader, std::chrono::steady_clock::now()) ==
        dmw::impl::ResponseWriterMatchState::WaitStatus::Removed);

    auto target_wake_observations = std::make_shared<dmw::impl::ParticipantObservationRegistry>();
    auto target_wake_readers = std::make_shared<dmw::impl::TargetReaderObservationRegistry>();
    auto target_wake_matches = std::make_shared<dmw::impl::ResponseWriterMatchState>(
        target_wake_observations, target_wake_readers);
    target_wake_readers->add_dependency_wake([weak = std::weak_ptr(target_wake_matches)] {
        if (const auto state = weak.lock()) state->notify_dependency_change();
    });
    const auto target_wake_reader = eprosima::fastrtps::rtps::iHandle2GUID(make_handle(7));
    const auto target_wake_participant =
        target_wake_observations->observe(target_wake_reader.guidPrefix, false);
    dmw::impl::ResponseWriterMatchState::WaitStatus target_wake_status{
        dmw::impl::ResponseWriterMatchState::WaitStatus::Timeout};
    std::thread target_waiter([&] {
        target_wake_status = target_wake_matches->wait_for_match(
            target_wake_reader, std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    target_wake_readers->observe_noexcept(target_wake_reader, target_wake_participant, true);
    target_waiter.join();
    assert(target_wake_status == dmw::impl::ResponseWriterMatchState::WaitStatus::Removed);
    target_wake_readers->observe(target_wake_reader, target_wake_participant, false);
    assert(
        target_wake_matches->wait_for_match(target_wake_reader, std::chrono::steady_clock::now()) ==
        dmw::impl::ResponseWriterMatchState::WaitStatus::Degraded);

    auto waking_observations = std::make_shared<dmw::impl::ParticipantObservationRegistry>();
    auto waking_matches =
        std::make_shared<dmw::impl::ResponseWriterMatchState>(waking_observations);
    waking_observations->add_dependency_wake([weak = std::weak_ptr(waking_matches)] {
        if (const auto state = weak.lock()) state->notify_dependency_change();
    });
    const auto waking_reader = eprosima::fastrtps::rtps::iHandle2GUID(make_handle(5));
    dmw::impl::ResponseWriterMatchState::WaitStatus waking_status{
        dmw::impl::ResponseWriterMatchState::WaitStatus::Timeout};
    std::thread waiter([&] {
        waking_status = waking_matches->wait_for_match(
            waking_reader, std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    waking_observations->observe_noexcept(waking_reader.guidPrefix, true);
    waiter.join();
    assert(waking_status == dmw::impl::ResponseWriterMatchState::WaitStatus::Removed);
    return 0;
}
