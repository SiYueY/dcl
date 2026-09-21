#ifndef DMW_ACTION_SERVER_HPP_
#define DMW_ACTION_SERVER_HPP_

#include <chrono>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "dmw/action_common.hpp"
#include "dmw/action_type.hpp"
#include "dmw/qos.hpp"
#include "dmw/request_id.hpp"
#include "dmw/result.hpp"
#include "dmw/visibility_control.hpp"

namespace dmw {

class Node;
class WaitSet;

/// Per-endpoint QoS plus terminal goal retention of one ActionServer.
struct ActionServerOptions {
    Qos goal_service_qos{Qos::ros2_services_default()};
    Qos cancel_service_qos{Qos::ros2_services_default()};
    Qos result_service_qos{Qos::ros2_services_default()};
    Qos feedback_topic_qos{Qos::ros2_default()};
    Qos status_topic_qos{Qos::ros2_action_status_default()};

    /// How long a terminal goal stays visible for late GetResult requests.
    std::chrono::nanoseconds result_timeout{std::chrono::seconds(10)};
};

/// Aggregate Action server primitive and the sole Goal FSM authority.
///
/// Accepted responses are published through accept_goal(), which makes the
/// wire response and the local GoalRecord one observable transaction.
class DMW_PUBLIC ActionServer {
public:
    ~ActionServer() noexcept;

    ActionServer(const ActionServer&) = delete;
    ActionServer& operator=(const ActionServer&) = delete;
    ActionServer(ActionServer&&) = delete;
    ActionServer& operator=(ActionServer&&) = delete;

    Result<bool> read_goal_request(void* request, RequestId& request_id);
    /// Raw response path for rejected/non-accepted goals only.
    Result<void> write_goal_response(const RequestId& request_id, const void* rejected_response);

    Result<bool> read_cancel_request(void* request, RequestId& request_id);
    Result<void> write_cancel_response(const RequestId& request_id, const void* response);

    Result<bool> read_result_request(void* request, RequestId& request_id);
    Result<void> write_result_response(const RequestId& request_id, const void* response);

    Result<void> publish_feedback(const void* feedback);
    Result<void> publish_status(const void* status);

    /// Commit an accepted goal and its wire response as one transaction.
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

    std::string_view action_name() const noexcept;

private:
    friend class Node;
    friend class WaitSet;

    class Impl;

    explicit ActionServer(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace dmw

#endif  // DMW_ACTION_SERVER_HPP_
