#ifndef DMW_WAIT_RESULT_HPP_
#define DMW_WAIT_RESULT_HPP_

#include <exception>
#include <utility>
#include <vector>

#include "dmw/waitable_registration.hpp"

namespace dmw {

class WaitSet;

enum class WaitStatus { Ready, Timeout };

/// Immutable readiness snapshot returned by WaitSet::wait().
class WaitResult {
public:
    WaitStatus status() const noexcept { return status_; }

    const std::vector<WaitableRegistration>& ready() const noexcept { return ready_; }

private:
    friend class WaitSet;

    static WaitResult timeout() { return WaitResult(WaitStatus::Timeout, {}); }

    static WaitResult ready(std::vector<WaitableRegistration> registrations) {
        if (registrations.empty()) {
            std::terminate();
        }
        return WaitResult(WaitStatus::Ready, std::move(registrations));
    }

    WaitResult(WaitStatus status, std::vector<WaitableRegistration> ready)
    : status_(status), ready_(std::move(ready)) {}

    WaitStatus status_;
    std::vector<WaitableRegistration> ready_;
};

}  // namespace dmw

#endif  // DMW_WAIT_RESULT_HPP_
