#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "impl/discovery_graph.hpp"
#include "impl/graph_candidates.hpp"
#include "impl/identity.hpp"

// A DiscoveryGraph unit test (no DDS): the public GraphRevision may only move
// when the public-observable graph state actually changes (dmw.md §7.3).

namespace {

using dmw::impl::DiscoveryChange;
using dmw::impl::DiscoveryGraph;
using dmw::impl::EndpointKind;

eprosima::fastrtps::rtps::GUID_t make_guid(std::uint8_t participant, std::uint8_t entity) {
    eprosima::fastrtps::rtps::GUID_t guid;
    for (std::size_t index = 0; index < 12; ++index) guid.guidPrefix.value[index] = participant;
    guid.entityId.value[3] = entity;
    return guid;
}

std::vector<std::uint8_t> guid_bytes(const eprosima::fastrtps::rtps::GUID_t& guid) {
    std::vector<std::uint8_t> bytes;
    for (std::size_t index = 0; index < 12; ++index) bytes.push_back(guid.guidPrefix.value[index]);
    for (std::size_t index = 0; index < 4; ++index) bytes.push_back(guid.entityId.value[index]);
    return bytes;
}

}  // namespace

int main() {
    DiscoveryGraph graph;
    std::uint64_t notifications = 0;
    std::uint64_t last_revision = 0;
    auto subscription = graph.subscribe([&](std::uint64_t revision) {
        ++notifications;
        last_revision = revision;
    });

    const auto participant = make_guid(1, 0).guidPrefix;
    const auto reader = make_guid(1, 4);
    const auto writer = make_guid(1, 3);

    assert(graph.revision() == 0);
    graph.apply_participant(participant, DiscoveryChange::Added);
    assert(graph.revision() == 1);
    assert(notifications == 1 && last_revision == 1);

    // Duplicate participant Added must not move the revision.
    graph.apply_participant(participant, DiscoveryChange::Added);
    assert(graph.revision() == 1);
    assert(notifications == 1);

    graph.apply_endpoint(reader, EndpointKind::Reader, "rq", "RequestType",
                         DiscoveryChange::Added);
    assert(graph.revision() == 2);
    // Duplicate endpoint Added with identical data must not move the revision.
    graph.apply_endpoint(reader, EndpointKind::Reader, "rq", "RequestType",
                         DiscoveryChange::Added);
    assert(graph.revision() == 2);
    assert(notifications == 2);

    // A real change (topic/type) does move it.
    graph.apply_endpoint(reader, EndpointKind::Reader, "rq2", "RequestType",
                         DiscoveryChange::Added);
    assert(graph.revision() == 3);

    graph.apply_endpoint(writer, EndpointKind::Writer, "rr", "ResponseType",
                         DiscoveryChange::Added);
    assert(graph.revision() == 4);
    graph.apply_endpoint(writer, EndpointKind::Writer, "rr", "ResponseType",
                         DiscoveryChange::Removed);
    assert(graph.revision() == 5);
    // Removed again: state already reflects removal.
    graph.apply_endpoint(writer, EndpointKind::Writer, "rr", "ResponseType",
                         DiscoveryChange::Removed);
    assert(graph.revision() == 5);

    // Remote node metadata: identical snapshot is a no-op, a real change is not.
    std::vector<dmw::impl::RemoteNodeRecord> nodes;
    dmw::impl::RemoteNodeRecord node;
    node.node_name = "remote_node";
    node.node_namespace = "/";
    std::array<std::uint8_t, 16> wire_gid{};
    const auto reader_bytes = guid_bytes(reader);
    for (std::size_t index = 0; index < reader_bytes.size(); ++index) {
        wire_gid[index] = reader_bytes[index];
    }
    node.reader_gids.push_back(wire_gid);
    nodes.push_back(node);
    graph.apply_remote_nodes(participant, nodes);
    const auto after_metadata = graph.revision();
    assert(after_metadata == 6);
    graph.apply_remote_nodes(participant, nodes);
    assert(graph.revision() == after_metadata);
    assert(notifications == 6);

    node.node_name = "renamed_node";
    nodes.clear();
    nodes.push_back(node);
    graph.apply_remote_nodes(participant, nodes);
    assert(graph.revision() == after_metadata + 1);

    // Removing the participant is a real change; repeating it is not.
    graph.apply_participant(participant, DiscoveryChange::Removed);
    const auto after_removal = graph.revision();
    graph.apply_participant(participant, DiscoveryChange::Removed);
    assert(graph.revision() == after_removal);

    // An emptied metadata snapshot removes the association exactly once.
    graph.apply_remote_nodes(participant, {});
    const auto after_metadata_removal = graph.revision();
    assert(after_metadata_removal == after_removal + 1);
    graph.apply_remote_nodes(participant, {});
    assert(graph.revision() == after_metadata_removal);

    // ROS 2 metadata makes Node identity part of Action composition.  A
    // participant may host several Nodes, so endpoint halves from different
    // Nodes must never become one public Action Server candidate.
    std::vector<dmw::impl::EndpointCandidate> action_candidates;
    const auto add_action_endpoint = [&](const char* logical_name,
                                         dmw::impl::GraphEndpointRole role,
                                         EndpointKind kind, const char* node_name,
                                         std::uint8_t entity) {
        dmw::impl::EndpointCandidate endpoint;
        endpoint.gid = dmw::impl::to_gid(make_guid(7, entity));
        endpoint.native_kind = kind;
        endpoint.role = role;
        endpoint.logical_name = logical_name;
        endpoint.participant_key = "one-participant";
        endpoint.node_name = node_name;
        endpoint.node_namespace = "/";
        endpoint.has_node = true;
        action_candidates.push_back(std::move(endpoint));
    };
    add_action_endpoint("/split/_action/send_goal", dmw::impl::GraphEndpointRole::ServiceRequest,
                        EndpointKind::Reader, "node_a", 1);
    add_action_endpoint("/split/_action/cancel_goal", dmw::impl::GraphEndpointRole::ServiceRequest,
                        EndpointKind::Reader, "node_b", 2);
    add_action_endpoint("/split/_action/get_result", dmw::impl::GraphEndpointRole::ServiceRequest,
                        EndpointKind::Reader, "node_b", 3);
    add_action_endpoint("/split/_action/feedback", dmw::impl::GraphEndpointRole::Topic,
                        EndpointKind::Writer, "node_b", 4);
    add_action_endpoint("/split/_action/status", dmw::impl::GraphEndpointRole::Topic,
                        EndpointKind::Writer, "node_b", 5);
    const auto split_actions =
        dmw::impl::compose_action_candidates(dmw::RuntimeMode::ROS2, action_candidates);
    assert(std::none_of(split_actions.begin(), split_actions.end(), [](const auto& candidate) {
        return candidate.has_server;
    }));

    action_candidates.front().node_name = "node_b";
    const auto complete_actions =
        dmw::impl::compose_action_candidates(dmw::RuntimeMode::ROS2, action_candidates);
    assert(std::any_of(complete_actions.begin(), complete_actions.end(), [](const auto& candidate) {
        return candidate.has_server;
    }));

    subscription.close_and_drain();
    return 0;
}
