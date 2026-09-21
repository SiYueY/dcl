#include "impl/action_impl.hpp"

#include <chrono>
#include <thread>
#include <utility>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/rtps/common/InstanceHandle.h>

#include "dmw/error.hpp"
#include "impl/client_impl.hpp"
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
            state->revision.fetch_add(1, std::memory_order_release);
            state->cv.notify_all();
        }
    });
    shutdown_callback_id_ = context_->register_shutdown_callback([weak_state] {
        if (const auto state = weak_state.lock()) {
            state->revision.fetch_add(1, std::memory_order_release);
            state->cv.notify_all();
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

Result<bool> ActionClient::Impl::wait_for_server(WaitTimeout timeout) const {
    const auto deadline = timeout.kind() == WaitTimeout::Kind::Finite
                              ? std::chrono::steady_clock::now() + timeout.duration()
                              : std::chrono::steady_clock::time_point::max();
    auto observed = availability_state_->revision.load(std::memory_order_acquire);
    while (true) {
        if (context_->is_shutdown()) {
            return Result<bool>::failure(
                Error(ErrorCode::ContextShutdown, "Context is shut down"));
        }
        auto available = check_availability();
        if (!available) return Result<bool>::failure(std::move(available.error()));
        if (available.value()) return Result<bool>::success(true);
        if (timeout.kind() == WaitTimeout::Kind::Poll) return Result<bool>::success(false);
        std::unique_lock lock(availability_state_->mutex);
        const auto current = availability_state_->revision.load(std::memory_order_acquire);
        if (current != observed) {
            observed = current;
            continue;
        }
        // Discovery revision changes and Context shutdown both wake this wait;
        // the loop re-evaluates availability against the original deadline.
        const auto changed = [this, observed] {
            return availability_state_->revision.load(std::memory_order_acquire) != observed;
        };
        if (timeout.kind() == WaitTimeout::Kind::Finite) {
            if (!availability_state_->cv.wait_until(lock, deadline, changed)) {
                return Result<bool>::success(false);
            }
        } else {
            availability_state_->cv.wait(lock, changed);
        }
        observed = availability_state_->revision.load(std::memory_order_acquire);
    }
}

Result<ActionClientReadySet> ActionClient::Impl::readiness() const {
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
    return result_server_->write_response(request_id, response);
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
    auto written = goal_server_->write_response(request_id, accepted_response);
    if (!written) {
        goals_->rollback_reservation(goal_info.goal_id);
        return Result<GoalTransition>::failure(std::move(written.error()));
    }
    if (!goals_->commit(goal_info, mode)) {
        goals_->rollback_reservation(goal_info.goal_id);
        return Result<GoalTransition>::failure(
            Error(ErrorCode::InvalidState, "Goal reservation was lost before commit"));
    }
    GoalTransition transition;
    transition.previous = GoalState::Unknown;
    transition.current =
        mode == GoalAcceptMode::Execute ? GoalState::Executing : GoalState::Accepted;
    return Result<GoalTransition>::success(transition);
}

Result<GoalState> ActionServer::Impl::goal_state(const GoalId& goal_id) const {
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
    return goals_->select_cancel_goals(criteria);
}

Result<ResultRequestDisposition> ActionServer::Impl::register_result_request(
    const GoalId& goal_id, const RequestId& request_id) {
    return goals_->register_result_request(goal_id, request_id);
}

Result<std::vector<RequestId>> ActionServer::Impl::take_pending_result_requests(
    const GoalId& goal_id) {
    return goals_->take_pending_result_requests(goal_id);
}

Result<std::vector<GoalStatusInfo>> ActionServer::Impl::status_snapshot() const {
    return goals_->status_snapshot();
}

Result<std::vector<GoalId>> ActionServer::Impl::take_expired_goals() {
    return goals_->take_expired_goals();
}

Result<ActionServerReadySet> ActionServer::Impl::readiness() const {
    ActionServerReadySet ready;
    ready.goal_request = goal_server_->impl_->wait_state()->is_ready();
    ready.cancel_request = cancel_server_->impl_->wait_state()->is_ready();
    ready.result_request = result_server_->impl_->wait_state()->is_ready();
    const auto expiry = goals_->earliest_expiry();
    if (expiry && expiry.value().has_value()) {
        ready.goal_expired = std::chrono::steady_clock::now() >= *expiry.value();
    }
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
