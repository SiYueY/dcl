#ifndef DMW_MESSAGE_INFO_HPP_
#define DMW_MESSAGE_INFO_HPP_

#include <cstdint>
#include "dmw/gid.hpp"

namespace dmw {

/// Metadata associated with a taken topic sample.
struct MessageInfo {
    Gid writer_gid{};
    std::int64_t writer_timestamp{0};
    std::uint64_t writer_sequence{0};  // 0 = unknown/unavailable
    std::int64_t reader_timestamp{0};
};

}  // namespace dmw

#endif  // DMW_MESSAGE_INFO_HPP_
