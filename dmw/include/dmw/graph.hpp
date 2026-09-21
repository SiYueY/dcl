#ifndef DMW_GRAPH_HPP_
#define DMW_GRAPH_HPP_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dmw/gid.hpp"
#include "dmw/qos.hpp"

namespace dmw {

/// Monotonic revision of the public-observable graph state.
using GraphRevision = std::uint64_t;

struct NodeGraphInfo {
    std::string node_name;
    std::string node_namespace;
};

enum class EndpointKind { Publisher, Subscriber };

/// One discovered topic endpoint.  `node_name`/`node_namespace` stay empty when
/// DMW cannot prove a Node association; Participant names are never guessed.
struct TopicEndpointInfo {
    Gid endpoint_gid{};
    EndpointKind kind{EndpointKind::Publisher};
    std::string node_name;
    std::string node_namespace;
    std::string topic_name;
    std::string wire_type;
    Qos qos;
};

struct TopicGraphInfo {
    std::string topic_name;
    std::vector<std::string> wire_types;
    std::size_t publisher_count{0};
    std::size_t subscriber_count{0};
};

enum class ServiceEndpointKind { Client, Server };

/// One participant-consistent Client/Server candidate composition.
struct ServiceEndpointInfo {
    ServiceEndpointKind kind{ServiceEndpointKind::Client};
    std::string node_name;
    std::string node_namespace;
    std::string service_name;
    Gid request_endpoint_gid{};
    Gid response_endpoint_gid{};
    std::string request_wire_type;
    std::string response_wire_type;
};

struct ServiceGraphInfo {
    std::string service_name;
    std::vector<std::string> request_wire_types;
    std::vector<std::string> response_wire_types;
    std::size_t client_candidate_count{0};
    std::size_t server_candidate_count{0};
};

enum class ActionEndpointKind { Client, Server };

/// Constituent endpoint identities of one composed logical Action candidate.
struct ActionEndpointInfo {
    ActionEndpointKind kind{ActionEndpointKind::Client};
    std::string node_name;
    std::string node_namespace;
    std::string action_name;
    std::vector<Gid> endpoint_gids;
};

struct ActionGraphInfo {
    std::string action_name;
    std::size_t client_candidate_count{0};
    std::size_t server_candidate_count{0};
};

/// One internally consistent snapshot of the whole graph at `revision`.
struct GraphSnapshot {
    GraphRevision revision{0};
    std::vector<NodeGraphInfo> nodes;
    std::vector<TopicGraphInfo> topics;
    std::vector<TopicEndpointInfo> topic_endpoints;
    std::vector<ServiceGraphInfo> services;
    std::vector<ServiceEndpointInfo> service_endpoints;
    std::vector<ActionGraphInfo> actions;
    std::vector<ActionEndpointInfo> action_endpoints;
};

/// Revision delta carried by one GraphEvent::take().
struct GraphChangeInfo {
    GraphRevision previous_revision{0};
    GraphRevision current_revision{0};
};

}  // namespace dmw

#endif  // DMW_GRAPH_HPP_
