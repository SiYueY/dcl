#include "impl/graph_metadata.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>
#include <fastcdr/config.h>
#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/topic/TopicDataType.hpp>
#include <fastdds/rtps/common/InstanceHandle.h>

#include "dmw/fastdds/message_type.hpp"
#include "impl/context.hpp"
#include "impl/discovery_graph.hpp"
#include "impl/identity.hpp"
#include "impl/return_code.hpp"

namespace dmw::impl {

namespace {

/// Set while one thread folds a remote snapshot into the DiscoveryGraph.
thread_local bool g_remote_update_in_progress = false;

}  // namespace

bool graph_metadata_remote_update_in_progress() noexcept {
    return g_remote_update_in_progress;
}

namespace {

using Cdr = eprosima::fastcdr::Cdr;

constexpr std::uint32_t kMetadataPayloadSize = 65536U;
constexpr std::uint32_t kGraphMetadataWriterDepth = 1U;

#if FASTCDR_VERSION_MAJOR >= 2
constexpr auto kCdrVersion = eprosima::fastcdr::DDS_CDR;
#else
constexpr auto kCdrVersion = eprosima::fastcdr::Cdr::DDS_CDR;
#endif

void write_string(Cdr& cdr, const std::string& value) { cdr << value; }

void read_string(Cdr& cdr, std::string& value) { cdr >> value; }

void write_gid(Cdr& cdr, const RmwGid& gid) {
    for (const auto byte : gid.data) cdr << byte;
}

void read_gid(Cdr& cdr, RmwGid& gid) {
    for (auto& byte : gid.data) cdr >> byte;
}

void write_gid_sequence(Cdr& cdr, const std::vector<RmwGid>& gids) {
    cdr << static_cast<std::uint32_t>(gids.size());
    for (const auto& gid : gids) write_gid(cdr, gid);
}

void read_gid_sequence(Cdr& cdr, std::vector<RmwGid>& gids) {
    std::uint32_t size = 0;
    cdr >> size;
    gids.clear();
    gids.resize(size);
    for (auto& gid : gids) read_gid(cdr, gid);
}

void write_participant_entities(Cdr& cdr, const RmwParticipantEntitiesInfo& value) {
    write_gid(cdr, value.gid);
    cdr << static_cast<std::uint32_t>(value.node_entities_info_seq.size());
    for (const auto& node : value.node_entities_info_seq) {
        write_string(cdr, node.node_namespace);
        write_string(cdr, node.node_name);
        write_gid_sequence(cdr, node.reader_gids);
        write_gid_sequence(cdr, node.writer_gids);
    }
}

void read_participant_entities(Cdr& cdr, RmwParticipantEntitiesInfo& value) {
    read_gid(cdr, value.gid);
    std::uint32_t size = 0;
    cdr >> size;
    value.node_entities_info_seq.clear();
    value.node_entities_info_seq.resize(size);
    for (auto& node : value.node_entities_info_seq) {
        read_string(cdr, node.node_namespace);
        read_string(cdr, node.node_name);
        read_gid_sequence(cdr, node.reader_gids);
        read_gid_sequence(cdr, node.writer_gids);
    }
}

/// Wire-compatible `rmw_dds_common::msg::ParticipantEntitiesInfo` binding.
class ParticipantEntitiesInfoType final : public eprosima::fastdds::dds::TopicDataType {
public:
    ParticipantEntitiesInfoType() {
        m_typeSize = kMetadataPayloadSize;
        m_isGetKeyDefined = false;
        setName(kGraphMetadataTypeName);
    }

    bool serialize(void* data, eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
        eprosima::fastcdr::FastBuffer buffer(
            reinterpret_cast<char*>(payload->data), payload->max_size);
        Cdr cdr(buffer, Cdr::DEFAULT_ENDIAN, kCdrVersion);
        cdr.serialize_encapsulation();
        write_participant_entities(cdr, *static_cast<RmwParticipantEntitiesInfo*>(data));
#if FASTCDR_VERSION_MAJOR >= 2
        payload->length = static_cast<std::uint32_t>(cdr.get_serialized_data_length());
#else
        payload->length = static_cast<std::uint32_t>(cdr.getSerializedDataLength());
#endif
        return true;
    }

    bool deserialize(
        eprosima::fastrtps::rtps::SerializedPayload_t* payload, void* data) override {
        eprosima::fastcdr::FastBuffer buffer(
            reinterpret_cast<char*>(payload->data), static_cast<std::size_t>(payload->length));
        Cdr cdr(buffer, Cdr::DEFAULT_ENDIAN, kCdrVersion);
        cdr.read_encapsulation();
        read_participant_entities(cdr, *static_cast<RmwParticipantEntitiesInfo*>(data));
        return true;
    }

    std::function<std::uint32_t()> getSerializedSizeProvider(void*) override {
        return [] { return kMetadataPayloadSize; };
    }

    void* createData() override { return new RmwParticipantEntitiesInfo(); }

    void deleteData(void* data) override { delete static_cast<RmwParticipantEntitiesInfo*>(data); }

    bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override {
        return false;
    }
};

}  // namespace

/// Reader listener that folds one remote participant snapshot into the graph.
class GraphMetadataListener final : public eprosima::fastdds::dds::DataReaderListener {
public:
    GraphMetadataListener(
        std::weak_ptr<DiscoveryGraph> graph, eprosima::fastrtps::rtps::GuidPrefix_t local_prefix,
        std::function<void()> on_data) noexcept
    : graph_(std::move(graph)), local_prefix_(local_prefix), on_data_(std::move(on_data)) {}

    void activate() noexcept { active_.store(true, std::memory_order_release); }

    void close_and_drain() noexcept {
        active_.store(false, std::memory_order_release);
        std::unique_lock lock(mutex_);
        accepting_ = false;
        cv_.wait(lock, [this] { return in_flight_ == 0; });
    }

    void on_data_available(eprosima::fastdds::dds::DataReader* reader) override {
        (void)reader;
        if (!active_.load(std::memory_order_acquire)) return;
        if (on_data_) on_data_();
    }

    /// A TransientLocal peer snapshot is delivered as historical data when the
    /// writer matches, which does not raise DATA_AVAILABLE, so ingestion also
    /// runs on match.
    void on_subscription_matched(
        eprosima::fastdds::dds::DataReader* reader,
        const eprosima::fastdds::dds::SubscriptionMatchedStatus& status) override {
        if (status.current_count_change <= 0) return;
        if (!active_.load(std::memory_order_acquire)) return;
        (void)reader;
        if (on_data_) on_data_();
    }

    /// Drain the reader into the graph.  Serialized by the transport mutex.
    void ingest(eprosima::fastdds::dds::DataReader* reader) {
        const auto graph = graph_.lock();
        if (!graph) return;
        // One pass consumes at most the samples that are already unread, so a
        // sample that is reported but not consumed (for example a not-valid
        // instance sample on newer Fast DDS) can never spin this loop forever.
        // This mirrors the finite candidate budget of Subscriber::read.
        auto remaining = reader->get_unread_count();
        while (remaining-- != 0U) {
            RmwParticipantEntitiesInfo sample;
            eprosima::fastdds::dds::SampleInfo info;
            const auto taken = reader->take_next_sample(&sample, &info);
            if (taken == eprosima::fastrtps::types::ReturnCode_t::RETCODE_NO_DATA) return;
            if (taken != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) return;
            if (!info.valid_data) continue;
            // Ignore our own publication: DMW never feeds its own snapshot back
            // into the graph as a remote update.
            const auto guid = rmw_gid_to_guid(sample.gid);
            if (std::equal(
                    guid.begin(), guid.begin() + 12, local_prefix_.value)) {
                continue;
            }
            eprosima::fastrtps::rtps::GuidPrefix_t prefix{};
            std::copy_n(guid.begin(), 12, prefix.value);
            std::vector<RemoteNodeRecord> nodes;
            nodes.reserve(sample.node_entities_info_seq.size());
            for (const auto& node : sample.node_entities_info_seq) {
                RemoteNodeRecord record;
                record.node_namespace = node.node_namespace;
                record.node_name = node.node_name;
                record.reader_gids.reserve(node.reader_gids.size());
                for (const auto& gid : node.reader_gids) {
                    record.reader_gids.push_back(rmw_gid_to_guid(gid));
                }
                record.writer_gids.reserve(node.writer_gids.size());
                for (const auto& gid : node.writer_gids) {
                    record.writer_gids.push_back(rmw_gid_to_guid(gid));
                }
                nodes.push_back(std::move(record));
            }
            g_remote_update_in_progress = true;
            graph->apply_remote_nodes(prefix, std::move(nodes));
            g_remote_update_in_progress = false;
        }
    }

private:
    std::weak_ptr<DiscoveryGraph> graph_;
    eprosima::fastrtps::rtps::GuidPrefix_t local_prefix_;
    std::function<void()> on_data_;
    std::atomic<bool> active_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    bool accepting_{true};
    std::size_t in_flight_{0};
};

namespace {

/// Build the complete ParticipantEntitiesInfo snapshot for this participant.
RmwParticipantEntitiesInfo build_local_snapshot(
    const GraphView& view, const eprosima::fastrtps::rtps::GUID_t& participant_guid) {
    RmwParticipantEntitiesInfo info;
    const auto participant = to_gid(participant_guid);
    for (std::size_t index = 0; index < kDdsGuidSize && index < participant.data.size(); ++index) {
        info.gid.data[index] = participant.data[index];
    }
    info.node_entities_info_seq.reserve(view.local_nodes.size());
    for (const auto& node : view.local_nodes) {
        RmwNodeEntitiesInfo record;
        record.node_namespace = node.node_namespace;
        record.node_name = node.name;
        for (const auto& endpoint : view.local_endpoints) {
            if (endpoint.node_id != node.id) continue;
            if (endpoint.topic == kGraphMetadataTopicName) continue;
            const auto gid = to_gid(endpoint.guid);
            RmwGid wire_gid;
            for (std::size_t index = 0; index < kDdsGuidSize && index < gid.data.size(); ++index) {
                wire_gid.data[index] = gid.data[index];
            }
            if (endpoint.kind == EndpointKind::Reader) {
                record.reader_gids.push_back(wire_gid);
            } else {
                record.writer_gids.push_back(wire_gid);
            }
        }
        info.node_entities_info_seq.push_back(std::move(record));
    }
    return info;
}

}  // namespace

Result<std::shared_ptr<GraphMetadataTransport>> GraphMetadataTransport::create(
    Context& context, const std::shared_ptr<DiscoveryGraph>& graph) {
    eprosima::fastdds::dds::TypeSupport support(new ParticipantEntitiesInfoType());
    auto metadata_type = dmw::fastdds::MessageTypeAdapter::create(
        std::move(support), std::type_index(typeid(ParticipantEntitiesInfoType)));
    if (!metadata_type) {
        return Result<std::shared_ptr<GraphMetadataTransport>>::failure(
            std::move(metadata_type.error()));
    }

    // Writer: KeepLast(1) / Reliable / TransientLocal.  Reader: KeepAll /
    // Reliable / TransientLocal, so a late joiner still receives the complete
    // current state of every participant.
    Qos writer_qos;
    auto keep_last = writer_qos.keep_last(kGraphMetadataWriterDepth);
    if (!keep_last) {
        return Result<std::shared_ptr<GraphMetadataTransport>>::failure(
            std::move(keep_last.error()));
    }
    writer_qos.reliable().transient_local();
    Qos reader_qos;
    reader_qos.keep_all().reliable().transient_local();

    auto transport = std::shared_ptr<GraphMetadataTransport>(new GraphMetadataTransport());
    transport->graph_ = graph;

    auto topic = context.acquire_topic(
        metadata_type.value(), kGraphMetadataTopicName, writer_qos);
    if (!topic) {
        return Result<std::shared_ptr<GraphMetadataTransport>>::failure(
            std::move(topic.error()));
    }
    transport->topic_ = std::move(topic.value());

    const auto participant_guid = eprosima::fastrtps::rtps::iHandle2GUID(
        context.participant()->get_instance_handle());
    const std::weak_ptr<GraphMetadataTransport> weak_transport = transport;
    transport->listener_ = std::make_unique<GraphMetadataListener>(
        graph, participant_guid.guidPrefix, [weak_transport] {
            if (const auto transport = weak_transport.lock()) transport->ingest_from_listener();
        });
    transport->listener_->activate();

    auto device_writer_qos = to_writer_qos(
        writer_qos, context.runtime_mode(), context.writer_qos_baseline());
    if (!device_writer_qos) {
        return Result<std::shared_ptr<GraphMetadataTransport>>::failure(
            std::move(device_writer_qos.error()));
    }
    auto device_reader_qos = to_reader_qos(
        reader_qos, context.runtime_mode(), context.reader_qos_baseline());
    if (!device_reader_qos) {
        return Result<std::shared_ptr<GraphMetadataTransport>>::failure(
            std::move(device_reader_qos.error()));
    }
    transport->local_participant_guid_ = participant_guid;

    auto* writer = context.publisher()->create_datawriter(
        transport->topic_.get(), device_writer_qos.value());
    if (writer == nullptr) {
        return Result<std::shared_ptr<GraphMetadataTransport>>::failure(
            Error(ErrorCode::DDSError, "Fast DDS failed to create graph metadata writer"));
    }
    transport->writer_ = writer;
    auto* reader =
        context.subscriber()->create_datareader(transport->topic_.get(), device_reader_qos.value());
    if (reader == nullptr) {
        return Result<std::shared_ptr<GraphMetadataTransport>>::failure(
            Error(ErrorCode::DDSError, "Fast DDS failed to create graph metadata reader"));
    }
    transport->reader_ = reader;
    const auto attached = reader->set_listener(
        transport->listener_.get(), eprosima::fastdds::dds::StatusMask::all());
    if (attached != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
        return Result<std::shared_ptr<GraphMetadataTransport>>::failure(
            Error(ErrorCode::DDSError, "Fast DDS rejected the graph metadata listener"));
    }
    return Result<std::shared_ptr<GraphMetadataTransport>>::success(std::move(transport));
}

GraphMetadataTransport::~GraphMetadataTransport() noexcept {
    if (listener_) {
        bool detached = false;
        try {
            if (reader_ != nullptr) {
                detached = reader_->set_listener(nullptr) ==
                           eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
            }
        } catch (...) {
            detached = false;
        }
        if (detached) listener_->close_and_drain();
    }
    listener_.reset();
}

void GraphMetadataTransport::publish_local_snapshot() noexcept {
    if (writer_ == nullptr || graph_ == nullptr) return;
    std::lock_guard lock(publish_mutex_);
    try {
        const auto view = graph_->view();
        auto snapshot = build_local_snapshot(view, local_participant_guid_);
        if (writer_->write(&snapshot) != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
            degraded_.store(true, std::memory_order_release);
        }
    } catch (...) {
        // A metadata publish failure never rolls back committed local state.
        degraded_.store(true, std::memory_order_release);
    }
}

void GraphMetadataTransport::ingest_pending() noexcept {
    if (reader_ == nullptr || !listener_) return;
    // Drain unconditionally: the listener flag is only a wake hint, and an
    // edge-triggered notification can be consumed before the sample lands.
    data_pending_.store(false, std::memory_order_release);
    std::lock_guard lock(ingest_mutex_);
    try {
        listener_->ingest(reader_);
    } catch (...) {
        degraded_.store(true, std::memory_order_release);
    }
}

void GraphMetadataTransport::ingest_from_listener() noexcept {
    // The Fast DDS callback thread only records that work is pending.  Taking
    // samples from inside the listener can block (the reader is being
    // notified), so draining happens on application threads instead.
    data_pending_.store(true, std::memory_order_release);
}

}  // namespace dmw::impl
