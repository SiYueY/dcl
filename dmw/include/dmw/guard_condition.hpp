// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__GUARD_CONDITION_HPP_
#define DMW__GUARD_CONDITION_HPP_

#include <memory>
#include <utility>

#include "dmw/result.hpp"
#include "dmw/visibility_control.hpp"

namespace dmw {

class Context;
class WaitSet;

/// Reserved extension point for GuardCondition creation.
struct GuardConditionOptions {};

/// Application-triggered WaitSet waitable with pending-trigger semantics.
class DMW_PUBLIC GuardCondition {
public:
    ~GuardCondition() noexcept;

    GuardCondition(const GuardCondition&) = delete;
    GuardCondition& operator=(const GuardCondition&) = delete;
    GuardCondition(GuardCondition&&) = delete;
    GuardCondition& operator=(GuardCondition&&) = delete;

    Result<void> trigger();

private:
    friend class Context;
    friend class WaitSet;

    class Impl;

    explicit GuardCondition(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace dmw

#endif  // DMW__GUARD_CONDITION_HPP_
