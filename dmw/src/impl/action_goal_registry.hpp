#ifndef DMW_IMPL__ACTION_GOAL_REGISTRY_HPP_
#define DMW_IMPL__ACTION_GOAL_REGISTRY_HPP_

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "dmw/action_common.hpp"
#include "dmw/result.hpp"

namespace dmw::impl {

/// Node-local Action goal state.
///
/// This is the sole authority for the Goal FSM, accept reservations, pending
/// GetResult requests and result expiry.  It owns no DDS entity and never runs
/// a background thread.
class ActionGoalRegistry {
public:
    explicit ActionGoalRegistry(std::chrono::nanoseconds result_timeout) noexcept
    : result_timeout_(result_timeout < std::chrono::nanoseconds::zero()
                          ? std::chrono::nanoseconds::zero()
                          : result_timeout) {}

    /// Reserve a GoalId before the accepted response is written.  False means
    /// the id is already reserved or committed.
    bool reserve(const GoalId& goal_id);

    /// Drop a reservation after a failed response write.
    void rollback_reservation(const GoalId& goal_id) noexcept;

    bool is_reserved(const GoalId& goal_id) const;

    /// Commit a reservation.  This must not allocate: it only publishes state
    /// that the reservation already prepared.
    bool commit(const GoalInfo& goal_info, GoalAcceptMode mode) noexcept;

    Result<GoalState> state(const GoalId& goal_id) const;

    Result<GoalTransition> update_state(const GoalId& goal_id, GoalEvent event);

    Result<CancelSelection> select_cancel_goals(const CancelGoalCriteria& criteria) const;

    Result<ResultRequestDisposition> register_result_request(
        const GoalId& goal_id, const RequestId& request_id);

    Result<std::vector<RequestId>> take_pending_result_requests(const GoalId& goal_id);

    Result<std::vector<GoalStatusInfo>> status_snapshot();

    Result<std::vector<GoalId>> take_expired_goals();

    /// Earliest absolute steady deadline at which a terminal goal expires.
    Result<std::optional<std::chrono::steady_clock::time_point>> earliest_expiry() const;

    /// Number of committed goals; reserved-but-uncommitted goals are excluded.
    std::size_t committed_goal_count() const;

private:
    struct GoalRecord {
        GoalInfo info;
        GoalState state{GoalState::Unknown};
        bool committed{false};
        bool expiry_scheduled{false};
        std::chrono::steady_clock::time_point terminal_time{};
        std::vector<RequestId> pending_result_requests;
    };

    bool expired_locked(
        const GoalRecord& record, std::chrono::steady_clock::time_point now) const noexcept;

    /// Lazy retention pruning; collects removed ids when requested.
    void prune_expired_locked(
        std::chrono::steady_clock::time_point now, std::vector<GoalId>* removed);

    mutable std::mutex mutex_;
    const std::chrono::nanoseconds result_timeout_;
    std::unordered_map<GoalId, GoalRecord, GoalIdHash> goals_;
};

}  // namespace dmw::impl

#endif  // DMW_IMPL__ACTION_GOAL_REGISTRY_HPP_
