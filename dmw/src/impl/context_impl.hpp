#ifndef DMW_IMPL__CONTEXT_IMPL_HPP_
#define DMW_IMPL__CONTEXT_IMPL_HPP_

#include <memory>

#include "dmw/context.hpp"
#include "impl/fastdds/context.hpp"

namespace dmw {

class Context::Impl {
public:
    explicit Impl(std::shared_ptr<impl::fastdds::Context> state) noexcept
    : state_(std::move(state)) {}

    static Result<std::unique_ptr<Context>> create(const ContextOptions& options);
    std::uint32_t domain_id() const noexcept;
    bool is_shutdown() const noexcept;
    Result<void> shutdown();
    Result<std::unique_ptr<Node>> create_node(const NodeOptions& options);
    Result<std::unique_ptr<GuardCondition>> create_guard_condition(
        const GuardConditionOptions& options);
    Result<std::unique_ptr<WaitSet>> create_wait_set(const WaitSetOptions& options);

private:
    std::shared_ptr<impl::fastdds::Context> state_;
};

}  // namespace dmw

#endif  // DMW_IMPL__CONTEXT_IMPL_HPP_
