#ifndef DMW_IMPL__FASTDDS__IDENTITY_HPP_
#define DMW_IMPL__FASTDDS__IDENTITY_HPP_

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>

#include <fastdds/rtps/common/Guid.h>
#include <fastdds/rtps/common/SampleIdentity.h>

#include "dmw/gid.hpp"
#include "dmw/request_id.hpp"

namespace dmw {

namespace impl {

using SequenceNumber = eprosima::fastrtps::rtps::SequenceNumber_t;

static_assert(sizeof(std::int32_t) == 4, "DMW requires 32-bit int32_t");
static_assert(sizeof(std::uint32_t) == 4, "DMW requires 32-bit uint32_t");
static_assert(sizeof(std::int64_t) == 8, "DMW requires 64-bit int64_t");
static_assert(sizeof(std::uint64_t) == 8, "DMW requires 64-bit uint64_t");
static_assert(
    std::is_same<decltype(SequenceNumber{}.high), std::int32_t>::value,
    "Unexpected Fast DDS sequence high word type");
static_assert(
    std::is_same<decltype(SequenceNumber{}.low), std::uint32_t>::value,
    "Unexpected Fast DDS sequence low word type");

inline std::int64_t from_sequence(const SequenceNumber& sequence) noexcept {
    std::uint32_t high_bits{};
    std::memcpy(&high_bits, &sequence.high, sizeof(high_bits));
    const auto bits =
        (static_cast<std::uint64_t>(high_bits) << 32U) | static_cast<std::uint64_t>(sequence.low);
    std::int64_t value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline SequenceNumber to_sequence(std::int64_t value) noexcept {
    std::uint64_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));

    const auto high_bits = static_cast<std::uint32_t>(bits >> 32U);
    std::int32_t high{};
    std::memcpy(&high, &high_bits, sizeof(high));
    return SequenceNumber(high, static_cast<std::uint32_t>(bits));
}

inline Gid to_gid(const eprosima::fastrtps::rtps::GUID_t& guid) noexcept {
    Gid gid;
    std::copy_n(guid.guidPrefix.value, 12, gid.data.begin());
    std::copy_n(guid.entityId.value, 4, gid.data.begin() + 12);
    return gid;
}

inline std::optional<RequestId> to_request_id(
    const eprosima::fastrtps::rtps::SampleIdentity& identity) noexcept {
    if (identity.sequence_number() == eprosima::fastrtps::rtps::c_SequenceNumber_Unknown)
        return std::nullopt;
    RequestId request_id;
    request_id.client_gid = to_gid(identity.writer_guid());
    request_id.sequence_number = from_sequence(identity.sequence_number());
    return request_id;
}

inline std::uint64_t to_writer_sequence(const SequenceNumber& sequence) noexcept {
    if (sequence == eprosima::fastrtps::rtps::c_SequenceNumber_Unknown) {
        return 0;
    }
    return static_cast<std::uint64_t>(from_sequence(sequence));
}

}  // namespace impl
}  // namespace dmw

#endif  // DMW_IMPL__FASTDDS__IDENTITY_HPP_
