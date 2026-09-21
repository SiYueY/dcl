#ifndef DMW_IMPL__CONTEXT_IMPL_HPP_
#define DMW_IMPL__CONTEXT_IMPL_HPP_

#include <memory>

#include "dmw/context.hpp"
#include "dmw/graph.hpp"
#include "dmw/graph_event.hpp"
#include "impl/context.hpp"

namespace dmw {

class Context::Impl {
public:
    Impl(std::shared_ptr<impl::Context> context, Arguments arguments) noexcept
    : context_(std::move(context)), arguments_(std::move(arguments)) {}

    static Result<std::unique_ptr<Context>> create(const ContextOptions& options);
    std::uint32_t domain_id() const noexcept;
    RuntimeMode runtime_mode() const noexcept;
    bool is_shutdown() const noexcept;
    Result<void> shutdown();
    Result<std::unique_ptr<Node>> create_node(const NodeOptions& options);
    Result<std::unique_ptr<Clock>> create_clock(ClockType type);
    Result<std::unique_ptr<Timer>> create_timer(Clock& clock, const TimerOptions& options);
    Result<std::unique_ptr<GuardCondition>> create_guard_condition(
        const GuardConditionOptions& options);
    Result<std::unique_ptr<GraphEvent>> create_graph_event();
    Result<GraphRevision> graph_revision() const;
    Result<GraphSnapshot> graph_snapshot() const;
    Result<std::unique_ptr<WaitSet>> create_wait_set(const WaitSetOptions& options);

private:
    std::shared_ptr<impl::Context> context_;
    Arguments arguments_;
};

}  // namespace dmw

#endif  // DMW_IMPL__CONTEXT_IMPL_HPP_
