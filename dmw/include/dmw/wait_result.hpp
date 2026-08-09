// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__WAIT_RESULT_HPP_
#define DMW__WAIT_RESULT_HPP_

#include <exception>
#include <utility>
#include <vector>

#include "dmw/wait_token.hpp"

namespace dmw {

class WaitSet;

enum class WaitStatus { Ready, Timeout };

/// Immutable readiness snapshot returned by WaitSet::wait().
class WaitResult {
public:
    WaitStatus status() const noexcept { return status_; }

    const std::vector<WaitToken>& ready() const noexcept { return ready_; }

private:
    friend class WaitSet;

    static WaitResult timeout() { return WaitResult(WaitStatus::Timeout, {}); }

    static WaitResult ready(std::vector<WaitToken> tokens) {
        if (tokens.empty()) {
            std::terminate();
        }
        return WaitResult(WaitStatus::Ready, std::move(tokens));
    }

    WaitResult(WaitStatus status, std::vector<WaitToken> ready)
    : status_(status), ready_(std::move(ready)) {}

    WaitStatus status_;
    std::vector<WaitToken> ready_;
};

}  // namespace dmw

#endif  // DMW__WAIT_RESULT_HPP_
