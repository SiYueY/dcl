// SPDX-License-Identifier: Apache-2.0

#include "dmw/guard_condition.hpp"

#include <memory>
#include <utility>

#include "dmw/error.hpp"
#include "impl/guard_condition_impl.hpp"

namespace dmw {

GuardCondition::GuardCondition(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

GuardCondition::~GuardCondition() noexcept = default;

Result<void> GuardCondition::trigger() {
    const auto operation = impl_->state_->context_state->try_acquire_operation();
    if (!operation) {
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    impl_->state_->pending.store(true, std::memory_order_release);
    impl_->state_->notify_wait_set();
    return Result<void>::success();
}

}  // namespace dmw
