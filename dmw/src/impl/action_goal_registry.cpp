#include "impl/action_goal_registry.hpp"

#include <algorithm>
#include <utility>

#include "dmw/error.hpp"

namespace dmw::impl {

namespace {

/// Goal FSM from dmw.md §8.4.  Any other transition is InvalidState.
std::optional<GoalState> next_goal_state(GoalState state, GoalEvent event) noexcept {
    switch (state) {
        case GoalState::Accepted:
            if (event == GoalEvent::Execute) return GoalState::Executing;
            if (event == GoalEvent::CancelGoal) return GoalState::Canceling;
            return std::nullopt;
        case GoalState::Executing:
            if (event == GoalEvent::CancelGoal) return GoalState::Canceling;
            if (event == GoalEvent::Succeed) return GoalState::Succeeded;
            if (event == GoalEvent::Abort) return GoalState::Aborted;
            return std::nullopt;
        case GoalState::Canceling:
            if (event == GoalEvent::Succeed) return GoalState::Succeeded;
            if (event == GoalEvent::Abort) return GoalState::Aborted;
            if (event == GoalEvent::Canceled) return GoalState::Canceled;
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

}  // namespace

bool ActionGoalRegistry::reserve(const GoalId& goal_id) {
    std::lock_guard lock(mutex_);
    if (goals_.count(goal_id) != 0) return false;
    // The insertion allocates here, before any wire write, so the later commit
    // cannot fail on allocation.
    goals_.emplace(goal_id, GoalRecord{});
    return true;
}

void ActionGoalRegistry::rollback_reservation(const GoalId& goal_id) noexcept {
    std::lock_guard lock(mutex_);
    const auto found = goals_.find(goal_id);
    if (found != goals_.end() && !found->second.committed) goals_.erase(found);
}

bool ActionGoalRegistry::is_reserved(const GoalId& goal_id) const {
    std::lock_guard lock(mutex_);
    return goals_.count(goal_id) != 0;
}

bool ActionGoalRegistry::commit(const GoalInfo& goal_info, GoalAcceptMode mode) noexcept {
    std::lock_guard lock(mutex_);
    const auto found = goals_.find(goal_info.goal_id);
    if (found == goals_.end() || found->second.committed) return false;
    GoalRecord& record = found->second;
    record.info = goal_info;
    record.state = mode == GoalAcceptMode::Execute ? GoalState::Executing : GoalState::Accepted;
    record.committed = true;
    record.expiry_scheduled = false;
    record.pending_result_requests.clear();
    return true;
}

bool ActionGoalRegistry::expired_locked(
    const GoalRecord& record, std::chrono::steady_clock::time_point now) const noexcept {
    if (!record.expiry_scheduled) return false;
    return now - record.terminal_time >= result_timeout_;
}

void ActionGoalRegistry::prune_expired_locked(
    std::chrono::steady_clock::time_point now, std::vector<GoalId>* removed) {
    for (auto entry = goals_.begin(); entry != goals_.end();) {
        if (entry->second.committed && expired_locked(entry->second, now)) {
            if (removed != nullptr) removed->push_back(entry->first);
            entry = goals_.erase(entry);
            continue;
        }
        ++entry;
    }
}

Result<GoalState> ActionGoalRegistry::state(const GoalId& goal_id) const {
    std::lock_guard lock(mutex_);
    const auto found = goals_.find(goal_id);
    if (found == goals_.end() || !found->second.committed) {
        return Result<GoalState>::failure(Error(ErrorCode::NotFound, "Goal is not registered"));
    }
    return Result<GoalState>::success(found->second.state);
}

Result<GoalTransition> ActionGoalRegistry::update_state(const GoalId& goal_id, GoalEvent event) {
    std::lock_guard lock(mutex_);
    const auto found = goals_.find(goal_id);
    if (found == goals_.end() || !found->second.committed) {
        return Result<GoalTransition>::failure(
            Error(ErrorCode::NotFound, "Goal is not registered"));
    }
    GoalRecord& record = found->second;
    if (is_terminal_goal_state(record.state)) {
        return Result<GoalTransition>::failure(
            Error(ErrorCode::InvalidState, "Goal is already terminal"));
    }
    const auto next = next_goal_state(record.state, event);
    if (!next) {
        return Result<GoalTransition>::failure(
            Error(ErrorCode::InvalidState, "Goal transition is not allowed"));
    }
    GoalTransition transition;
    transition.previous = record.state;
    transition.current = next.value();
    record.state = next.value();
    if (is_terminal_goal_state(record.state)) {
        transition.became_terminal = true;
        record.terminal_time = std::chrono::steady_clock::now();
        record.expiry_scheduled = true;
    }
    return Result<GoalTransition>::success(transition);
}

Result<CancelSelection> ActionGoalRegistry::select_cancel_goals(
    const CancelGoalCriteria& criteria) const {
    if (criteria.stamp < std::chrono::nanoseconds::zero()) {
        return Result<CancelSelection>::failure(
            Error(ErrorCode::InvalidArgument, "Cancel stamp must not be negative"));
    }
    const bool wildcard = is_zero_goal_id(criteria.goal_id);
    const bool time_bounded = criteria.stamp != std::chrono::nanoseconds::zero();
    std::lock_guard lock(mutex_);
    CancelSelection selection;
    for (const auto& entry : goals_) {
        const GoalRecord& record = entry.second;
        if (!record.committed || !is_cancelable_goal_state(record.state)) continue;
        const bool exact_match = !wildcard && record.info.goal_id == criteria.goal_id;
        const bool before_stamp = time_bounded && record.info.accepted_stamp <= criteria.stamp;
        // The four documented forms collapse to: exact id OR time bound, with
        // "all cancelable goals" being the unbounded wildcard case.
        const bool selected = wildcard ? (time_bounded ? before_stamp : true)
                                       : (time_bounded ? (exact_match || before_stamp)
                                                       : exact_match);
        if (selected) selection.goals.push_back(record.info);
    }
    std::sort(
        selection.goals.begin(), selection.goals.end(),
        [](const GoalInfo& lhs, const GoalInfo& rhs) {
            return lhs.accepted_stamp < rhs.accepted_stamp;
        });
    return Result<CancelSelection>::success(std::move(selection));
}

Result<ResultRequestDisposition> ActionGoalRegistry::register_result_request(
    const GoalId& goal_id, const RequestId& request_id) {
    std::lock_guard lock(mutex_);
    prune_expired_locked(std::chrono::steady_clock::now(), nullptr);
    const auto found = goals_.find(goal_id);
    if (found == goals_.end() || !found->second.committed) {
        return Result<ResultRequestDisposition>::success(ResultRequestDisposition::UnknownGoal);
    }
    GoalRecord& record = found->second;
    if (is_terminal_goal_state(record.state)) {
        return Result<ResultRequestDisposition>::success(ResultRequestDisposition::Terminal);
    }
    record.pending_result_requests.push_back(request_id);
    return Result<ResultRequestDisposition>::success(ResultRequestDisposition::Pending);
}

Result<std::vector<RequestId>> ActionGoalRegistry::take_pending_result_requests(
    const GoalId& goal_id) {
    std::lock_guard lock(mutex_);
    const auto found = goals_.find(goal_id);
    if (found == goals_.end() || !found->second.committed) {
        return Result<std::vector<RequestId>>::failure(
            Error(ErrorCode::NotFound, "Goal is not registered"));
    }
    std::vector<RequestId> requests;
    requests.swap(found->second.pending_result_requests);
    return Result<std::vector<RequestId>>::success(std::move(requests));
}

Result<std::vector<GoalStatusInfo>> ActionGoalRegistry::status_snapshot() {
    std::lock_guard lock(mutex_);
    prune_expired_locked(std::chrono::steady_clock::now(), nullptr);
    std::vector<GoalStatusInfo> snapshot;
    snapshot.reserve(goals_.size());
    for (const auto& entry : goals_) {
        if (!entry.second.committed) continue;
        snapshot.push_back(GoalStatusInfo{entry.second.info, entry.second.state});
    }
    std::sort(
        snapshot.begin(), snapshot.end(),
        [](const GoalStatusInfo& lhs, const GoalStatusInfo& rhs) {
            return lhs.goal_info.accepted_stamp < rhs.goal_info.accepted_stamp;
        });
    return Result<std::vector<GoalStatusInfo>>::success(std::move(snapshot));
}

Result<std::vector<GoalId>> ActionGoalRegistry::take_expired_goals() {
    std::lock_guard lock(mutex_);
    std::vector<GoalId> expired;
    prune_expired_locked(std::chrono::steady_clock::now(), &expired);
    return Result<std::vector<GoalId>>::success(std::move(expired));
}

Result<std::optional<std::chrono::steady_clock::time_point>> ActionGoalRegistry::earliest_expiry()
    const {
    std::lock_guard lock(mutex_);
    std::optional<std::chrono::steady_clock::time_point> earliest;
    for (const auto& entry : goals_) {
        if (!entry.second.committed || !entry.second.expiry_scheduled) continue;
        const auto deadline = entry.second.terminal_time + result_timeout_;
        if (!earliest || deadline < *earliest) earliest = deadline;
    }
    return Result<std::optional<std::chrono::steady_clock::time_point>>::success(earliest);
}

std::size_t ActionGoalRegistry::committed_goal_count() const {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& entry : goals_) {
        if (entry.second.committed) ++count;
    }
    return count;
}

}  // namespace dmw::impl
