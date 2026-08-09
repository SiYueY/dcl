// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__REQUEST_ID_HPP_
#define DMW__REQUEST_ID_HPP_

#include <cstddef>
#include <cstdint>
#include <functional>

#include "dmw/gid.hpp"

namespace dmw {

/// Correlates a service request with its response.
struct RequestId {
    Gid client_gid{};
    std::int64_t sequence_number{0};
};

inline bool operator==(const RequestId& lhs, const RequestId& rhs) noexcept {
    return lhs.client_gid == rhs.client_gid && lhs.sequence_number == rhs.sequence_number;
}

inline bool operator!=(const RequestId& lhs, const RequestId& rhs) noexcept {
    return !(lhs == rhs);
}

struct RequestIdHash {
    std::size_t operator()(const RequestId& request_id) const noexcept {
        const auto gid_hash = GidHash{}(request_id.client_gid);
        const auto sequence_hash = std::hash<std::int64_t>{}(request_id.sequence_number);
        return gid_hash ^ (sequence_hash + static_cast<std::size_t>(0x9e3779b9U) +
                           (gid_hash << 6U) + (gid_hash >> 2U));
    }
};

}  // namespace dmw

#endif  // DMW__REQUEST_ID_HPP_
