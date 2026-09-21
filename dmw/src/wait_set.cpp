#include "dmw/wait_set.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <utility>

#include "dmw/wait_result.hpp"
#include "impl/context_impl.hpp"
#include "impl/action_impl.hpp"
#include "impl/graph_impl.hpp"
#include "impl/timer_impl.hpp"
#include "impl/wait_set_impl.hpp"

namespace dmw {

WaitSet::WaitSet(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

WaitSet::~WaitSet() noexcept = default;

Result<std::unique_ptr<WaitSet>> Context::create_wait_set(const WaitSetOptions& options) {
    return impl_->create_wait_set(options);
}

Result<WaitableRegistration> WaitSet::add(GuardCondition& value) {
    return impl_->add(value.impl_->wait_state(), WaitableKind::GuardCondition);
}

Result<WaitableRegistration> WaitSet::add(Subscriber& value) {
    return impl_->add(value.impl_->wait_state(), WaitableKind::Subscriber);
}

Result<WaitableRegistration> WaitSet::add(Client& value) {
    return impl_->add(value.impl_->wait_state(), WaitableKind::Client);
}

Result<WaitableRegistration> WaitSet::add(Server& value) {
    return impl_->add(value.impl_->wait_state(), WaitableKind::Server);
}

Result<WaitableRegistration> WaitSet::add(Event& value) {
    return impl_->add(value.impl_->wait_state(), WaitableKind::Event);
}

Result<WaitableRegistration> WaitSet::add(Timer& value) {
    return impl_->add(value.impl_->wait_state(), WaitableKind::Timer);
}

Result<WaitableRegistration> WaitSet::add(GraphEvent& value) {
    return impl_->add(value.impl_->wait_state(), WaitableKind::GraphEvent);
}

Result<WaitableRegistration> WaitSet::add(ActionClient& value) {
    return impl_->add_composite(value.impl_->wait_states(), WaitableKind::ActionClient);
}

Result<WaitableRegistration> WaitSet::add(ActionServer& value) {
    // Result expiry is logical, not reader-driven: the WaitSet folds the
    // earliest expiry deadline into its native wait and reports the
    // goal_expired sub-channel from the same snapshot.
    const std::weak_ptr<impl::ActionGoalRegistry> weak_goals = value.impl_->goals();
    auto goal_expired = [weak_goals]() -> std::uint32_t {
        const auto goals = weak_goals.lock();
        if (!goals) return 0;
        const auto expiry = goals->earliest_expiry();
        if (!expiry || !expiry.value()) return 0;
        return std::chrono::steady_clock::now() >= *expiry.value() ? kActionGoalExpiredBit : 0;
    };
    auto earliest_expiry = [weak_goals]()
        -> std::optional<std::chrono::steady_clock::time_point> {
        const auto goals = weak_goals.lock();
        if (!goals) return std::nullopt;
        const auto expiry = goals->earliest_expiry();
        if (!expiry || !expiry.value()) return std::nullopt;
        return *expiry.value();
    };
    return impl_->add_composite(
        value.impl_->wait_states(), WaitableKind::ActionServer, std::move(goal_expired),
        std::move(earliest_expiry));
}

Result<void> WaitSet::remove(WaitableRegistration registration) {
    return impl_->remove(registration);
}

Result<WaitResult> WaitSet::wait(WaitTimeout timeout) { return impl_->wait(timeout); }

}  // namespace dmw
