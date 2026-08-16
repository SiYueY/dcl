#include "impl/guard_condition_impl.hpp"

#include <memory>
#include <limits>
#include <utility>

#include "dmw/error.hpp"

namespace dmw {

Result<void> GuardCondition::Impl::trigger() { return state_->trigger(); }

Result<void> GuardConditionState::trigger() {
    const auto operation = context_state->try_acquire_operation();
    if (!operation) {
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    auto generation = trigger_generation.load(std::memory_order_acquire);
    while (true) {
        if (generation == std::numeric_limits<std::uint64_t>::max()) {
            return Result<void>::failure(Error(
                ErrorCode::ResourceExhausted, "GuardCondition trigger generation is exhausted"));
        }
        if (trigger_generation.compare_exchange_weak(
                generation, generation + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }
    pending.store(true, std::memory_order_release);
    notify_wait_set();
    return Result<void>::success();
}

}  // namespace dmw
