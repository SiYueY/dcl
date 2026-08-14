#ifndef DMW_GID_HPP_
#define DMW_GID_HPP_

#include <array>
#include <cstddef>
#include <cstdint>

namespace dmw {

/// Middleware-neutral endpoint identifier.
struct Gid {
    static constexpr std::size_t kSize = 16;

    std::array<std::uint8_t, kSize> data{};
};

inline bool operator==(const Gid& lhs, const Gid& rhs) noexcept { return lhs.data == rhs.data; }

inline bool operator!=(const Gid& lhs, const Gid& rhs) noexcept { return !(lhs == rhs); }

struct GidHash {
    std::size_t operator()(const Gid& gid) const noexcept {
        std::size_t value = static_cast<std::size_t>(1469598103934665603ULL);
        for (const auto byte : gid.data) {
            value ^= static_cast<std::size_t>(byte);
            value *= static_cast<std::size_t>(1099511628211ULL);
        }
        return value;
    }
};

}  // namespace dmw

#endif  // DMW_GID_HPP_
