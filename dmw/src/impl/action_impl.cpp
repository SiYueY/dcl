#include "impl/action_impl.hpp"

#include <chrono>
#include <thread>
#include <utility>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/rtps/common/InstanceHandle.h>

#include "dmw/error.hpp"
#include "impl/client_impl.hpp"
#include "impl/deadline.hpp"
#include "impl/identity.hpp"
#include "impl/reader_wait_state.hpp"
#include "impl/server_impl.hpp"
#include "impl/subscriber_impl.hpp"

namespace dmw {

namespace {

/// Participant identity used to keep Action availability composition inside
/// one participant and to exclude the local participant from "remote server".
std::string local_participant_key(const impl::Context& context) {
    const auto guid = eprosima::fastrtps::rtps::iHandle2GUID(
        context.participant()->get_instance_handle());
    return std::string(
        reinterpret_cast<const char*>(guid.guidPrefix.value), sizeof(guid.guidPrefix.value));
}

}  // namespace

ActionClient::Impl::Impl(
    std::shared_ptr<impl::Context> context, std::string action_name,
    impl::ActionEndpointNames names, impl::ActionEndpointTypes types,
    std::unique_ptr<Client> goal_client, std::unique_ptr<Client> cancel_client,
    std::unique_ptr<Client> result_client, std::unique_ptr<Subscriber> feedback_subscriber,
    std::unique_ptr<Subscriber> status_subscriber)
: context_(std::move(context)),
  action_name_(std::move(action_name)),
  names_(std::move(names)),
  types_(std::move(types)),
  local_participant_(local_participant_key(*context_)),
  goal_client_(std::move(goal_client)),
  cancel_client_(std::move(cancel_client)),
  result_client_(std::move(result_client)),
  feedback_subscriber_(std::move(feedback_subscriber)),
  status_subscriber_(std::move(status_subscriber)),
  availability_state_(std::make_shared<AvailabilityWaitState>()) {
    const std::weak_ptr<AvailabilityWaitState> weak_state = availability_state_;
    availability_subscription_ = context_->discovery_graph()->subscribe([weak_state](std::uint64_t) {
        if (const auto state = weak_state.lock()) {
            state->notify_revision();
        }
    });
    shutdown_callback_id_ = context_->register_shutdown_callback([weak_state] {
        if (const auto state = weak_state.lock()) {
            state->notify_revision();
        }
    });
}

ActionClient::Impl::~Impl() noexcept {
    if (shutdown_callback_id_ != 0) context_->unregister_shutdown_callback(shutdown_callback_id_);
}

Result<RequestId> ActionClient::Impl::write_goal_request(const void* request) {
    return goal_client_->write_request(request);
}

Result<bool> ActionClient::Impl::read_goal_response(void* response, RequestId& request_id) {
    return goal_client_->read_response(response, request_id);
}

Result<RequestId> ActionClient::Impl::write_cancel_request(const void* request) {
    return cancel_client_->write_request(request);
}

Result<bool> ActionClient::Impl::read_cancel_response(void* response, RequestId& request_id) {
    return cancel_client_->read_response(response, request_id);
}

Result<RequestId> ActionClient::Impl::write_result_request(const void* request) {
    return result_client_->write_request(request);
}

Result<bool> ActionClient::Impl::read_result_response(void* response, RequestId& request_id) {
    return result_client_->read_response(response, request_id);
}

Result<bool> ActionClient::Impl::read_feedback(void* feedback, MessageInfo& info) {
    return feedback_subscriber_->read(feedback, info);
}

Result<bool> ActionClient::Impl::read_status(void* status, MessageInfo& info) {
    return status_subscriber_->read(status, info);
}

Result<bool> ActionClient::Impl::check_availability() const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    const auto graph = context_->discovery_graph();
    if (graph->health() != impl::DiscoveryHealth::Healthy) {
        return Result<bool>::failure(
            Error(ErrorCode::DDSError, "Discovery graph is unavailable"));
    }
    const auto candidates =
        impl::collect_endpoint_candidates(context_->runtime_mode(), graph->view());
    return Result<bool>::success(
        impl::has_remote_action_server(candidates, names_, types_, local_participant_));
}

Result<bool> ActionClient::Impl::server_is_available() const { return check_availability(); }

Result<AvailabilityWaitToken> ActionClient::Impl::prepare_availability_wait() const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<AvailabilityWaitToken>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return Result<AvailabilityWaitToken>::success(availability_state_->prepare());
}

Result<void> ActionClient::Impl::interrupt_waits() {
    availability_state_->interrupt();
    return Result<void>::success();
}

Result<bool> ActionClient::Impl::wait_for_server(WaitTimeout timeout) const {
    auto token = prepare_availability_wait();
    if (!token) return Result<bool>::failure(std::move(token.error()));
    return wait_for_server(timeout, token.value());
}

Result<bool> ActionClient::Impl::wait_for_server(
    WaitTimeout timeout, const AvailabilityWaitToken& token) const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    const auto state = availability_state_;
    if (!state->owns(token)) {
        return Result<bool>::failure(
            Error(ErrorCode::InvalidArgument, "Availability token belongs to another endpoint"));
    }
    const auto deadline = impl::steady_deadline(timeout);
    std::unique_lock lock(state->mutex);
    while (true) {
        if (context_->is_shutdown()) {
            return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
        }
        if (state->interrupted(token)) {
            return Result<bool>::failure(Error(ErrorCode::Interrupted, "Availability wait interrupted"));
        }
        const auto observed_revision = state->revision;
        auto available = check_availability();
        if (!available || available.value() || timeout.kind() == WaitTimeout::Kind::Poll) {
            return available;
        }
        const auto changed = [&] {
            return context_->is_shutdown() || state->interrupted(token) ||
                   state->revision != observed_revision;
        };
        if (timeout.kind() == WaitTimeout::Kind::Finite) {
            if (!state->cv.wait_until(lock, deadline, changed)) return Result<bool>::success(false);
        } else {
            state->cv.wait(lock, changed);
        }
    }
}

Result<ActionClientReadySet> ActionClient::Impl::readiness() const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<ActionClientReadySet>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    ActionClientReadySet ready;
    ready.goal_response = goal_client_->impl_->wait_state()->is_ready();
    ready.cancel_response = cancel_client_->impl_->wait_state()->is_ready();
    ready.result_response = result_client_->impl_->wait_state()->is_ready();
    ready.feedback = feedback_subscriber_->impl_->wait_state()->is_ready();
    ready.status = status_subscriber_->impl_->wait_state()->is_ready();
    return Result<ActionClientReadySet>::success(ready);
}

std::vector<std::shared_ptr<impl::ReaderWaitState>> ActionClient::Impl::wait_states() const {
    return {
        goal_client_->impl_->wait_state(),
        cancel_client_->impl_->wait_state(),
        result_client_->impl_->wait_state(),
        feedback_subscriber_->impl_->wait_state(),
        status_subscriber_->impl_->wait_state(),
    };
}

ActionServer::Impl::Impl(
    std::shared_ptr<impl::Context> context, std::string action_name,
    std::unique_ptr<Server> goal_server, std::unique_ptr<Server> cancel_server,
    std::unique_ptr<Server> result_server, std::unique_ptr<Publisher> feedback_publisher,
    std::unique_ptr<Publisher> status_publisher, std::chrono::nanoseconds result_timeout)
: context_(std::move(context)),
  action_name_(std::move(action_name)),
  goal_server_(std::move(goal_server)),
  cancel_server_(std::move(cancel_server)),
  result_server_(std::move(result_server)),
  feedback_publisher_(std::move(feedback_publisher)),
  status_publisher_(std::move(status_publisher)),
  goals_(std::make_shared<impl::ActionGoalRegistry>(result_timeout)) {}

ActionServer::Impl::~Impl() noexcept = default;

Result<bool> ActionServer::Impl::read_goal_request(void* request, RequestId& request_id) {
    return goal_server_->read_request(request, request_id);
}

Result<void> ActionServer::Impl::write_goal_response(
    const RequestId& request_id, const void* rejected_response) {
    return goal_server_->write_response(request_id, rejected_response);
}

Result<bool> ActionServer::Impl::read_cancel_request(void* request, RequestId& request_id) {
    return cancel_server_->read_request(request, request_id);
}

Result<void> ActionServer::Impl::write_cancel_response(
    const RequestId& request_id, const void* response) {
    return cancel_server_->write_response(request_id, response);
}

Result<bool> ActionServer::Impl::read_result_request(void* request, RequestId& request_id) {
    return result_server_->read_request(request, request_id);
}

Result<void> ActionServer::Impl::write_result_response(
    const RequestId& request_id, const void* response) {
    if (response == nullptr)
        return Result<void>::failure(Error(ErrorCode::InvalidArgument, "Response must not be null"));
    auto claimed = [&] {
        std::lock_guard lock(result_coordination_mutex_);
        auto claim = result_server_->impl_->claim_response(request_id);
        if (claim) goals_->remove_result_request(request_id);
        return claim;
    }();
    if (!claimed) return Result<void>::failure(std::move(claimed.error()));
    return result_server_->impl_->write_claimed_response(request_id, claimed.value(), response);
}

Result<void> ActionServer::Impl::discard_goal_request(const RequestId& request_id) {
    return goal_server_->discard_request(request_id);
}

Result<void> ActionServer::Impl::discard_cancel_request(const RequestId& request_id) {
    return cancel_server_->discard_request(request_id);
}

Result<void> ActionServer::Impl::discard_result_request(const RequestId& request_id) {
    std::lock_guard lock(result_coordination_mutex_);
    auto discarded = result_server_->discard_request(request_id);
    if (discarded || discarded.error().code() == ErrorCode::NotFound)
        goals_->remove_result_request(request_id);
    return discarded;
}

Result<void> ActionServer::Impl::publish_feedback(const void* feedback) {
    return feedback_publisher_->write(feedback);
}

Result<void> ActionServer::Impl::publish_status(const void* status) {
    return status_publisher_->write(status);
}

Result<GoalTransition> ActionServer::Impl::accept_goal(
    const RequestId& request_id, const GoalInfo& goal_info, const void* accepted_response,
    GoalAcceptMode mode) {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<GoalTransition>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    // Reserve local state first: the only allocation happens here, before the
    // wire write, so a successful response can be committed without failure.
    if (!goals_->reserve(goal_info.goal_id)) {
        return Result<GoalTransition>::failure(
            Error(ErrorCode::AlreadyExists, "Goal is already accepted"));
    }
    struct ReservationRollback {
        impl::ActionGoalRegistry& registry;
        const GoalId& id;
        bool committed{false};
        ~ReservationRollback() noexcept {
            if (!committed) registry.rollback_reservation(id);
        }
    } rollback{*goals_, goal_info.goal_id};
    auto written = goal_server_->write_response(request_id, accepted_response);
    if (!written) {
        return Result<GoalTransition>::failure(std::move(written.error()));
    }
    if (!goals_->commit(goal_info, mode)) {
        return Result<GoalTransition>::failure(
            Error(ErrorCode::ProtocolFault, "Goal reservation was lost after response commit"));
    }
    rollback.committed = true;
    GoalTransition transition;
    transition.previous = GoalState::Unknown;
    transition.current =
        mode == GoalAcceptMode::Execute ? GoalState::Executing : GoalState::Accepted;
    return Result<GoalTransition>::success(transition);
}

Result<GoalState> ActionServer::Impl::goal_state(const GoalId& goal_id) const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<GoalState>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return goals_->state(goal_id);
}

Result<GoalTransition> ActionServer::Impl::update_goal_state(
    const GoalId& goal_id, GoalEvent event) {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<GoalTransition>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return goals_->update_state(goal_id, event);
}

Result<CancelSelection> ActionServer::Impl::select_cancel_goals(
    const CancelGoalCriteria& criteria) const {
    if (criteria.stamp < std::chrono::nanoseconds::zero()) {
        return Result<CancelSelection>::failure(
            Error(ErrorCode::InvalidArgument, "Cancel stamp must not be negative"));
    }
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<CancelSelection>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return goals_->select_cancel_goals(criteria);
}

Result<ResultRequestDisposition> ActionServer::Impl::register_result_request(
    const GoalId& goal_id, const RequestId& request_id) {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<ResultRequestDisposition>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    std::lock_guard lock(result_coordination_mutex_);
    auto pending = result_server_->impl_->check_pending_request(request_id);
    if (!pending) return Result<ResultRequestDisposition>::failure(std::move(pending.error()));
    return goals_->register_result_request(goal_id, request_id);
}

Result<std::vector<RequestId>> ActionServer::Impl::take_pending_result_requests(
    const GoalId& goal_id) {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<std::vector<RequestId>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    std::lock_guard lock(result_coordination_mutex_);
    return goals_->take_pending_result_requests(goal_id);
}

Result<std::vector<GoalStatusInfo>> ActionServer::Impl::status_snapshot() const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<std::vector<GoalStatusInfo>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return goals_->status_snapshot();
}

Result<std::vector<GoalId>> ActionServer::Impl::take_expired_goals() {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<std::vector<GoalId>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    return goals_->take_expired_goals();
}

Result<ActionServerReadySet> ActionServer::Impl::readiness() const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<ActionServerReadySet>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    ActionServerReadySet ready;
    ready.goal_request = goal_server_->impl_->wait_state()->is_ready();
    ready.cancel_request = cancel_server_->impl_->wait_state()->is_ready();
    ready.result_request = result_server_->impl_->wait_state()->is_ready();
    ready.goal_expired = goals_->expiry_ready();
    return Result<ActionServerReadySet>::success(ready);
}

std::vector<std::shared_ptr<impl::ReaderWaitState>> ActionServer::Impl::wait_states() const {
    return {
        goal_server_->impl_->wait_state(),
        cancel_server_->impl_->wait_state(),
        result_server_->impl_->wait_state(),
    };
}

ActionClient::ActionClient(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ActionClient::~ActionClient() noexcept = default;

Result<RequestId> ActionClient::write_goal_request(const void* request) {
    return impl_->write_goal_request(request);
}
Result<bool> ActionClient::read_goal_response(void* response, RequestId& request_id) {
    return impl_->read_goal_response(response, request_id);
}
Result<RequestId> ActionClient::write_cancel_request(const void* request) {
    return impl_->write_cancel_request(request);
}
Result<bool> ActionClient::read_cancel_response(void* response, RequestId& request_id) {
    return impl_->read_cancel_response(response, request_id);
}
Result<RequestId> ActionClient::write_result_request(const void* request) {
    return impl_->write_result_request(request);
}
Result<bool> ActionClient::read_result_response(void* response, RequestId& request_id) {
    return impl_->read_result_response(response, request_id);
}
Result<bool> ActionClient::read_feedback(void* feedback, MessageInfo& info) {
    return impl_->read_feedback(feedback, info);
}
Result<bool> ActionClient::read_status(void* status, MessageInfo& info) {
    return impl_->read_status(status, info);
}
Result<bool> ActionClient::server_is_available() const { return impl_->server_is_available(); }
Result<AvailabilityWaitToken> ActionClient::prepare_availability_wait() const {
    return impl_->prepare_availability_wait();
}
Result<void> ActionClient::interrupt_waits() { return impl_->interrupt_waits(); }
Result<bool> ActionClient::wait_for_server(
    WaitTimeout timeout, const AvailabilityWaitToken& token) const {
    return impl_->wait_for_server(timeout, token);
}
Result<bool> ActionClient::wait_for_server(WaitTimeout timeout) const {
    return impl_->wait_for_server(timeout);
}
Result<ActionClientReadySet> ActionClient::readiness() const { return impl_->readiness(); }
std::string_view ActionClient::action_name() const noexcept { return impl_->action_name(); }

ActionServer::ActionServer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ActionServer::~ActionServer() noexcept = default;

Result<bool> ActionServer::read_goal_request(void* request, RequestId& request_id) {
    return impl_->read_goal_request(request, request_id);
}
Result<void> ActionServer::write_goal_response(
    const RequestId& request_id, const void* rejected_response) {
    return impl_->write_goal_response(request_id, rejected_response);
}
Result<bool> ActionServer::read_cancel_request(void* request, RequestId& request_id) {
    return impl_->read_cancel_request(request, request_id);
}
Result<void> ActionServer::write_cancel_response(
    const RequestId& request_id, const void* response) {
    return impl_->write_cancel_response(request_id, response);
}
Result<bool> ActionServer::read_result_request(void* request, RequestId& request_id) {
    return impl_->read_result_request(request, request_id);
}
Result<void> ActionServer::write_result_response(
    const RequestId& request_id, const void* response) {
    return impl_->write_result_response(request_id, response);
}
Result<void> ActionServer::discard_goal_request(const RequestId& request_id) {
    return impl_->discard_goal_request(request_id);
}
Result<void> ActionServer::discard_cancel_request(const RequestId& request_id) {
    return impl_->discard_cancel_request(request_id);
}
Result<void> ActionServer::discard_result_request(const RequestId& request_id) {
    return impl_->discard_result_request(request_id);
}
Result<void> ActionServer::publish_feedback(const void* feedback) {
    return impl_->publish_feedback(feedback);
}
Result<void> ActionServer::publish_status(const void* status) {
    return impl_->publish_status(status);
}
Result<GoalTransition> ActionServer::accept_goal(
    const RequestId& request_id, const GoalInfo& goal_info, const void* accepted_response,
    GoalAcceptMode mode) {
    return impl_->accept_goal(request_id, goal_info, accepted_response, mode);
}
Result<GoalState> ActionServer::goal_state(const GoalId& goal_id) const {
    return impl_->goal_state(goal_id);
}
Result<GoalTransition> ActionServer::update_goal_state(const GoalId& goal_id, GoalEvent event) {
    return impl_->update_goal_state(goal_id, event);
}
Result<CancelSelection> ActionServer::select_cancel_goals(
    const CancelGoalCriteria& criteria) const {
    return impl_->select_cancel_goals(criteria);
}
Result<ResultRequestDisposition> ActionServer::register_result_request(
    const GoalId& goal_id, const RequestId& request_id) {
    return impl_->register_result_request(goal_id, request_id);
}
Result<std::vector<RequestId>> ActionServer::take_pending_result_requests(const GoalId& goal_id) {
    return impl_->take_pending_result_requests(goal_id);
}
Result<std::vector<GoalStatusInfo>> ActionServer::status_snapshot() const {
    return impl_->status_snapshot();
}
Result<std::vector<GoalId>> ActionServer::take_expired_goals() {
    return impl_->take_expired_goals();
}
Result<ActionServerReadySet> ActionServer::readiness() const { return impl_->readiness(); }
std::string_view ActionServer::action_name() const noexcept { return impl_->action_name(); }

}  // namespace dmw
