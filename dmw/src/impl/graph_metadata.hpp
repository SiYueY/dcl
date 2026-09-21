#ifndef DMW_IMPL__GRAPH_METADATA_HPP_
#define DMW_IMPL__GRAPH_METADATA_HPP_

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/rtps/common/Guid.h>

#include "dmw/result.hpp"
#include "impl/topic.hpp"

namespace dmw::impl {

class Context;
class DiscoveryGraph;
class GraphMetadataListener;

/// DDS topic and type of the ROS 2 graph metadata transport.
///
/// DMW implements the wire contract itself so it never links a ROS 2 runtime
/// package; the names are frozen because interoperability depends on them.
inline constexpr const char* kGraphMetadataTopicName = "ros_discovery_info";
inline constexpr const char* kGraphMetadataTypeName =
    "rmw_dds_common::msg::dds_::ParticipantEntitiesInfo_";

/// `rmw_gid_t` as carried by rmw_dds_common.
///
/// The wire layout changed between ROS 2 generations: Humble uses `char[24]`
/// while Rolling/Jazzy use `char[16]`.  DMW cannot link a ROS 2 runtime package
/// to detect the peer, so the size is selected at build time from the Fast DDS
/// generation that ships with the target ROS 2 distribution (see
/// `DMW_RMW_GID_SIZE` in CMakeLists.txt).  The leading 16 octets always hold the
/// DDS GUID for a Fast DDS implementation, so GUID extraction is layout-neutral.
#ifndef DMW_RMW_GID_SIZE
#define DMW_RMW_GID_SIZE 16
#endif
inline constexpr std::size_t kRmwGidSize = DMW_RMW_GID_SIZE;
inline constexpr std::size_t kDdsGuidSize = 16;

struct RmwGid {
    std::array<std::uint8_t, kRmwGidSize> data{};
};

struct RmwNodeEntitiesInfo {
    std::string node_namespace;
    std::string node_name;
    std::vector<RmwGid> reader_gids;
    std::vector<RmwGid> writer_gids;
};

struct RmwParticipantEntitiesInfo {
    RmwGid gid;
    std::vector<RmwNodeEntitiesInfo> node_entities_info_seq;
};

/// Extract the leading DDS GUID bytes of one rmw Gid.
inline std::array<std::uint8_t, kDdsGuidSize> rmw_gid_to_guid(const RmwGid& gid) noexcept {
    std::array<std::uint8_t, kDdsGuidSize> guid{};
    for (std::size_t index = 0; index < kDdsGuidSize; ++index) guid[index] = gid.data[index];
    return guid;
}

/// True while the calling thread is folding remote metadata into the graph.
///
/// A remote snapshot never changes this participant's own Node/endpoint set, so
/// the transport must not republish because of it; this also keeps the metadata
/// writer out of the Fast DDS listener thread.
bool graph_metadata_remote_update_in_progress() noexcept;

/// Internal ROS 2 graph metadata transport.
///
/// Publishes the complete local participant snapshot on `ros_discovery_info`
/// and folds remote snapshots into the DiscoveryGraph.  The endpoints are
/// private: they are never exposed as user publishers/subscribers and never
/// appear in the public graph view.
class GraphMetadataTransport {
public:
    /// Create the transport, or return the Error of the failing step.
    static Result<std::shared_ptr<GraphMetadataTransport>> create(
        Context& context, const std::shared_ptr<DiscoveryGraph>& graph);

    ~GraphMetadataTransport() noexcept;

    GraphMetadataTransport(const GraphMetadataTransport&) = delete;
    GraphMetadataTransport& operator=(const GraphMetadataTransport&) = delete;

    /// Publish the complete local Node/endpoint snapshot.
    ///
    /// Failures mark the transport degraded and never roll back the local
    /// communication entities that were already committed.
    void publish_local_snapshot() noexcept;

    /// Fold any pending remote metadata into the graph.
    ///
    /// Fast DDS delivers a TransientLocal peer snapshot as historical data at
    /// match time, which does not reliably raise DATA_AVAILABLE, so graph
    /// observers drain the reader before reading the graph.  Safe to call from
    /// any thread.
    void ingest_pending() noexcept;

    bool degraded() const noexcept { return degraded_.load(std::memory_order_acquire); }

    /// Drain the reader from the Fast DDS listener thread.
    ///
    /// Never blocks: if an application thread is already draining, this returns
    /// immediately because that pass will consume the newly arrived samples.
    void ingest_from_listener() noexcept;

private:
    GraphMetadataTransport() = default;

    std::shared_ptr<DiscoveryGraph> graph_;
    Topic topic_;
    eprosima::fastdds::dds::DataWriter* writer_{nullptr};
    eprosima::fastdds::dds::DataReader* reader_{nullptr};
    std::unique_ptr<GraphMetadataListener> listener_;
    eprosima::fastrtps::rtps::GUID_t local_participant_guid_{};
    std::atomic<bool> degraded_{false};
    std::mutex publish_mutex_;
    /// Separate from publish_mutex_: ingesting a remote snapshot can trigger
    /// the graph subscription, which republishes the local snapshot.
    std::mutex ingest_mutex_;
    std::atomic<bool> data_pending_{false};
};

}  // namespace dmw::impl

#endif  // DMW_IMPL__GRAPH_METADATA_HPP_
