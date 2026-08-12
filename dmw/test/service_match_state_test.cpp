// SPDX-License-Identifier: Apache-2.0

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>

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
    return 0;
}
