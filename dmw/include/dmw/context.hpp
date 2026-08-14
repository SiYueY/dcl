#ifndef DMW_CONTEXT_HPP_
#define DMW_CONTEXT_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "dmw/compatibility.hpp"
#include "dmw/guard_condition.hpp"
#include "dmw/node.hpp"
#include "dmw/result.hpp"
#include "dmw/visibility_control.hpp"
#include "dmw/wait_set.hpp"

namespace dmw {

struct ContextOptions {
    std::uint32_t domain_id{0};
    std::string participant_name;
    CompatibilityProfile compatibility_profile{CompatibilityProfile::NativeDds};
};

/// Root of one DMW runtime and exactly one DDS domain participant.
class DMW_PUBLIC Context {
public:
    /// Transactionally create an active Context with an immutable compatibility profile.
    static Result<std::unique_ptr<Context>> create(const ContextOptions& options);

    ~Context() noexcept;

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    std::uint32_t domain_id() const noexcept;
    bool is_shutdown() const noexcept;

    /// Irreversibly transition the runtime from Active to Shutdown.
    Result<void> shutdown();

    /// Create a child only while the Context is active.
    Result<std::unique_ptr<Node>> create_node(const NodeOptions& options);

    Result<std::unique_ptr<WaitSet>> create_wait_set(const WaitSetOptions& options = {});

    Result<std::unique_ptr<GuardCondition>> create_guard_condition(
        const GuardConditionOptions& options = {});

private:
    class Impl;

    explicit Context(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace dmw

#endif  // DMW_CONTEXT_HPP_
