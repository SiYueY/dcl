// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__WAIT_TOKEN_HPP_
#define DMW__WAIT_TOKEN_HPP_

#include <cstdint>

namespace dmw {

class WaitSet;

enum class WaitableKind { Subscriber, Client, Server, Event, GuardCondition };

/// Opaque identity of a registration in one WaitSet.
class WaitToken {
public:
    WaitToken() noexcept = default;

    bool valid() const noexcept { return wait_set_id_ != 0 && registration_id_ != 0; }

    WaitableKind kind() const noexcept { return kind_; }

    friend bool operator==(const WaitToken& lhs, const WaitToken& rhs) noexcept {
        return lhs.wait_set_id_ == rhs.wait_set_id_ &&
               lhs.registration_id_ == rhs.registration_id_ && lhs.kind_ == rhs.kind_;
    }

    friend bool operator!=(const WaitToken& lhs, const WaitToken& rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    friend class WaitSet;

    WaitToken(std::uint64_t wait_set_id, std::uint64_t registration_id, WaitableKind kind) noexcept
    : wait_set_id_(wait_set_id), registration_id_(registration_id), kind_(kind) {}

    std::uint64_t wait_set_id_{0};
    std::uint64_t registration_id_{0};
    WaitableKind kind_{WaitableKind::Subscriber};
};

}  // namespace dmw

#endif  // DMW__WAIT_TOKEN_HPP_
