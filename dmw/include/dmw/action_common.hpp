#ifndef DMW_ACTION_COMMON_HPP_
#define DMW_ACTION_COMMON_HPP_

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dmw/request_id.hpp"
#include "dmw/wait_result.hpp"

namespace dmw {

/// Protocol identity of one Action goal.
struct GoalId {
    static constexpr std::size_t Size = 16;
    std::array<std::uint8_t, Size> data{};
};

inline bool operator==(const GoalId& lhs, const GoalId& rhs) noexcept {
    return lhs.data == rhs.data;
}

inline bool operator!=(const GoalId& lhs, const GoalId& rhs) noexcept { return !(lhs == rhs); }

struct GoalIdHash {
    std::size_t operator()(const GoalId& goal_id) const noexcept {
        std::size_t value = static_cast<std::size_t>(1469598103934665603ULL);
        for (const auto byte : goal_id.data) {
            value ^= static_cast<std::size_t>(byte);
            value *= static_cast<std::size_t>(1099511628211ULL);
        }
        return value;
    }
};

inline bool is_zero_goal_id(const GoalId& goal_id) noexcept {
    for (const auto byte : goal_id.data) {
        if (byte != 0) return false;
    }
    return true;
}

struct GoalInfo {
    GoalId goal_id{};
    /// Protocol time stamp used by cancel-before semantics.  It is not a
    /// result-expiry deadline.
    std::chrono::nanoseconds accepted_stamp{0};
};

enum class GoalState {
    Unknown,
    Accepted,
    Executing,
    Canceling,
    Succeeded,
    Canceled,
    Aborted
};

enum class GoalEvent { Execute, CancelGoal, Succeed, Abort, Canceled };

enum class GoalAcceptMode { Defer, Execute };

struct GoalTransition {
    GoalState previous{GoalState::Unknown};
    GoalState current{GoalState::Unknown};
    bool became_terminal{false};
};

/// Terminal states never transition again.
inline bool is_terminal_goal_state(GoalState state) noexcept {
    return state == GoalState::Succeeded || state == GoalState::Canceled ||
           state == GoalState::Aborted;
}

/// Goals that cancel selection and cancel transitions may act on.
inline bool is_cancelable_goal_state(GoalState state) noexcept {
    return state == GoalState::Accepted || state == GoalState::Executing;
}

struct GoalStatusInfo {
    GoalInfo goal_info;
    GoalState state{GoalState::Unknown};
};

struct CancelGoalCriteria {
    /// All-zero means "every cancelable goal".
    GoalId goal_id{};
    /// Zero means "no time bound".
    std::chrono::nanoseconds stamp{0};
};

/// Goals selected by one CancelGoal request, mirroring the ROS 2 response.
struct CancelSelection {
    std::vector<GoalInfo> goals;
};

enum class ResultRequestDisposition { UnknownGoal, Pending, Terminal };

/// Aggregate readiness snapshot of one ActionClient registration.
struct ActionClientReadySet {
    bool goal_response{false};
    bool cancel_response{false};
    bool result_response{false};
    bool feedback{false};
    bool status{false};

    bool any() const noexcept {
        return goal_response || cancel_response || result_response || feedback || status;
    }
};

/// Aggregate readiness snapshot of one ActionServer registration.
struct ActionServerReadySet {
    bool goal_request{false};
    bool cancel_request{false};
    bool result_request{false};
    bool goal_expired{false};

    bool any() const noexcept {
        return goal_request || cancel_request || result_request || goal_expired;
    }
};

/// Decode one WaitResult detail mask for kind == WaitableKind::ActionClient.
inline ActionClientReadySet action_client_ready_set(std::uint32_t detail_mask) noexcept {
    ActionClientReadySet ready;
    ready.goal_response = (detail_mask & kActionGoalResponseBit) != 0;
    ready.cancel_response = (detail_mask & kActionCancelResponseBit) != 0;
    ready.result_response = (detail_mask & kActionResultResponseBit) != 0;
    ready.feedback = (detail_mask & kActionFeedbackBit) != 0;
    ready.status = (detail_mask & kActionStatusBit) != 0;
    return ready;
}

/// Decode one WaitResult detail mask for kind == WaitableKind::ActionServer.
inline ActionServerReadySet action_server_ready_set(std::uint32_t detail_mask) noexcept {
    ActionServerReadySet ready;
    ready.goal_request = (detail_mask & kActionGoalRequestBit) != 0;
    ready.cancel_request = (detail_mask & kActionCancelRequestBit) != 0;
    ready.result_request = (detail_mask & kActionResultRequestBit) != 0;
    ready.goal_expired = (detail_mask & kActionGoalExpiredBit) != 0;
    return ready;
}

}  // namespace dmw

#endif  // DMW_ACTION_COMMON_HPP_
