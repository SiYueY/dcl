#include "impl/wait_set_impl.hpp"

namespace dmw {

Result<std::unique_ptr<WaitSet>> Context::Impl::create_wait_set(const WaitSetOptions&) {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<WaitSet>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    const auto wait_set_id = impl::next_wait_set_id.fetch_add(1, std::memory_order_relaxed);
    if (wait_set_id == 0) {
        return Result<std::unique_ptr<WaitSet>>::failure(
            Error(ErrorCode::ResourceExhausted, "WaitSet IDs are exhausted"));
    }
    const auto context = std::make_shared<impl::WaitSetState>(context_, wait_set_id);
    auto initialized = context->initialize_shutdown_callback();
    if (!initialized)
        return Result<std::unique_ptr<WaitSet>>::failure(std::move(initialized.error()));
    return Result<std::unique_ptr<WaitSet>>::success(
        std::unique_ptr<WaitSet>(new WaitSet(std::make_unique<WaitSet::Impl>(context))));
}

Result<WaitableRegistration> WaitSet::Impl::add(
    const std::shared_ptr<GuardConditionState>& guard_condition, WaitableKind kind) {
    auto registration = impl::add_guard(context_, guard_condition, kind);
    if (!registration)
        return Result<WaitableRegistration>::failure(std::move(registration.error()));
    return Result<WaitableRegistration>::success(
        WaitableRegistration(context_->wait_set_id_, registration.value(), kind));
}

Result<WaitableRegistration> WaitSet::Impl::add(
    const std::shared_ptr<impl::ReaderWaitState>& reader, WaitableKind kind) {
    auto registration = impl::add_reader(context_, reader, kind);
    if (!registration)
        return Result<WaitableRegistration>::failure(std::move(registration.error()));
    return Result<WaitableRegistration>::success(
        WaitableRegistration(context_->wait_set_id_, registration.value(), kind));
}

Result<WaitableRegistration> WaitSet::Impl::add(
    const std::shared_ptr<impl::TimerState>& timer, WaitableKind kind) {
    if (kind != WaitableKind::Timer) {
        return Result<WaitableRegistration>::failure(
            Error(ErrorCode::InvalidArgument, "Timer registration requires the Timer waitable kind"));
    }
    auto registration = impl::add_timer(context_, timer);
    if (!registration)
        return Result<WaitableRegistration>::failure(std::move(registration.error()));
    return Result<WaitableRegistration>::success(
        WaitableRegistration(context_->wait_set_id_, registration.value(), kind));
}

Result<WaitableRegistration> WaitSet::Impl::add(
    const std::shared_ptr<impl::GraphEventState>& graph_event, WaitableKind kind) {
    if (kind != WaitableKind::GraphEvent) {
        return Result<WaitableRegistration>::failure(Error(
            ErrorCode::InvalidArgument, "GraphEvent registration requires the GraphEvent kind"));
    }
    auto registration = impl::add_graph_event(context_, graph_event);
    if (!registration)
        return Result<WaitableRegistration>::failure(std::move(registration.error()));
    return Result<WaitableRegistration>::success(
        WaitableRegistration(context_->wait_set_id_, registration.value(), kind));
}

Result<WaitableRegistration> WaitSet::Impl::add_composite(
    const std::vector<std::shared_ptr<impl::ReaderWaitState>>& readers, WaitableKind kind,
    std::function<std::uint32_t()> logical_detail,
    std::function<std::optional<std::chrono::steady_clock::time_point>()> runtime_deadline) {
    auto registration = impl::add_readers(
        context_, readers, kind, std::move(logical_detail), std::move(runtime_deadline));
    if (!registration)
        return Result<WaitableRegistration>::failure(std::move(registration.error()));
    return Result<WaitableRegistration>::success(
        WaitableRegistration(context_->wait_set_id_, registration.value(), kind));
}

Result<void> WaitSet::Impl::remove(WaitableRegistration registration) {
    const auto context = context_;
    if (!registration.valid() || registration.wait_set_id_ != context->wait_set_id_) {
        return Result<void>::failure(
            Error(ErrorCode::InvalidArgument, "Registration belongs to another WaitSet"));
    }
    const auto operation = context->context_->try_acquire_operation();
    if (!operation) {
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    if (!context->remove(registration.registration_id_, registration.kind_)) {
        return Result<void>::failure(
            Error(ErrorCode::NotRegistered, "WaitSet registration is stale"));
    }
    return Result<void>::success();
}

Result<WaitResult> WaitSet::Impl::wait(WaitTimeout timeout) {
    const auto context = context_;
    const auto operation = context->context_->try_acquire_operation();
    if (!operation) {
        return Result<WaitResult>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    auto begin = context->begin_wait();
    if (!begin) return Result<WaitResult>::failure(std::move(begin.error()));
    const impl::WaitActivityGuard active_wait(context);
    const auto deadline = timeout.kind() == WaitTimeout::Kind::Finite
                              ? std::chrono::steady_clock::now() + timeout.duration()
                              : std::chrono::steady_clock::time_point::max();

    while (true) {
        auto repaired = context->repair_control_guard_if_needed();
        if (!repaired) return Result<WaitResult>::failure(std::move(repaired.error()));
        if (context->context_->is_shutdown()) {
            return Result<WaitResult>::failure(
                Error(ErrorCode::ContextShutdown, "Context is shut down"));
        }
        if (context->is_closing()) {
            return Result<WaitResult>::failure(
                Error(ErrorCode::ParentDestroyed, "WaitSet is closing"));
        }

        const auto observed_topology = context->topology_generation();
        // A notification that arrives after this observation must not be
        // cleared before native wait starts.  This closes the final lost-wake
        // window for logical GuardConditions, which have no native condition.
        const auto observed_wake_generation =
            context->wake_->generation.load(std::memory_order_acquire);
        const auto snapshot = context->snapshot();
        std::vector<ReadyWaitable> registrations;
        for (const auto& registration : snapshot) {
            if (registration->is_closing()) {
                context->detach(registration);
                continue;
            }
            const auto detail = registration->ready_mask();
            if (detail == 0) continue;
            registrations.push_back(ReadyWaitable{
                WaitableRegistration(context->wait_set_id_, registration->id, registration->kind),
                registration->kind, detail});
        }
        // Do not expose a readiness set assembled across a topology change
        // (notably Server available↔full reader detach/reattach).
        if (context->topology_generation() != observed_topology) continue;
        if (!registrations.empty()) {
            return Result<WaitResult>::success(WaitResult::ready(std::move(registrations)));
        }
        if (timeout.kind() == WaitTimeout::Kind::Poll) {
            return Result<WaitResult>::success(WaitResult::timeout());
        }
        if (timeout.kind() == WaitTimeout::Kind::Finite &&
            std::chrono::steady_clock::now() >= deadline) {
            return Result<WaitResult>::success(WaitResult::timeout());
        }

        // Fold the earliest Clock-bound Timer deadline into the native wait so
        // a timer wakes an otherwise infinite wait without a polling slice.
        auto native_deadline = deadline;
        for (const auto& registration : snapshot) {
            if (registration->timer) {
                const auto timer_deadline = registration->timer->steady_deadline();
                if (timer_deadline && *timer_deadline < native_deadline) {
                    native_deadline = *timer_deadline;
                }
            }
            if (registration->runtime_deadline) {
                const auto runtime = registration->runtime_deadline();
                if (runtime && *runtime < native_deadline) native_deadline = *runtime;
            }
        }

        if (context->topology_generation() != observed_topology) continue;
        Result<void> wake = Result<void>::success();
        if (native_deadline != std::chrono::steady_clock::time_point::max()) {
            const auto remaining = native_deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                // A ROS/System clock jump moved the runtime deadline after it
                // was computed.  Re-evaluate instead of blocking on a stale
                // deadline; the next iteration recomputes it from the clock.
                continue;
            }
            wake = context->wait_for_notification(
                std::chrono::duration_cast<std::chrono::nanoseconds>(remaining),
                observed_wake_generation);
        } else {
            wake = context->wait_for_notification(observed_wake_generation);
        }
        if (!wake) {
            return Result<WaitResult>::failure(wake.error());
        }
    }
}

}  // namespace dmw
