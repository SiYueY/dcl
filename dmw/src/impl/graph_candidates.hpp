#ifndef DMW_IMPL__GRAPH_CANDIDATES_HPP_
#define DMW_IMPL__GRAPH_CANDIDATES_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include <fastdds/rtps/common/Guid.h>

#include "dmw/gid.hpp"
#include "dmw/qos.hpp"
#include "dmw/runtime_mode.hpp"
#include "impl/discovery_graph.hpp"
#include "impl/graph_names.hpp"

namespace dmw::impl {

/// One endpoint reduced to the graph facts shared by every aggregate view.
struct EndpointCandidate {
    Gid gid{};
    EndpointKind native_kind{EndpointKind::Reader};
    GraphEndpointRole role{GraphEndpointRole::Topic};
    std::string logical_name;
    std::string wire_type;
    Qos qos;
    std::string node_name;
    std::string node_namespace;
    bool has_node{false};
    std::string participant_key;
};

bool candidate_is_publisher(const EndpointCandidate& candidate) noexcept;

/// Opaque participant identity that keeps candidate composition inside one
/// participant, as dmw.md §5.9 requires.
std::string participant_key(const eprosima::fastrtps::rtps::GuidPrefix_t& prefix);

/// Normalize local metadata plus remote discovery into one candidate list.
///
/// Local metadata wins for local endpoints so Node association is never lost.
std::vector<EndpointCandidate> collect_endpoint_candidates(
    RuntimeMode mode, const GraphView& view);

/// One participant-consistent Action candidate composition.
struct ActionCandidate {
    std::string action_name;
    bool has_server{false};
    bool has_client{false};
    std::vector<Gid> server_gids;
    std::vector<Gid> client_gids;
    std::string server_node_name;
    std::string server_node_namespace;
    std::string client_node_name;
    std::string client_node_namespace;
    std::string participant_key;
};

/// Compose the five constituent endpoints of each Action into candidates.
///
/// The composition never crosses participants and never invents a DDS object
/// identity that the middleware cannot prove.
std::vector<ActionCandidate> compose_action_candidates(
    const std::vector<EndpointCandidate>& candidates);

/// Wire type names of the five logical Action endpoints.
struct ActionEndpointTypes {
    std::string send_goal_request;
    std::string send_goal_response;
    std::string cancel_goal_request;
    std::string cancel_goal_response;
    std::string get_result_request;
    std::string get_result_response;
    std::string feedback;
    std::string status;
};

/// True when a participant other than `local_participant` holds a complete
/// Action server composition whose wire types match `types`.
///
/// This is the only availability authority; language layers never recompute it.
bool has_remote_action_server(
    const std::vector<EndpointCandidate>& candidates, const ActionEndpointNames& names,
    const ActionEndpointTypes& types, const std::string& local_participant);

/// True when this participant holds a complete Action server composition.
bool has_local_action_server(
    const std::vector<EndpointCandidate>& candidates, const ActionEndpointNames& names,
    const ActionEndpointTypes& types, const std::string& local_participant);

}  // namespace dmw::impl

#endif  // DMW_IMPL__GRAPH_CANDIDATES_HPP_
