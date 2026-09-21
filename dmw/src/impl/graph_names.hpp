#ifndef DMW_IMPL__GRAPH_NAMES_HPP_
#define DMW_IMPL__GRAPH_NAMES_HPP_

#include <optional>
#include <string>
#include <string_view>

#include "dmw/runtime_mode.hpp"

namespace dmw::impl {

/// Which DMW logical protocol one DDS endpoint belongs to.
enum class GraphEndpointRole { Topic, ServiceRequest, ServiceResponse };

struct NormalizedEndpointName {
    std::string logical_name;
    GraphEndpointRole role{GraphEndpointRole::Topic};
};

enum class ActionEndpointRole {
    SendGoalRequest,
    SendGoalResponse,
    CancelGoalRequest,
    CancelGoalResponse,
    GetResultRequest,
    GetResultResponse,
    Feedback,
    Status
};

struct ActionEndpointClassification {
    std::string action_name;
    ActionEndpointRole role{ActionEndpointRole::Feedback};
};

/// The five fixed logical endpoint names derived from one Action FQN.
struct ActionEndpointNames {
    std::string send_goal;
    std::string cancel_goal;
    std::string get_result;
    std::string feedback;
    std::string status;
};

inline bool name_starts_with(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

inline bool name_ends_with(std::string_view value, std::string_view suffix) noexcept {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// Map a resolved DDS transport name back to its normalized logical name.
///
/// Returns nullopt when the DDS name does not match the current RuntimeMode
/// mapping, in which case the endpoint stays out of the public graph view.
inline std::optional<NormalizedEndpointName> normalize_endpoint_name(
    RuntimeMode mode, std::string_view dds_name) {
    NormalizedEndpointName result;
    if (mode == RuntimeMode::ROS2) {
        if (name_starts_with(dds_name, "rt/")) {
            result.logical_name = "/" + std::string(dds_name.substr(3));
            result.role = GraphEndpointRole::Topic;
        } else if (name_starts_with(dds_name, "rq/") && name_ends_with(dds_name, "Request")) {
            result.logical_name =
                "/" + std::string(dds_name.substr(3, dds_name.size() - 3 - 7));
            result.role = GraphEndpointRole::ServiceRequest;
        } else if (name_starts_with(dds_name, "rr/") && name_ends_with(dds_name, "Reply")) {
            result.logical_name =
                "/" + std::string(dds_name.substr(3, dds_name.size() - 3 - 5));
            result.role = GraphEndpointRole::ServiceResponse;
        } else {
            return std::nullopt;
        }
    } else if (name_ends_with(dds_name, "_Request")) {
        result.logical_name = "/" + std::string(dds_name.substr(0, dds_name.size() - 8));
        result.role = GraphEndpointRole::ServiceRequest;
    } else if (name_ends_with(dds_name, "_Reply")) {
        result.logical_name = "/" + std::string(dds_name.substr(0, dds_name.size() - 6));
        result.role = GraphEndpointRole::ServiceResponse;
    } else {
        result.logical_name = "/" + std::string(dds_name);
        result.role = GraphEndpointRole::Topic;
    }
    if (result.logical_name.size() <= 1) return std::nullopt;
    return result;
}

/// Derive the five fixed logical endpoint names of one Action FQN.
inline std::optional<ActionEndpointNames> derive_action_endpoint_names(std::string_view action_fqn) {
    if (action_fqn.size() <= 1 || action_fqn.front() != '/' || action_fqn.back() == '/') {
        return std::nullopt;
    }
    if (action_fqn.find("/_action") != std::string_view::npos) return std::nullopt;
    ActionEndpointNames names;
    const std::string prefix = std::string(action_fqn) + "/_action/";
    names.send_goal = prefix + "send_goal";
    names.cancel_goal = prefix + "cancel_goal";
    names.get_result = prefix + "get_result";
    names.feedback = prefix + "feedback";
    names.status = prefix + "status";
    return names;
}

/// Recognize the fixed ROS 2 Action logical endpoint suffixes.
inline std::optional<ActionEndpointClassification> classify_action_endpoint(
    const NormalizedEndpointName& endpoint) {
    constexpr std::string_view kMarker = "/_action/";
    const auto position = endpoint.logical_name.rfind(kMarker);
    if (position == std::string::npos || position == 0) return std::nullopt;
    const std::string_view suffix =
        std::string_view(endpoint.logical_name).substr(position + kMarker.size());
    if (suffix.empty() || suffix.find('/') != std::string_view::npos) return std::nullopt;

    ActionEndpointClassification result;
    result.action_name = endpoint.logical_name.substr(0, position);
    if (endpoint.role == GraphEndpointRole::Topic) {
        if (suffix == "feedback") {
            result.role = ActionEndpointRole::Feedback;
        } else if (suffix == "status") {
            result.role = ActionEndpointRole::Status;
        } else {
            return std::nullopt;
        }
        return result;
    }

    const bool request = endpoint.role == GraphEndpointRole::ServiceRequest;
    if (suffix == "send_goal") {
        result.role = request ? ActionEndpointRole::SendGoalRequest
                              : ActionEndpointRole::SendGoalResponse;
    } else if (suffix == "cancel_goal") {
        result.role = request ? ActionEndpointRole::CancelGoalRequest
                              : ActionEndpointRole::CancelGoalResponse;
    } else if (suffix == "get_result") {
        result.role = request ? ActionEndpointRole::GetResultRequest
                              : ActionEndpointRole::GetResultResponse;
    } else {
        return std::nullopt;
    }
    return result;
}

}  // namespace dmw::impl

#endif  // DMW_IMPL__GRAPH_NAMES_HPP_
