#include "impl/wait_set_impl.hpp"

#include "impl/wait_set_impl.hpp"

namespace dmw {

Result<std::unique_ptr<WaitSet>> Context::Impl::create_wait_set(const WaitSetOptions&) {
    const auto operation = state_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<WaitSet>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    const auto wait_set_id = impl::next_wait_set_id.fetch_add(1, std::memory_order_relaxed);
    if (wait_set_id == 0) {
        return Result<std::unique_ptr<WaitSet>>::failure(
            Error(ErrorCode::ResourceExhausted, "WaitSet IDs are exhausted"));
    }
    const auto state = std::make_shared<impl::WaitSetState>(state_, wait_set_id);
    auto initialized = state->initialize_shutdown_callback();
    if (!initialized)
        return Result<std::unique_ptr<WaitSet>>::failure(std::move(initialized.error()));
    return Result<std::unique_ptr<WaitSet>>::success(
        std::unique_ptr<WaitSet>(new WaitSet(std::make_unique<WaitSet::Impl>(state))));
}

Result<WaitToken> WaitSet::Impl::add(
    const std::shared_ptr<GuardConditionState>& guard_condition, WaitableKind kind) {
    auto registration = impl::add_guard(state_, guard_condition, kind);
    if (!registration) return Result<WaitToken>::failure(std::move(registration.error()));
    return Result<WaitToken>::success(WaitToken(state_->wait_set_id_, registration.value(), kind));
}

Result<WaitToken> WaitSet::Impl::add(
    const std::shared_ptr<impl::ReaderWaitState>& reader, WaitableKind kind) {
    auto registration = impl::add_reader(state_, reader, kind);
    if (!registration) return Result<WaitToken>::failure(std::move(registration.error()));
    return Result<WaitToken>::success(WaitToken(state_->wait_set_id_, registration.value(), kind));
}

Result<void> WaitSet::Impl::remove(WaitToken token) {
    const auto state = state_;
    if (!token.valid() || token.wait_set_id_ != state->wait_set_id_) {
        return Result<void>::failure(
            Error(ErrorCode::InvalidArgument, "Token belongs to another WaitSet"));
    }
    const auto operation = state->context_state_->try_acquire_operation();
    if (!operation) {
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    if (!state->remove(token.registration_id_, token.kind_)) {
        return Result<void>::failure(Error(ErrorCode::NotRegistered, "WaitSet token is stale"));
    }
    return Result<void>::success();
}

Result<WaitResult> WaitSet::Impl::wait(WaitTimeout timeout) {
    const auto state = state_;
    const auto operation = state->context_state_->try_acquire_operation();
    if (!operation) {
        return Result<WaitResult>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    auto begin = state->begin_wait();
    if (!begin) return Result<WaitResult>::failure(std::move(begin.error()));
    const impl::WaitActivityGuard active_wait(state);
    const auto deadline = timeout.kind() == WaitTimeout::Kind::Finite
                              ? std::chrono::steady_clock::now() + timeout.duration()
                              : std::chrono::steady_clock::time_point::max();

    while (true) {
        auto repaired = state->repair_control_guard_if_needed();
        if (!repaired) return Result<WaitResult>::failure(std::move(repaired.error()));
        if (state->context_state_->is_shutdown()) {
            return Result<WaitResult>::failure(
                Error(ErrorCode::ContextShutdown, "Context is shut down"));
        }
        if (state->is_closing()) {
            return Result<WaitResult>::failure(
                Error(ErrorCode::ParentDestroyed, "WaitSet is closing"));
        }

        const auto observed_topology = state->topology_generation();
        std::vector<WaitToken> tokens;
        for (const auto& registration : state->snapshot()) {
            if (registration->is_closing()) {
                state->detach(registration);
                continue;
            }
            if (registration->ready()) {
                const WaitToken token(state->wait_set_id_, registration->id, registration->kind);
                tokens.push_back(token);
            }
        }
        // Do not expose a readiness set assembled across a topology change
        // (notably Server available↔full reader detach/reattach).
        if (state->topology_generation() != observed_topology) continue;
        if (!tokens.empty()) {
            return Result<WaitResult>::success(WaitResult::ready(std::move(tokens)));
        }
        if (timeout.kind() == WaitTimeout::Kind::Poll) {
            return Result<WaitResult>::success(WaitResult::timeout());
        }
        if (timeout.kind() == WaitTimeout::Kind::Finite &&
            std::chrono::steady_clock::now() >= deadline) {
            return Result<WaitResult>::success(WaitResult::timeout());
        }

        const auto slice = std::chrono::milliseconds(100);
        const auto wake_deadline =
            timeout.kind() == WaitTimeout::Kind::Finite
                ? std::min(deadline, std::chrono::steady_clock::now() + slice)
                : std::chrono::steady_clock::now() + slice;
        const auto remaining = wake_deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
            continue;
        }
        if (state->topology_generation() != observed_topology) continue;
        const auto wake = state->wait_for_notification(
            std::chrono::duration_cast<std::chrono::nanoseconds>(remaining));
        if (!wake) {
            return Result<WaitResult>::failure(std::move(wake.error()));
        }
    }
}

}  // namespace dmw
