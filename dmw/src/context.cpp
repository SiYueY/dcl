#include "dmw/context.hpp"

#include <utility>

#include "impl/context_impl.hpp"
#include "impl/guard_condition_impl.hpp"
#include "impl/node_impl.hpp"
#include "impl/wait_set_impl.hpp"

namespace dmw {

Result<std::unique_ptr<Context>> Context::create(const ContextOptions& options) {
    return Impl::create(options);
}

Context::Context(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Context::~Context() noexcept {
    if (impl_) (void)impl_->shutdown();
}
std::uint32_t Context::domain_id() const noexcept { return impl_->domain_id(); }
bool Context::is_shutdown() const noexcept { return impl_->is_shutdown(); }
Result<void> Context::shutdown() { return impl_->shutdown(); }

Result<std::unique_ptr<Node>> Context::create_node(const NodeOptions& options) {
    return impl_->create_node(options);
}

Result<std::unique_ptr<GuardCondition>> Context::create_guard_condition(
    const GuardConditionOptions& options) {
    return impl_->create_guard_condition(options);
}

}  // namespace dmw
