#include "dmw/graph.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "dmw/error.hpp"
#include "dmw/graph_event.hpp"
#include "impl/context_impl.hpp"
#include "impl/graph_candidates.hpp"
#include "impl/graph_impl.hpp"
#include "impl/graph_names.hpp"
#include "impl/identity.hpp"

namespace dmw {

namespace {

using impl::EndpointCandidate;
using impl::GraphEndpointRole;
using NativeEndpointKind = impl::EndpointKind;

void fill_node_association(ServiceEndpointInfo& info, const EndpointCandidate& candidate) {
    if (!candidate.has_node) return;
    info.node_name = candidate.node_name;
    info.node_namespace = candidate.node_namespace;
}

/// Assemble one consistent public snapshot from the Context graph authority.
GraphSnapshot assemble_snapshot(impl::Context& context) {
    const auto mode = context.runtime_mode();
    const auto view = context.discovery_graph()->view();

    GraphSnapshot snapshot;
    snapshot.revision = view.revision;

    std::unordered_map<std::uint64_t, const impl::LocalNodeRecord*> nodes_by_id;
    for (const auto& node : view.local_nodes) {
        nodes_by_id.emplace(node.id, &node);
        snapshot.nodes.push_back(NodeGraphInfo{node.name, node.node_namespace});
    }
    // Node identities published by remote peers over graph metadata.
    for (const auto& participant : view.remote_nodes) {
        for (const auto& node : participant.nodes) {
            snapshot.nodes.push_back(NodeGraphInfo{node.node_name, node.node_namespace});
        }
    }

    const auto candidates = impl::collect_endpoint_candidates(mode, view);

    // Topic endpoints and per-topic aggregates.
    std::map<std::string, TopicGraphInfo> topic_aggregates;
    std::map<std::string, std::set<std::string>> topic_wire_types;
    for (const auto& candidate : candidates) {
        if (candidate.role != GraphEndpointRole::Topic) continue;
        TopicEndpointInfo info;
        info.endpoint_gid = candidate.gid;
        info.kind = impl::candidate_is_publisher(candidate) ? EndpointKind::Publisher
                                                            : EndpointKind::Subscriber;
        info.node_name = candidate.node_name;
        info.node_namespace = candidate.node_namespace;
        info.topic_name = candidate.logical_name;
        info.wire_type = candidate.wire_type;
        info.qos = candidate.qos;
        auto& aggregate = topic_aggregates[candidate.logical_name];
        aggregate.topic_name = candidate.logical_name;
        if (info.kind == EndpointKind::Publisher) {
            ++aggregate.publisher_count;
        } else {
            ++aggregate.subscriber_count;
        }
        topic_wire_types[candidate.logical_name].insert(candidate.wire_type);
        snapshot.topic_endpoints.push_back(std::move(info));
    }
    for (auto& entry : topic_aggregates) {
        const auto& types = topic_wire_types[entry.first];
        entry.second.wire_types.assign(types.begin(), types.end());
        snapshot.topics.push_back(std::move(entry.second));
    }

    // Service composition: a candidate needs a request reader plus a response
    // writer (Server) or a request writer plus a response reader (Client) that
    // live in the same participant.
    struct ServiceGroup {
        std::string name;
        std::optional<EndpointCandidate> request_reader;
        std::optional<EndpointCandidate> request_writer;
        std::optional<EndpointCandidate> response_reader;
        std::optional<EndpointCandidate> response_writer;
    };
    std::map<std::string, ServiceGroup> service_groups;
    for (const auto& candidate : candidates) {
        if (candidate.role == GraphEndpointRole::Topic) continue;
        auto& group = service_groups[candidate.participant_key + "|" + candidate.logical_name];
        group.name = candidate.logical_name;
        const bool reader = candidate.native_kind == NativeEndpointKind::Reader;
        if (candidate.role == GraphEndpointRole::ServiceRequest) {
            if (reader) {
                group.request_reader = candidate;
            } else {
                group.request_writer = candidate;
            }
        } else if (reader) {
            group.response_reader = candidate;
        } else {
            group.response_writer = candidate;
        }
    }

    std::map<std::string, ServiceGraphInfo> service_aggregates;
    std::map<std::string, std::set<std::string>> request_wire_types;
    std::map<std::string, std::set<std::string>> response_wire_types;
    for (const auto& entry : service_groups) {
        const auto& group = entry.second;
        auto& aggregate = service_aggregates[group.name];
        aggregate.service_name = group.name;
        if (group.request_reader) {
            request_wire_types[group.name].insert(group.request_reader->wire_type);
        }
        if (group.request_writer) {
            request_wire_types[group.name].insert(group.request_writer->wire_type);
        }
        if (group.response_reader) {
            response_wire_types[group.name].insert(group.response_reader->wire_type);
        }
        if (group.response_writer) {
            response_wire_types[group.name].insert(group.response_writer->wire_type);
        }
        if (group.request_reader && group.response_writer) {
            ++aggregate.server_candidate_count;
            ServiceEndpointInfo info;
            info.kind = ServiceEndpointKind::Server;
            info.service_name = group.name;
            info.request_endpoint_gid = group.request_reader->gid;
            info.response_endpoint_gid = group.response_writer->gid;
            info.request_wire_type = group.request_reader->wire_type;
            info.response_wire_type = group.response_writer->wire_type;
            fill_node_association(info, *group.request_reader);
            snapshot.service_endpoints.push_back(std::move(info));
        }
        if (group.request_writer && group.response_reader) {
            ++aggregate.client_candidate_count;
            ServiceEndpointInfo info;
            info.kind = ServiceEndpointKind::Client;
            info.service_name = group.name;
            info.request_endpoint_gid = group.request_writer->gid;
            info.response_endpoint_gid = group.response_reader->gid;
            info.request_wire_type = group.request_writer->wire_type;
            info.response_wire_type = group.response_reader->wire_type;
            fill_node_association(info, *group.request_writer);
            snapshot.service_endpoints.push_back(std::move(info));
        }
    }
    for (auto& entry : service_aggregates) {
        const auto request = request_wire_types.find(entry.first);
        if (request != request_wire_types.end()) {
            entry.second.request_wire_types.assign(request->second.begin(), request->second.end());
        }
        const auto response = response_wire_types.find(entry.first);
        if (response != response_wire_types.end()) {
            entry.second.response_wire_types.assign(
                response->second.begin(), response->second.end());
        }
        snapshot.services.push_back(std::move(entry.second));
    }

    // Action composition is shared with Action availability so both use the
    // same participant-consistent candidate semantics.
    std::map<std::string, ActionGraphInfo> action_aggregates;
    for (const auto& candidate : impl::compose_action_candidates(candidates)) {
        auto& aggregate = action_aggregates[candidate.action_name];
        aggregate.action_name = candidate.action_name;
        if (candidate.has_server) {
            ++aggregate.server_candidate_count;
            ActionEndpointInfo info;
            info.kind = ActionEndpointKind::Server;
            info.action_name = candidate.action_name;
            info.endpoint_gids = candidate.server_gids;
            info.node_name = candidate.server_node_name;
            info.node_namespace = candidate.server_node_namespace;
            snapshot.action_endpoints.push_back(std::move(info));
        }
        if (candidate.has_client) {
            ++aggregate.client_candidate_count;
            ActionEndpointInfo info;
            info.kind = ActionEndpointKind::Client;
            info.action_name = candidate.action_name;
            info.endpoint_gids = candidate.client_gids;
            info.node_name = candidate.client_node_name;
            info.node_namespace = candidate.client_node_namespace;
            snapshot.action_endpoints.push_back(std::move(info));
        }
    }
    for (auto& entry : action_aggregates) {
        snapshot.actions.push_back(std::move(entry.second));
    }

    return snapshot;
}

}  // namespace

GraphEvent::GraphEvent(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GraphEvent::~GraphEvent() noexcept = default;

Result<bool> GraphEvent::take(GraphChangeInfo& info) { return impl_->take(info); }

Result<std::unique_ptr<GraphEvent>> Context::create_graph_event() {
    return impl_->create_graph_event();
}

Result<GraphRevision> Context::graph_revision() const { return impl_->graph_revision(); }

Result<GraphSnapshot> Context::graph_snapshot() const { return impl_->graph_snapshot(); }

Result<GraphRevision> Context::Impl::graph_revision() const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<GraphRevision>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    // Remote graph metadata that arrived as historical data is folded in before
    // the revision is observed.
    if (context_->graph_metadata()) context_->graph_metadata()->ingest_pending();
    return Result<GraphRevision>::success(context_->discovery_graph()->revision());
}

Result<GraphSnapshot> Context::Impl::graph_snapshot() const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<GraphSnapshot>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    if (context_->discovery_graph()->health() != impl::DiscoveryHealth::Healthy) {
        return Result<GraphSnapshot>::failure(
            Error(ErrorCode::DDSError, "Discovery graph is unavailable"));
    }
    if (context_->graph_metadata()) context_->graph_metadata()->ingest_pending();
    return Result<GraphSnapshot>::success(assemble_snapshot(*context_));
}

Result<std::unique_ptr<GraphEvent>> Context::Impl::create_graph_event() {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<GraphEvent>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    auto event_impl = std::make_unique<GraphEvent::Impl>(context_);
    return Result<std::unique_ptr<GraphEvent>>::success(
        std::unique_ptr<GraphEvent>(new GraphEvent(std::move(event_impl))));
}

}  // namespace dmw
