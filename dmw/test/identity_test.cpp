#include <cassert>
#include <cstdint>
#include <cstring>

#include <fastdds/rtps/common/Guid.h>
#include <fastdds/rtps/common/SequenceNumber.h>

#include "impl/identity.hpp"

int main() {
    eprosima::fastrtps::rtps::GUID_t guid;
    for (std::size_t index = 0; index < 12; ++index) {
        guid.guidPrefix.value[index] = static_cast<eprosima::fastrtps::rtps::octet>(index);
    }
    for (std::size_t index = 0; index < 4; ++index) {
        guid.entityId.value[index] = static_cast<eprosima::fastrtps::rtps::octet>(index + 12);
    }
    const auto gid = dmw::impl::to_gid(guid);
    for (std::size_t index = 0; index < gid.data.size(); ++index) {
        assert(gid.data[index] == index);
    }

    assert(dmw::impl::to_writer_sequence(eprosima::fastrtps::rtps::c_SequenceNumber_Unknown) == 0U);
    const eprosima::fastrtps::rtps::SequenceNumber_t sequence(2, 17);
    const auto converted = dmw::impl::to_writer_sequence(sequence);
    assert(converted == static_cast<std::uint64_t>(sequence.to64long()));

    const eprosima::fastrtps::rtps::SequenceNumber_t negative_sequence(-2, 23);
    const auto value = dmw::impl::from_sequence(negative_sequence);
    const auto round_trip = dmw::impl::to_sequence(value);
    assert(round_trip.high == negative_sequence.high);
    assert(round_trip.low == negative_sequence.low);

    eprosima::fastrtps::rtps::SampleIdentity identity;
    identity.sequence_number(sequence);
    const auto request_id = dmw::impl::to_request_id(identity);
    assert(request_id);
    assert(request_id->sequence_number == dmw::impl::from_sequence(sequence));
    identity.sequence_number(eprosima::fastrtps::rtps::c_SequenceNumber_Unknown);
    assert(!dmw::impl::to_request_id(identity));
    return 0;
}
