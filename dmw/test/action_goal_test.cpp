#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "dmw/action_common.hpp"
#include "impl/action_goal_registry.hpp"
#include "impl/graph_names.hpp"

namespace {

using namespace std::chrono_literals;
using dmw::GoalAcceptMode;
using dmw::GoalEvent;
using dmw::GoalId;
using dmw::GoalInfo;
using dmw::GoalState;
using dmw::GoalTransition;
using dmw::ResultRequestDisposition;

GoalId goal_id(std::uint8_t seed) {
    GoalId id;
    id.data[0] = seed;
    id.data[15] = seed;
    return id;
}

GoalInfo goal_info(std::uint8_t seed, std::int64_t stamp_ns) {
    GoalInfo info;
    info.goal_id = goal_id(seed);
    info.accepted_stamp = std::chrono::nanoseconds(stamp_ns);
    return info;
}

dmw::RequestId request_id(std::uint8_t seed) {
    dmw::RequestId id;
    id.client_gid.data[0] = seed;
    id.sequence_number = seed;
    return id;
}

void test_reservation_and_commit() {
    dmw::impl::ActionGoalRegistry registry(10s);
    const auto first = goal_id(1);

    assert(registry.reserve(first));
    assert(!registry.reserve(first));
    // A reserved goal is not publicly visible before the accept transaction
    // commits it.
    assert(!registry.state(first));
    assert(registry.state(first).error().code() == dmw::ErrorCode::NotFound);
    assert(registry.committed_goal_count() == 0);

    assert(registry.commit(goal_info(1, 100), GoalAcceptMode::Defer));
    assert(!registry.commit(goal_info(1, 100), GoalAcceptMode::Defer));
    assert(registry.committed_goal_count() == 1);
    assert(registry.state(first).value() == GoalState::Accepted);

    const auto second = goal_id(2);
    assert(registry.reserve(second));
    registry.rollback_reservation(second);
    assert(!registry.is_reserved(second));
    assert(!registry.commit(goal_info(2, 200), GoalAcceptMode::Execute));

    const auto third = goal_id(3);
    assert(registry.reserve(third));
    assert(registry.commit(goal_info(3, 300), GoalAcceptMode::Execute));
    assert(registry.state(third).value() == GoalState::Executing);
}

void test_state_machine() {
    dmw::impl::ActionGoalRegistry registry(10s);
    const auto deferred = goal_id(1);
    const auto executing = goal_id(2);
    const auto canceling = goal_id(3);
    registry.reserve(deferred);
    registry.commit(goal_info(1, 1), GoalAcceptMode::Defer);
    registry.reserve(executing);
    registry.commit(goal_info(2, 2), GoalAcceptMode::Execute);
    registry.reserve(canceling);
    registry.commit(goal_info(3, 3), GoalAcceptMode::Execute);

    const auto transition = registry.update_state(deferred, GoalEvent::Execute);
    assert(transition);
    assert(transition.value().previous == GoalState::Accepted);
    assert(transition.value().current == GoalState::Executing);
    assert(!transition.value().became_terminal);

    // Illegal transition leaves the state untouched.  Canceled is only
    // reachable from Canceling, never directly from Executing.
    const auto illegal = registry.update_state(deferred, GoalEvent::Canceled);
    assert(!illegal);
    assert(illegal.error().code() == dmw::ErrorCode::InvalidState);
    assert(registry.state(deferred).value() == GoalState::Executing);

    assert(registry.update_state(executing, GoalEvent::CancelGoal));
    assert(registry.update_state(executing, GoalEvent::Canceled));
    assert(registry.state(executing).value() == GoalState::Canceled);
    const auto after_terminal = registry.update_state(executing, GoalEvent::Succeed);
    assert(!after_terminal);
    assert(after_terminal.error().code() == dmw::ErrorCode::InvalidState);

    const auto terminal = registry.update_state(canceling, GoalEvent::CancelGoal);
    assert(terminal);
    assert(terminal.value().current == GoalState::Canceling);
    const auto canceled = registry.update_state(canceling, GoalEvent::Canceled);
    assert(canceled);
    assert(canceled.value().became_terminal);
    assert(canceled.value().current == GoalState::Canceled);

    const auto missing = registry.update_state(goal_id(9), GoalEvent::Execute);
    assert(!missing);
    assert(missing.error().code() == dmw::ErrorCode::NotFound);
}

void test_cancel_selection() {
    dmw::impl::ActionGoalRegistry registry(10s);
    // 1 = Accepted at stamp 100, 2 = Executing at stamp 200, 3 = terminal.
    registry.reserve(goal_id(1));
    registry.commit(goal_info(1, 100), GoalAcceptMode::Defer);
    registry.reserve(goal_id(2));
    registry.commit(goal_info(2, 200), GoalAcceptMode::Execute);
    registry.reserve(goal_id(3));
    registry.commit(goal_info(3, 300), GoalAcceptMode::Execute);
    assert(registry.update_state(goal_id(3), GoalEvent::Succeed));

    // Exact id, no time bound.
    dmw::CancelGoalCriteria exact;
    exact.goal_id = goal_id(1);
    const auto exact_selection = registry.select_cancel_goals(exact);
    assert(exact_selection);
    assert(exact_selection.value().goals.size() == 1);
    assert(exact_selection.value().goals.front().goal_id == goal_id(1));

    // Wildcard, no time bound: every cancelable goal, never terminal ones.
    dmw::CancelGoalCriteria all;
    const auto all_selection = registry.select_cancel_goals(all);
    assert(all_selection);
    assert(all_selection.value().goals.size() == 2);

    // Wildcard with a stamp selects goals accepted at or before it.
    dmw::CancelGoalCriteria bounded;
    bounded.stamp = 150ns;
    const auto bounded_selection = registry.select_cancel_goals(bounded);
    assert(bounded_selection);
    assert(bounded_selection.value().goals.size() == 1);
    assert(bounded_selection.value().goals.front().goal_id == goal_id(1));

    // Explicit id plus a stamp selects the exact goal or everything older.
    dmw::CancelGoalCriteria combined;
    combined.goal_id = goal_id(2);
    combined.stamp = 150ns;
    const auto combined_selection = registry.select_cancel_goals(combined);
    assert(combined_selection);
    assert(combined_selection.value().goals.size() == 2);

    // Boundary: a stamp equal to accepted_stamp is included.
    dmw::CancelGoalCriteria boundary;
    boundary.stamp = 100ns;
    const auto boundary_selection = registry.select_cancel_goals(boundary);
    assert(boundary_selection);
    assert(boundary_selection.value().goals.size() == 1);

    dmw::CancelGoalCriteria unknown;
    unknown.goal_id = goal_id(7);
    assert(registry.select_cancel_goals(unknown).value().goals.empty());

    dmw::CancelGoalCriteria negative;
    negative.stamp = -1ns;
    const auto rejected = registry.select_cancel_goals(negative);
    assert(!rejected);
    assert(rejected.error().code() == dmw::ErrorCode::InvalidArgument);
}

void test_result_requests_and_expiry() {
    dmw::impl::ActionGoalRegistry registry(60ms);
    const auto active = goal_id(1);
    const auto terminal = goal_id(2);
    registry.reserve(active);
    registry.commit(goal_info(1, 1), GoalAcceptMode::Execute);
    registry.reserve(terminal);
    registry.commit(goal_info(2, 2), GoalAcceptMode::Execute);

    assert(
        registry.register_result_request(goal_id(9), request_id(9)).value() ==
        ResultRequestDisposition::UnknownGoal);
    assert(
        registry.register_result_request(active, request_id(1)).value() ==
        ResultRequestDisposition::Pending);
    assert(
        registry.register_result_request(active, request_id(2)).value() ==
        ResultRequestDisposition::Pending);

    assert(registry.update_state(active, GoalEvent::Succeed));
    assert(
        registry.register_result_request(active, request_id(3)).value() ==
        ResultRequestDisposition::Terminal);
    const auto pending = registry.take_pending_result_requests(active);
    assert(pending);
    assert(pending.value().size() == 2);
    assert(registry.take_pending_result_requests(active).value().empty());

    const auto expiry = registry.earliest_expiry();
    assert(expiry && expiry.value().has_value());

    const auto snapshot = registry.status_snapshot();
    assert(snapshot && snapshot.value().size() == 2);

    // Nothing expires before the retention timeout elapses.
    assert(registry.take_expired_goals().value().empty());
    assert(registry.committed_goal_count() == 2);

    std::this_thread::sleep_for(80ms);
    const auto expired = registry.take_expired_goals();
    assert(expired);
    assert(expired.value().size() == 1);
    assert(expired.value().front() == active);
    assert(registry.committed_goal_count() == 1);
    assert(!registry.state(active));
    assert(registry.state(terminal).value() == GoalState::Executing);
    assert(registry.take_pending_result_requests(terminal).value().empty());
}

void test_concurrent_accept_is_exactly_once() {
    dmw::impl::ActionGoalRegistry registry(10s);
    const auto shared = goal_id(5);
    std::atomic<int> reservations{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < 4; ++index) {
        threads.emplace_back([&registry, &reservations, shared] {
            if (registry.reserve(shared)) reservations.fetch_add(1);
        });
    }
    for (auto& thread : threads) thread.join();
    assert(reservations.load() == 1);
    assert(registry.commit(goal_info(5, 5), GoalAcceptMode::Defer));
}

void test_action_endpoint_naming() {
    const auto names = dmw::impl::derive_action_endpoint_names("/move");
    assert(names);
    assert(names->send_goal == "/move/_action/send_goal");
    assert(names->cancel_goal == "/move/_action/cancel_goal");
    assert(names->get_result == "/move/_action/get_result");
    assert(names->feedback == "/move/_action/feedback");
    assert(names->status == "/move/_action/status");

    const auto nested = dmw::impl::derive_action_endpoint_names("/robot/move");
    assert(nested);
    assert(nested->send_goal == "/robot/move/_action/send_goal");

    assert(!dmw::impl::derive_action_endpoint_names("move"));
    assert(!dmw::impl::derive_action_endpoint_names("/move/"));
    assert(!dmw::impl::derive_action_endpoint_names("/"));
    assert(!dmw::impl::derive_action_endpoint_names("/move/_action/send_goal"));

    // The derived names must round-trip through the graph classifier with the
    // roles the Action runtime expects.
    const auto send_goal = dmw::impl::classify_action_endpoint(
        dmw::impl::NormalizedEndpointName{names->send_goal, dmw::impl::GraphEndpointRole::ServiceRequest});
    assert(send_goal);
    assert(send_goal->action_name == "/move");
    assert(send_goal->role == dmw::impl::ActionEndpointRole::SendGoalRequest);

    const auto cancel_goal = dmw::impl::classify_action_endpoint(
        dmw::impl::NormalizedEndpointName{names->cancel_goal, dmw::impl::GraphEndpointRole::ServiceResponse});
    assert(cancel_goal);
    assert(cancel_goal->role == dmw::impl::ActionEndpointRole::CancelGoalResponse);

    const auto get_result = dmw::impl::classify_action_endpoint(
        dmw::impl::NormalizedEndpointName{names->get_result, dmw::impl::GraphEndpointRole::ServiceRequest});
    assert(get_result);
    assert(get_result->role == dmw::impl::ActionEndpointRole::GetResultRequest);

    const auto feedback = dmw::impl::classify_action_endpoint(
        dmw::impl::NormalizedEndpointName{names->feedback, dmw::impl::GraphEndpointRole::Topic});
    assert(feedback);
    assert(feedback->role == dmw::impl::ActionEndpointRole::Feedback);

    const auto status = dmw::impl::classify_action_endpoint(
        dmw::impl::NormalizedEndpointName{names->status, dmw::impl::GraphEndpointRole::Topic});
    assert(status);
    assert(status->role == dmw::impl::ActionEndpointRole::Status);

    // ROS 2 transport mapping of an Action endpoint stays consistent.
    const auto resolved =
        dmw::impl::normalize_endpoint_name(dmw::RuntimeMode::ROS2, "rq/move/_action/send_goalRequest");
    assert(resolved);
    assert(resolved->logical_name == "/move/_action/send_goal");
    assert(resolved->role == dmw::impl::GraphEndpointRole::ServiceRequest);
    const auto resolved_feedback =
        dmw::impl::normalize_endpoint_name(dmw::RuntimeMode::ROS2, "rt/move/_action/feedback");
    assert(resolved_feedback);
    assert(resolved_feedback->logical_name == "/move/_action/feedback");
}

}  // namespace

int main() {
    test_reservation_and_commit();
    test_state_machine();
    test_cancel_selection();
    test_result_requests_and_expiry();
    test_concurrent_accept_is_exactly_once();
    test_action_endpoint_naming();
    return 0;
}
