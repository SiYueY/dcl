#ifndef DMW_IMPL__ACTION_IMPL_HPP_
#define DMW_IMPL__ACTION_IMPL_HPP_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "dmw/action_client.hpp"
#include "dmw/action_server.hpp"
#include "dmw/client.hpp"
#include "dmw/publisher.hpp"
#include "dmw/server.hpp"
#include "dmw/subscriber.hpp"
#include "impl/action_goal_registry.hpp"
#include "impl/context.hpp"
#include "impl/discovery_graph.hpp"
#include "impl/graph_candidates.hpp"
#include "impl/graph_names.hpp"
#include "impl/reader_wait_state.hpp"

namespace dmw {

namespace impl {

/// Wire type names of one ActionType, used for availability composition.
inline ActionEndpointTypes action_endpoint_types(const ActionType& type) {
    ActionEndpointTypes types;
    types.send_goal_request = std::string(type.send_goal_type().request_type().type_name());
    types.send_goal_response = std::string(type.send_goal_type().response_type().type_name());
    types.cancel_goal_request = std::string(type.cancel_goal_type().request_type().type_name());
    types.cancel_goal_response = std::string(type.cancel_goal_type().response_type().type_name());
    types.get_result_request = std::string(type.get_result_type().request_type().type_name());
    types.get_result_response = std::string(type.get_result_type().response_type().type_name());
    types.feedback = std::string(type.feedback_type().type_name());
    types.status = std::string(type.status_type().type_name());
    return types;
}

}  // namespace impl

class ActionClient::Impl {
public:
    Impl(
        std::shared_ptr<impl::Context> context, std::string action_name,
        impl::ActionEndpointNames names, impl::ActionEndpointTypes types,
        std::unique_ptr<Client> goal_client, std::unique_ptr<Client> cancel_client,
        std::unique_ptr<Client> result_client, std::unique_ptr<Subscriber> feedback_subscriber,
        std::unique_ptr<Subscriber> status_subscriber);
    ~Impl() noexcept;

    Result<RequestId> write_goal_request(const void* request);
    Result<bool> read_goal_response(void* response, RequestId& request_id);
    Result<RequestId> write_cancel_request(const void* request);
    Result<bool> read_cancel_response(void* response, RequestId& request_id);
    Result<RequestId> write_result_request(const void* request);
    Result<bool> read_result_response(void* response, RequestId& request_id);
    Result<bool> read_feedback(void* feedback, MessageInfo& info);
    Result<bool> read_status(void* status, MessageInfo& info);

    Result<bool> server_is_available() const;
    Result<bool> wait_for_server(WaitTimeout timeout) const;
    Result<ActionClientReadySet> readiness() const;

    std::string_view action_name() const noexcept { return action_name_; }

    std::vector<std::shared_ptr<impl::ReaderWaitState>> wait_states() const;
    bool initialized() const noexcept {
        return static_cast<bool>(availability_subscription_) && shutdown_callback_id_ != 0;
    }

private:
    Result<bool> check_availability() const;

    /// Wakes wait_for_server() on discovery revision change or shutdown
    /// without any fixed polling slice.
    struct AvailabilityWaitState {
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<std::uint64_t> revision{0};
    };

    std::shared_ptr<impl::Context> context_;
    std::string action_name_;
    impl::ActionEndpointNames names_;
    impl::ActionEndpointTypes types_;
    std::string local_participant_;
    std::unique_ptr<Client> goal_client_;
    std::unique_ptr<Client> cancel_client_;
    std::unique_ptr<Client> result_client_;
    std::unique_ptr<Subscriber> feedback_subscriber_;
    std::unique_ptr<Subscriber> status_subscriber_;
    std::shared_ptr<AvailabilityWaitState> availability_state_;
    impl::DiscoveryGraph::Subscription availability_subscription_;
    std::uint64_t shutdown_callback_id_{0};
};

class ActionServer::Impl {
public:
    Impl(
        std::shared_ptr<impl::Context> context, std::string action_name,
        std::unique_ptr<Server> goal_server, std::unique_ptr<Server> cancel_server,
        std::unique_ptr<Server> result_server, std::unique_ptr<Publisher> feedback_publisher,
        std::unique_ptr<Publisher> status_publisher, std::chrono::nanoseconds result_timeout);
    ~Impl() noexcept;

    Result<bool> read_goal_request(void* request, RequestId& request_id);
    Result<void> write_goal_response(const RequestId& request_id, const void* rejected_response);
    Result<bool> read_cancel_request(void* request, RequestId& request_id);
    Result<void> write_cancel_response(const RequestId& request_id, const void* response);
    Result<bool> read_result_request(void* request, RequestId& request_id);
    Result<void> write_result_response(const RequestId& request_id, const void* response);
    Result<void> publish_feedback(const void* feedback);
    Result<void> publish_status(const void* status);

    Result<GoalTransition> accept_goal(
        const RequestId& request_id, const GoalInfo& goal_info, const void* accepted_response,
        GoalAcceptMode mode);

    Result<GoalState> goal_state(const GoalId& goal_id) const;
    Result<GoalTransition> update_goal_state(const GoalId& goal_id, GoalEvent event);
    Result<CancelSelection> select_cancel_goals(const CancelGoalCriteria& criteria) const;
    Result<ResultRequestDisposition> register_result_request(
        const GoalId& goal_id, const RequestId& request_id);
    Result<std::vector<RequestId>> take_pending_result_requests(const GoalId& goal_id);
    Result<std::vector<GoalStatusInfo>> status_snapshot() const;
    Result<std::vector<GoalId>> take_expired_goals();
    Result<ActionServerReadySet> readiness() const;

    std::string_view action_name() const noexcept { return action_name_; }

    std::vector<std::shared_ptr<impl::ReaderWaitState>> wait_states() const;

    /// Shared so WaitSet registrations can observe expiry through weak_ptr
    /// without extending the ActionServer's lifetime.
    const std::shared_ptr<impl::ActionGoalRegistry>& goals() const noexcept { return goals_; }

private:
    std::shared_ptr<impl::Context> context_;
    std::string action_name_;
    std::unique_ptr<Server> goal_server_;
    std::unique_ptr<Server> cancel_server_;
    std::unique_ptr<Server> result_server_;
    std::unique_ptr<Publisher> feedback_publisher_;
    std::unique_ptr<Publisher> status_publisher_;
    std::shared_ptr<impl::ActionGoalRegistry> goals_;
};

}  // namespace dmw

#endif  // DMW_IMPL__ACTION_IMPL_HPP_
