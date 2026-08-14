#include "dmw/guard_condition.hpp"

#include <memory>
#include <limits>
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
    auto generation = impl_->state_->trigger_generation.load(std::memory_order_acquire);
    while (true) {
        if (generation == std::numeric_limits<std::uint64_t>::max()) {
            return Result<void>::failure(
                Error(ErrorCode::ResourceExhausted, "GuardCondition trigger generation is exhausted"));
        }
        if (impl_->state_->trigger_generation.compare_exchange_weak(
                generation, generation + 1, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            break;
        }
    }
    impl_->state_->pending.store(true, std::memory_order_release);
    impl_->state_->notify_wait_set();
    return Result<void>::success();
}

}  // namespace dmw
