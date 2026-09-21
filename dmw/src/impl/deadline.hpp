#ifndef DMW_IMPL__DEADLINE_HPP_
#define DMW_IMPL__DEADLINE_HPP_

#include <chrono>

#include "dmw/wait_timeout.hpp"

namespace dmw::impl {

/// Add a non-negative duration to a steady-clock time point without overflow.
inline std::chrono::steady_clock::time_point saturating_steady_add(
    std::chrono::steady_clock::time_point base, std::chrono::nanoseconds duration) noexcept {
    if (duration <= std::chrono::nanoseconds::zero()) return base;

    using WideDuration = std::chrono::duration<long double>;
    const WideDuration requested(duration);
    const WideDuration available(std::chrono::steady_clock::time_point::max() - base);
    if (requested >= available) return std::chrono::steady_clock::time_point::max();

    return base + std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration);
}

/// Convert the public WaitTimeout model to one overflow-safe absolute deadline.
inline std::chrono::steady_clock::time_point steady_deadline(WaitTimeout timeout) noexcept {
    if (timeout.kind() == WaitTimeout::Kind::Infinite) {
        return std::chrono::steady_clock::time_point::max();
    }
    const auto now = std::chrono::steady_clock::now();
    if (timeout.kind() == WaitTimeout::Kind::Poll) return now;
    return saturating_steady_add(now, timeout.duration());
}

}  // namespace dmw::impl

#endif  // DMW_IMPL__DEADLINE_HPP_
