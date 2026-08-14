#include "dmw/guard_condition.hpp"

#include <utility>

#include "impl/guard_condition_impl.hpp"

namespace dmw {
GuardCondition::GuardCondition(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GuardCondition::~GuardCondition() noexcept = default;
Result<void> GuardCondition::trigger() { return impl_->trigger(); }
}  // namespace dmw
