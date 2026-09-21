#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "impl/discovery_graph.hpp"

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

    subscription.close_and_drain();
    return 0;
}
