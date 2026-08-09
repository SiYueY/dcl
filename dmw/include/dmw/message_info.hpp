// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__MESSAGE_INFO_HPP_
#define DMW__MESSAGE_INFO_HPP_

#include <cstdint>
#include <optional>

#include "dmw/gid.hpp"

namespace dmw {

/// Metadata associated with a taken topic sample.
struct MessageInfo {
    std::int64_t source_timestamp_ns{0};
    std::int64_t received_timestamp_ns{0};
    Gid publisher_gid{};
    std::optional<std::uint64_t> publication_sequence_number;
    std::optional<std::uint64_t> reception_sequence_number;
};

}  // namespace dmw

#endif  // DMW__MESSAGE_INFO_HPP_
