// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__WAIT_TIMEOUT_HPP_
#define DMW__WAIT_TIMEOUT_HPP_

#include <chrono>
#include <exception>

#include "dmw/error.hpp"
#include "dmw/result.hpp"

namespace dmw {

/// Explicit poll, finite, or infinite WaitSet timeout.
class WaitTimeout {
public:
    enum class Kind { Poll, Finite, Infinite };

    static WaitTimeout poll() noexcept {
        return WaitTimeout(Kind::Poll, std::chrono::nanoseconds::zero());
    }

    static Result<WaitTimeout> finite(std::chrono::nanoseconds timeout) {
        if (timeout <= std::chrono::nanoseconds::zero()) {
            return Result<WaitTimeout>::failure(
                Error(ErrorCode::InvalidArgument, "Finite wait timeout must be positive"));
        }
        return Result<WaitTimeout>::success(WaitTimeout(Kind::Finite, timeout));
    }

    static WaitTimeout infinite() noexcept {
        return WaitTimeout(Kind::Infinite, std::chrono::nanoseconds::zero());
    }

    Kind kind() const noexcept { return kind_; }

    std::chrono::nanoseconds duration() const noexcept {
        if (kind_ != Kind::Finite) {
            std::terminate();
        }
        return duration_;
    }

private:
    WaitTimeout(Kind kind, std::chrono::nanoseconds duration) noexcept
    : kind_(kind), duration_(duration) {}

    Kind kind_;
    std::chrono::nanoseconds duration_;
};

}  // namespace dmw

#endif  // DMW__WAIT_TIMEOUT_HPP_
