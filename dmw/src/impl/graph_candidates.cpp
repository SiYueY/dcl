#include "impl/graph_candidates.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "impl/identity.hpp"

namespace dmw::impl {

namespace {

struct ActionRoleKey {
    ActionEndpointRole role{ActionEndpointRole::Feedback};
    EndpointKind kind{EndpointKind::Writer};

    bool operator<(const ActionRoleKey& other) const noexcept {
        return std::tie(role, kind) < std::tie(other.role, other.kind);
    }
};

struct ActionGroup {
    std::string action_name;
    std::map<ActionRoleKey, EndpointCandidate> endpoints;

    const EndpointCandidate* find(ActionEndpointRole role, EndpointKind kind) const {
        const auto found = endpoints.find(ActionRoleKey{role, kind});
        return found == endpoints.end() ? nullptr : &found->second;
    }
};

}  // namespace

bool candidate_is_publisher(const EndpointCandidate& candidate) noexcept {
    return candidate.native_kind == EndpointKind::Writer;
}

std::string participant_key(const eprosima::fastrtps::rtps::GuidPrefix_t& prefix) {
    return std::string(reinterpret_cast<const char*>(prefix.value), sizeof(prefix.value));
}

std::vector<EndpointCandidate> collect_endpoint_candidates(
    RuntimeMode mode, const GraphView& view) {
    std::unordered_map<std::uint64_t, const LocalNodeRecord*> nodes_by_id;
    for (const auto& node : view.local_nodes) nodes_by_id.emplace(node.id, &node);

    // Node association published by remote peers, keyed by endpoint GUID.
    std::map<std::array<std::uint8_t, kDdsGuidSize>, const RemoteNodeRecord*> remote_by_gid;
    for (const auto& participant : view.remote_nodes) {
        for (const auto& node : participant.nodes) {
            for (const auto& gid : node.reader_gids) {
                remote_by_gid.emplace(gid, &node);
            }
            for (const auto& gid : node.writer_gids) {
                remote_by_gid.emplace(gid, &node);
            }
        }
    }

    std::vector<EndpointCandidate> candidates;
    std::unordered_set<Gid, GidHash> local_gids;

    for (const auto& local : view.local_endpoints) {
        if (local.topic == kGraphMetadataTopicName) continue;  // internal transport
        const auto normalized = normalize_endpoint_name(mode, local.topic);
        if (!normalized) continue;
        EndpointCandidate candidate;
        candidate.gid = to_gid(local.guid);
        candidate.native_kind = local.kind;
        candidate.role = normalized->role;
        candidate.logical_name = normalized->logical_name;
        candidate.wire_type = local.type;
        candidate.qos = local.qos;
        candidate.participant_key = participant_key(local.guid.guidPrefix);
        if (const auto node = nodes_by_id.find(local.node_id); node != nodes_by_id.end()) {
            candidate.node_name = node->second->name;
            candidate.node_namespace = node->second->node_namespace;
            candidate.has_node = true;
        }
        local_gids.insert(candidate.gid);
        candidates.push_back(std::move(candidate));
    }

    for (const auto& remote : view.endpoints) {
        if (remote.lifecycle == DiscoveryChange::Removed) continue;
        if (remote.participant && remote.participant->lifecycle == DiscoveryChange::Removed) {
            continue;
        }
        // The graph metadata transport is an internal DMW/ROS 2 channel and is
        // never part of the user-visible graph.
        if (remote.topic == kGraphMetadataTopicName) continue;
        const auto gid = to_gid(remote.guid);
        if (local_gids.count(gid) != 0) continue;  // local metadata wins
        const auto normalized = normalize_endpoint_name(mode, remote.topic);
        if (!normalized) continue;
        EndpointCandidate candidate;
        candidate.gid = gid;
        candidate.native_kind = remote.kind;
        candidate.role = normalized->role;
        candidate.logical_name = normalized->logical_name;
        candidate.wire_type = remote.type;
        candidate.qos = remote.qos;
        candidate.participant_key = participant_key(remote.guid.guidPrefix);
        const auto association = remote_by_gid.find(candidate.gid.data);
        if (association != remote_by_gid.end()) {
            candidate.node_name = association->second->node_name;
            candidate.node_namespace = association->second->node_namespace;
            candidate.has_node = true;
        }
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

std::vector<ActionCandidate> compose_action_candidates(
    const std::vector<EndpointCandidate>& candidates) {
    // Action composition: all five logical endpoints must live in the same
    // participant before DMW reports an Action candidate.
    std::map<std::string, ActionGroup> groups;
    for (const auto& candidate : candidates) {
        const NormalizedEndpointName normalized{candidate.logical_name, candidate.role};
        const auto classified = classify_action_endpoint(normalized);
        if (!classified) continue;
        auto& group = groups[candidate.participant_key + "|" + classified->action_name];
        group.action_name = classified->action_name;
        group.endpoints.insert(
            {ActionRoleKey{classified->role, candidate.native_kind}, candidate});
    }

    std::vector<ActionCandidate> result;
    for (const auto& entry : groups) {
        const ActionGroup& group = entry.second;
        ActionCandidate candidate;
        candidate.action_name = group.action_name;
        candidate.participant_key = entry.first.substr(0, entry.first.find('|'));

        const auto* send_goal_reader =
            group.find(ActionEndpointRole::SendGoalRequest, EndpointKind::Reader);
        const auto* cancel_goal_reader =
            group.find(ActionEndpointRole::CancelGoalRequest, EndpointKind::Reader);
        const auto* get_result_reader =
            group.find(ActionEndpointRole::GetResultRequest, EndpointKind::Reader);
        const auto* feedback_writer =
            group.find(ActionEndpointRole::Feedback, EndpointKind::Writer);
        const auto* status_writer = group.find(ActionEndpointRole::Status, EndpointKind::Writer);
        if (send_goal_reader && cancel_goal_reader && get_result_reader && feedback_writer &&
            status_writer) {
            candidate.has_server = true;
            candidate.server_gids = {
                send_goal_reader->gid, cancel_goal_reader->gid, get_result_reader->gid,
                feedback_writer->gid, status_writer->gid};
            if (send_goal_reader->has_node) {
                candidate.server_node_name = send_goal_reader->node_name;
                candidate.server_node_namespace = send_goal_reader->node_namespace;
            }
        }

        const auto* send_goal_writer =
            group.find(ActionEndpointRole::SendGoalRequest, EndpointKind::Writer);
        const auto* cancel_goal_writer =
            group.find(ActionEndpointRole::CancelGoalRequest, EndpointKind::Writer);
        const auto* get_result_writer =
            group.find(ActionEndpointRole::GetResultRequest, EndpointKind::Writer);
        const auto* feedback_reader =
            group.find(ActionEndpointRole::Feedback, EndpointKind::Reader);
        const auto* status_reader = group.find(ActionEndpointRole::Status, EndpointKind::Reader);
        if (send_goal_writer && cancel_goal_writer && get_result_writer && feedback_reader &&
            status_reader) {
            candidate.has_client = true;
            candidate.client_gids = {
                send_goal_writer->gid, cancel_goal_writer->gid, get_result_writer->gid,
                feedback_reader->gid, status_reader->gid};
            if (send_goal_writer->has_node) {
                candidate.client_node_name = send_goal_writer->node_name;
                candidate.client_node_namespace = send_goal_writer->node_namespace;
            }
        }
        result.push_back(std::move(candidate));
    }
    return result;
}

namespace {

bool has_endpoint(
    const std::vector<EndpointCandidate>& candidates, const std::string& participant,
    const std::string& logical_name, GraphEndpointRole role, EndpointKind kind,
    const std::string& wire_type) {
    for (const auto& candidate : candidates) {
        if (candidate.participant_key != participant) continue;
        if (candidate.native_kind != kind || candidate.role != role) continue;
        if (candidate.logical_name != logical_name) continue;
        if (candidate.wire_type != wire_type) continue;
        return true;
    }
    return false;
}

bool action_server_composition_complete(
    const std::vector<EndpointCandidate>& candidates, const std::string& participant,
    const ActionEndpointNames& names, const ActionEndpointTypes& types) {
    return has_endpoint(
               candidates, participant, names.send_goal, GraphEndpointRole::ServiceRequest,
               EndpointKind::Reader, types.send_goal_request) &&
           has_endpoint(
               candidates, participant, names.send_goal, GraphEndpointRole::ServiceResponse,
               EndpointKind::Writer, types.send_goal_response) &&
           has_endpoint(
               candidates, participant, names.cancel_goal, GraphEndpointRole::ServiceRequest,
               EndpointKind::Reader, types.cancel_goal_request) &&
           has_endpoint(
               candidates, participant, names.cancel_goal, GraphEndpointRole::ServiceResponse,
               EndpointKind::Writer, types.cancel_goal_response) &&
           has_endpoint(
               candidates, participant, names.get_result, GraphEndpointRole::ServiceRequest,
               EndpointKind::Reader, types.get_result_request) &&
           has_endpoint(
               candidates, participant, names.get_result, GraphEndpointRole::ServiceResponse,
               EndpointKind::Writer, types.get_result_response) &&
           has_endpoint(
               candidates, participant, names.feedback, GraphEndpointRole::Topic,
               EndpointKind::Writer, types.feedback) &&
           has_endpoint(
               candidates, participant, names.status, GraphEndpointRole::Topic,
               EndpointKind::Writer, types.status);
}

}  // namespace

bool has_remote_action_server(
    const std::vector<EndpointCandidate>& candidates, const ActionEndpointNames& names,
    const ActionEndpointTypes& types, const std::string& local_participant) {
    std::set<std::string> participants;
    for (const auto& candidate : candidates) participants.insert(candidate.participant_key);
    for (const auto& participant : participants) {
        if (participant == local_participant) continue;
        if (action_server_composition_complete(candidates, participant, names, types)) return true;
    }
    return false;
}

bool has_local_action_server(
    const std::vector<EndpointCandidate>& candidates, const ActionEndpointNames& names,
    const ActionEndpointTypes& types, const std::string& local_participant) {
    return action_server_composition_complete(candidates, local_participant, names, types);
}

}  // namespace dmw::impl
