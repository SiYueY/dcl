// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__WAIT_SET_HPP_
#define DMW__WAIT_SET_HPP_

#include <memory>
#include <utility>

#include "dmw/result.hpp"
#include "dmw/visibility_control.hpp"
#include "dmw/wait_result.hpp"
#include "dmw/wait_timeout.hpp"
#include "dmw/wait_token.hpp"

namespace dmw {

class Client;
class Context;
class Event;
class GuardCondition;
class Server;
class Subscriber;

/// Reserved extension point for WaitSet creation.
struct WaitSetOptions {};

/// Non-owning set of registered waitables.
class DMW_PUBLIC WaitSet {
public:
    /// The caller must ensure no wait() call is active during destruction.
    ~WaitSet() noexcept;

    WaitSet(const WaitSet&) = delete;
    WaitSet& operator=(const WaitSet&) = delete;
    WaitSet(WaitSet&&) = delete;
    WaitSet& operator=(WaitSet&&) = delete;

    /// Register a non-owned waitable; it may belong to at most one WaitSet.
    Result<WaitToken> add(Subscriber& subscriber);
    Result<WaitToken> add(Client& client);
    Result<WaitToken> add(Server& server);
    Result<WaitToken> add(Event& event);
    Result<WaitToken> add(GuardCondition& guard_condition);

    /// Remove a token created by this WaitSet.
    Result<void> remove(WaitToken token);

    /// Wait for a non-empty readiness snapshot or a non-error timeout result.
    Result<WaitResult> wait(WaitTimeout timeout);

private:
    friend class Context;

    class Impl;

    explicit WaitSet(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace dmw

#endif  // DMW__WAIT_SET_HPP_
