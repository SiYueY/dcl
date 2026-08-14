#include <cassert>
#include <cstdint>
#include <cstring>

#include <fastdds/rtps/common/Guid.h>
#include <fastdds/rtps/common/SequenceNumber.h>

#include "impl/fastdds/identity.hpp"

int main() {
    eprosima::fastrtps::rtps::GUID_t guid;
    for (std::size_t index = 0; index < 12; ++index) {
        guid.guidPrefix.value[index] = static_cast<eprosima::fastrtps::rtps::octet>(index);
    }
    for (std::size_t index = 0; index < 4; ++index) {
        guid.entityId.value[index] = static_cast<eprosima::fastrtps::rtps::octet>(index + 12);
    }
    const auto gid = dmw::impl::fastdds::to_gid(guid);
    for (std::size_t index = 0; index < gid.data.size(); ++index) {
        assert(gid.data[index] == index);
    }

    assert(!dmw::impl::fastdds::publication_sequence_number(
        eprosima::fastrtps::rtps::c_SequenceNumber_Unknown));
    const eprosima::fastrtps::rtps::SequenceNumber_t sequence(2, 17);
    const auto converted = dmw::impl::fastdds::publication_sequence_number(sequence);
    assert(converted);
    assert(*converted == static_cast<std::uint64_t>(sequence.to64long()));

    const eprosima::fastrtps::rtps::SequenceNumber_t negative_sequence(-2, 23);
    const auto value = dmw::impl::fastdds::from_fastdds_sequence(negative_sequence);
    const auto round_trip = dmw::impl::fastdds::to_fastdds_sequence(value);
    assert(round_trip.high == negative_sequence.high);
    assert(round_trip.low == negative_sequence.low);

    eprosima::fastrtps::rtps::SampleIdentity identity;
    identity.sequence_number(sequence);
    const auto request_id = dmw::impl::fastdds::request_id_from_identity(identity);
    assert(request_id);
    assert(request_id->sequence_number == dmw::impl::fastdds::from_fastdds_sequence(sequence));
    identity.sequence_number(eprosima::fastrtps::rtps::c_SequenceNumber_Unknown);
    assert(!dmw::impl::fastdds::request_id_from_identity(identity));
    return 0;
}
