#ifndef DMW_IMPL__DISCOVERY_GRAPH_HPP_
#define DMW_IMPL__DISCOVERY_GRAPH_HPP_

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fastdds/dds/domain/DomainParticipantListener.hpp>

#include "dmw/qos.hpp"
#include "impl/qos.hpp"
#include "impl/lock_rank.hpp"
#include "impl/graph_metadata.hpp"

namespace dmw::impl {

enum class DiscoveryChange { Added, Removed };
enum class DiscoveryHealth { Healthy, Unavailable };
enum class EndpointKind { Reader, Writer };
enum class EndpointState { Unknown, Active, Removed, Unavailable };
enum class ServiceState { Unknown, Complete, Incomplete, Unavailable };

enum class GraphMutation { Changed, Unchanged, Invalid };

struct ParticipantRecord {
    eprosima::fastrtps::rtps::GuidPrefix_t prefix;
    DiscoveryChange lifecycle{DiscoveryChange::Added};
    std::uint64_t generation{0};
};

struct EndpointRecord {
    eprosima::fastrtps::rtps::GUID_t guid;
    EndpointKind kind;
    std::string topic;
    std::string type;
    Qos qos;
    std::shared_ptr<const ParticipantRecord> participant;
    DiscoveryChange lifecycle{DiscoveryChange::Added};
};

/// Local Node identity owned by one Context.
///
/// `references` counts the owning Node facade plus every local endpoint that
/// still carries this Node association, so endpoints outliving their Node
/// facade keep the name/namespace visible in the graph.
struct LocalNodeRecord {
    std::uint64_t id{0};
    std::string name;
    std::string node_namespace;
    std::size_t references{0};
};

struct LocalEndpointRecord {
    eprosima::fastrtps::rtps::GUID_t guid;
    EndpointKind kind{EndpointKind::Writer};
    std::string topic;
    std::string type;
    Qos qos;
    std::uint64_t node_id{0};
};

/// Node identity learned from another participant's graph metadata.
struct RemoteNodeRecord {
    std::string node_namespace;
    std::string node_name;
    std::vector<std::array<std::uint8_t, 16>> reader_gids;
    std::vector<std::array<std::uint8_t, 16>> writer_gids;
};

struct RemoteParticipantNodes {
    eprosima::fastrtps::rtps::GuidPrefix_t prefix{};
    std::vector<RemoteNodeRecord> nodes;
};

/// One internally consistent copy of the whole discovered graph state.
struct GraphView {
    std::uint64_t revision{0};
    std::vector<ParticipantRecord> participants;
    std::vector<EndpointRecord> endpoints;
    std::vector<LocalNodeRecord> local_nodes;
    std::vector<LocalEndpointRecord> local_endpoints;
    std::vector<RemoteParticipantNodes> remote_nodes;
};

class DiscoveryGraph : public std::enable_shared_from_this<DiscoveryGraph> {
public:
    class Subscription {
    public:
        Subscription() = default;
        ~Subscription() { reset(); }
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept
        : graph_(std::move(other.graph_)), id_(std::exchange(other.id_, 0)) {}

        /// False for a default-constructed subscription.
        explicit operator bool() const noexcept { return id_ != 0; }
        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                reset();
                graph_ = std::move(other.graph_);
                id_ = std::exchange(other.id_, 0);
            }
            return *this;
        }
        void close_and_drain() noexcept { reset(true); }

    private:
        friend class DiscoveryGraph;
        Subscription(std::weak_ptr<DiscoveryGraph> graph, std::uint64_t id)
        : graph_(std::move(graph)), id_(id) {}
        void reset(bool drain = false) noexcept {
            if (id_ != 0) {
                if (auto graph = graph_.lock()) graph->unsubscribe(id_, drain);
                id_ = 0;
            }
        }
        std::weak_ptr<DiscoveryGraph> graph_;
        std::uint64_t id_{0};
    };

    void apply_participant(
        const eprosima::fastrtps::rtps::GuidPrefix_t& prefix, DiscoveryChange change) noexcept {
        try {
            mutate([&] {
                auto found = std::find_if(
                    participants_.begin(), participants_.end(),
                    [&](const auto& value) { return value->prefix == prefix; });
                if (found == participants_.end()) {
                    auto participant = std::make_shared<ParticipantRecord>();
                    participant->prefix = prefix;
                    participant->lifecycle = change;
                    participants_.push_back(std::move(participant));
                    return GraphMutation::Changed;
                }
                const auto& participant = *found;
                if (change == DiscoveryChange::Added &&
                    participant->lifecycle == DiscoveryChange::Removed)
                    return GraphMutation::Invalid;
                if (change == DiscoveryChange::Removed &&
                    participant->lifecycle != DiscoveryChange::Removed) {
                    if (participant->generation == UINT64_MAX) return GraphMutation::Invalid;
                    participant->lifecycle = change;
                    ++participant->generation;
                    return GraphMutation::Changed;
                }
                return GraphMutation::Unchanged;
            });
        } catch (...) {
            unavailable();
        }
    }
    void apply_endpoint(
        const eprosima::fastrtps::rtps::GUID_t& guid, EndpointKind kind, std::string topic,
        std::string type, DiscoveryChange change, Qos qos = {}) noexcept {
        try {
            mutate([&] {
                auto participant = participant_for(guid.guidPrefix);
                if (change == DiscoveryChange::Added &&
                    participant->lifecycle == DiscoveryChange::Removed)
                    return GraphMutation::Invalid;
                auto found = std::find_if(
                    endpoints_.begin(), endpoints_.end(),
                    [&](const EndpointRecord& value) { return value.guid == guid; });
                if (found == endpoints_.end()) {
                    endpoints_.push_back(
                        {guid, kind, std::move(topic), std::move(type), std::move(qos),
                         std::move(participant), change});
                    return GraphMutation::Changed;
                }
                if (change == DiscoveryChange::Added &&
                    found->lifecycle == DiscoveryChange::Removed)
                    return GraphMutation::Invalid;
                if (found->kind == kind && found->topic == topic && found->type == type &&
                    found->lifecycle == change)
                    return GraphMutation::Unchanged;
                found->kind = kind;
                found->topic = std::move(topic);
                found->type = std::move(type);
                found->qos = std::move(qos);
                found->lifecycle = change;
                return GraphMutation::Changed;
            });
        } catch (...) {
            unavailable();
        }
    }
    DiscoveryHealth health() const noexcept { return health_.load(std::memory_order_acquire); }
    std::uint64_t revision() const noexcept {
        std::lock_guard lock(mutex_);
        return revision_;
    }
    std::optional<ParticipantRecord> participant(
        const eprosima::fastrtps::rtps::GuidPrefix_t& prefix) const {
        std::lock_guard lock(mutex_);
        const auto found = std::find_if(
            participants_.begin(), participants_.end(),
            [&](const auto& value) { return value->prefix == prefix; });
        return found == participants_.end() ? std::nullopt
                                            : std::optional<ParticipantRecord>{**found};
    }
    std::optional<EndpointRecord> endpoint_snapshot(
        const eprosima::fastrtps::rtps::GUID_t& guid) const {
        std::lock_guard lock(mutex_);
        const auto found = std::find_if(
            endpoints_.begin(), endpoints_.end(),
            [&](const EndpointRecord& value) { return value.guid == guid; });
        return found == endpoints_.end() ? std::nullopt : std::optional<EndpointRecord>{*found};
    }
    EndpointState endpoint(const eprosima::fastrtps::rtps::GUID_t& guid) const noexcept {
        if (health() != DiscoveryHealth::Healthy) return EndpointState::Unavailable;
        std::lock_guard lock(mutex_);
        auto found = std::find_if(
            endpoints_.begin(), endpoints_.end(),
            [&](const EndpointRecord& value) { return value.guid == guid; });
        return found == endpoints_.end() ? EndpointState::Unknown
               : found->lifecycle == DiscoveryChange::Removed ||
                       (found->participant &&
                        found->participant->lifecycle == DiscoveryChange::Removed)
                   ? EndpointState::Removed
                   : EndpointState::Active;
    }
    ServiceState service(
        const eprosima::fastrtps::rtps::GuidPrefix_t& prefix, const std::string& request_topic,
        const std::string& request_type, const std::string& response_topic,
        const std::string& response_type) const noexcept {
        if (health() != DiscoveryHealth::Healthy) return ServiceState::Unavailable;
        std::lock_guard lock(mutex_);
        bool relevant = false, request_reader = false, response_writer = false;
        for (const auto& value : endpoints_) {
            if (value.guid.guidPrefix != prefix || value.lifecycle == DiscoveryChange::Removed ||
                !value.participant || value.participant->lifecycle == DiscoveryChange::Removed)
                continue;
            if (value.topic == request_topic && value.type == request_type) {
                relevant = true;
                request_reader = request_reader || value.kind == EndpointKind::Reader;
            }
            if (value.topic == response_topic && value.type == response_type) {
                relevant = true;
                response_writer = response_writer || value.kind == EndpointKind::Writer;
            }
        }
        return request_reader && response_writer ? ServiceState::Complete
               : relevant                        ? ServiceState::Incomplete
                                                 : ServiceState::Unknown;
    }
    Subscription subscribe(std::function<void(std::uint64_t)> callback) {
        std::lock_guard lock(mutex_);
        if (next_subscription_ == UINT64_MAX) {
            health_.store(DiscoveryHealth::Unavailable, std::memory_order_release);
            return {};
        }
        const auto id = ++next_subscription_;
        subscriptions_.push_back(std::make_shared<Subscriber>(id, std::move(callback)));
        return Subscription(weak_from_this(), id);
    }
    void unavailable() noexcept {
        health_.store(DiscoveryHealth::Unavailable, std::memory_order_release);
        notify();
    }

    /// Register one local Node identity and return its stable id, or 0 when the
    /// graph cannot admit more local state.
    std::uint64_t add_local_node(std::string name, std::string node_namespace) noexcept {
        std::uint64_t id = 0;
        try {
            mutate([&] {
                if (next_local_node_id_ == UINT64_MAX) return GraphMutation::Invalid;
                const auto candidate = ++next_local_node_id_;
                local_nodes_.push_back({candidate, std::move(name), std::move(node_namespace), 1});
                id = candidate;
                return GraphMutation::Changed;
            });
        } catch (...) {
            unavailable();
            return 0;
        }
        return id;
    }

    /// Drop the Node facade's reference; the record survives while endpoints
    /// still reference it.
    void release_local_node(std::uint64_t id) noexcept {
        if (id == 0) return;
        try {
            mutate([&] {
                const auto found = std::find_if(
                    local_nodes_.begin(), local_nodes_.end(),
                    [&](const LocalNodeRecord& value) { return value.id == id; });
                if (found == local_nodes_.end()) return GraphMutation::Unchanged;
                if (found->references > 0) --found->references;
                if (found->references == 0) {
                    local_nodes_.erase(found);
                    return GraphMutation::Changed;
                }
                return GraphMutation::Unchanged;
            });
        } catch (...) {
            unavailable();
        }
    }

    /// Associate one local endpoint with the Node that created it.
    void add_local_endpoint(
        const eprosima::fastrtps::rtps::GUID_t& guid, EndpointKind kind, std::string topic,
        std::string type, std::uint64_t node_id, Qos qos = {}) noexcept {
        if (node_id == 0) return;
        try {
            mutate([&] {
                const auto node = std::find_if(
                    local_nodes_.begin(), local_nodes_.end(),
                    [&](const LocalNodeRecord& value) { return value.id == node_id; });
                if (node == local_nodes_.end()) return GraphMutation::Invalid;
                const auto found = std::find_if(
                    local_endpoints_.begin(), local_endpoints_.end(),
                    [&](const LocalEndpointRecord& value) { return value.guid == guid; });
                if (found != local_endpoints_.end()) {
                    if (found->kind == kind && found->topic == topic && found->type == type &&
                        found->node_id == node_id) {
                        return GraphMutation::Unchanged;
                    }
                    found->kind = kind;
                    found->topic = std::move(topic);
                    found->type = std::move(type);
                    found->qos = std::move(qos);
                    found->node_id = node_id;
                    return GraphMutation::Changed;
                }
                ++node->references;
                local_endpoints_.push_back(
                    {guid, kind, std::move(topic), std::move(type), std::move(qos), node_id});
                return GraphMutation::Changed;
            });
        } catch (...) {
            unavailable();
        }
    }

    void remove_local_endpoint(const eprosima::fastrtps::rtps::GUID_t& guid) noexcept {
        try {
            mutate([&] {
                const auto found = std::find_if(
                    local_endpoints_.begin(), local_endpoints_.end(),
                    [&](const LocalEndpointRecord& value) { return value.guid == guid; });
                if (found == local_endpoints_.end()) return GraphMutation::Unchanged;
                const auto node_id = found->node_id;
                local_endpoints_.erase(found);
                const auto node = std::find_if(
                    local_nodes_.begin(), local_nodes_.end(),
                    [&](const LocalNodeRecord& value) { return value.id == node_id; });
                if (node != local_nodes_.end() && node->references > 0) --node->references;
                if (node != local_nodes_.end() && node->references == 0) {
                    local_nodes_.erase(node);
                }
                return GraphMutation::Changed;
            });
        } catch (...) {
            unavailable();
        }
    }

    /// Copy one consistent view of remote discovery state plus local metadata.
    /// Replace one participant's Node/endpoint association snapshot.
    ///
    /// The whole participant snapshot is replaced so a stale association can
    /// never survive; the revision moves only when the public-observable graph
    /// state actually changes.
    void apply_remote_nodes(
        const eprosima::fastrtps::rtps::GuidPrefix_t& prefix,
        std::vector<RemoteNodeRecord> nodes) noexcept {
        try {
            mutate([&] {
                const auto found = std::find_if(
                    remote_nodes_.begin(), remote_nodes_.end(),
                    [&](const RemoteParticipantNodes& value) { return value.prefix == prefix; });
                if (found == remote_nodes_.end()) {
                    if (nodes.empty()) return GraphMutation::Unchanged;
                    remote_nodes_.push_back(RemoteParticipantNodes{prefix, std::move(nodes)});
                    return GraphMutation::Changed;
                }
                if (remote_nodes_equal(found->nodes, nodes)) return GraphMutation::Unchanged;
                if (nodes.empty()) {
                    remote_nodes_.erase(found);
                    return GraphMutation::Changed;
                }
                found->nodes = std::move(nodes);
                return GraphMutation::Changed;
            });
        } catch (...) {
            unavailable();
        }
    }

    /// Copy one consistent view of remote discovery state plus local metadata.
    GraphView view() const {
        GraphView result;
        std::lock_guard lock(mutex_);
        result.revision = revision_;
        result.participants.reserve(participants_.size());
        for (const auto& participant : participants_) result.participants.push_back(*participant);
        result.endpoints = endpoints_;
        result.local_nodes = local_nodes_;
        result.local_endpoints = local_endpoints_;
        result.remote_nodes = remote_nodes_;
        return result;
    }

private:
    static bool remote_nodes_equal(
        const std::vector<RemoteNodeRecord>& lhs,
        const std::vector<RemoteNodeRecord>& rhs) noexcept {
        if (lhs.size() != rhs.size()) return false;
        for (std::size_t index = 0; index < lhs.size(); ++index) {
            if (lhs[index].node_namespace != rhs[index].node_namespace ||
                lhs[index].node_name != rhs[index].node_name ||
                lhs[index].reader_gids != rhs[index].reader_gids ||
                lhs[index].writer_gids != rhs[index].writer_gids) {
                return false;
            }
        }
        return true;
    }

    struct Subscriber {
        Subscriber(std::uint64_t value, std::function<void(std::uint64_t)> fn)
        : id(value), callback(std::move(fn)) {}
        std::uint64_t id;
        std::function<void(std::uint64_t)> callback;
        std::mutex mutex;
        std::condition_variable cv;
        bool accepting{true};
        std::size_t in_flight{0};
    };
    std::shared_ptr<ParticipantRecord> participant_for(
        const eprosima::fastrtps::rtps::GuidPrefix_t& prefix) {
        auto found = std::find_if(
            participants_.begin(), participants_.end(),
            [&](const auto& value) { return value->prefix == prefix; });
        if (found != participants_.end()) return *found;
        auto value = std::make_shared<ParticipantRecord>();
        value->prefix = prefix;
        participants_.push_back(value);
        return value;
    }
    template <class F>
    void mutate(F&& change) {
        bool should_notify = false;
        {
            std::lock_guard lock(mutex_);
            if (health() != DiscoveryHealth::Healthy) {
                return;
            } else if (const auto mutation = change(); mutation == GraphMutation::Invalid) {
                health_.store(DiscoveryHealth::Unavailable, std::memory_order_release);
                should_notify = true;
            } else if (mutation == GraphMutation::Changed) {
                if (revision_ == UINT64_MAX)
                    health_.store(DiscoveryHealth::Unavailable, std::memory_order_release);
                else
                    ++revision_;
                should_notify = true;
            }
        }
        if (should_notify) notify();
    }
    void unsubscribe(std::uint64_t id, bool drain) noexcept {
        std::shared_ptr<Subscriber> subscriber;
        {
            std::lock_guard lock(mutex_);
            const auto found = std::find_if(
                subscriptions_.begin(), subscriptions_.end(),
                [&](const auto& value) { return value->id == id; });
            if (found == subscriptions_.end()) return;
            subscriber = *found;
            subscriptions_.erase(found);
        }
        std::unique_lock lock(subscriber->mutex);
        subscriber->accepting = false;
        if (drain) subscriber->cv.wait(lock, [&] { return subscriber->in_flight == 0; });
    }
    void notify() noexcept {
        std::vector<std::shared_ptr<Subscriber>> subscribers;
        std::uint64_t revision = 0;
        try {
            {
                std::lock_guard lock(mutex_);
                subscribers = subscriptions_;
                revision = revision_;
            }
            for (const auto& subscriber : subscribers) {
                {
                    std::lock_guard lock(subscriber->mutex);
                    if (!subscriber->accepting) continue;
                    ++subscriber->in_flight;
                }
                try {
                    subscriber->callback(revision);
                } catch (...) {
                    // Do not recursively notify here: a permanently throwing
                    // subscriber must not turn the noexcept failure path into
                    // unbounded recursion.
                    health_.store(DiscoveryHealth::Unavailable, std::memory_order_release);
                }
                {
                    std::lock_guard lock(subscriber->mutex);
                    if (--subscriber->in_flight == 0) subscriber->cv.notify_all();
                }
            }
        } catch (...) {
            health_.store(DiscoveryHealth::Unavailable, std::memory_order_release);
        }
    }
    mutable RankedMutex<LockRank::DiscoveryGraph> mutex_;
    std::atomic<DiscoveryHealth> health_{DiscoveryHealth::Healthy};
    std::uint64_t revision_{0}, next_subscription_{0};
    std::uint64_t next_local_node_id_{0};
    std::vector<LocalNodeRecord> local_nodes_;
    std::vector<LocalEndpointRecord> local_endpoints_;
    std::vector<RemoteParticipantNodes> remote_nodes_;
    std::vector<std::shared_ptr<ParticipantRecord>> participants_;
    std::vector<EndpointRecord> endpoints_;
    std::vector<std::shared_ptr<Subscriber>> subscriptions_;
};

class DiscoveryListener final : public eprosima::fastdds::dds::DomainParticipantListener {
public:
    explicit DiscoveryListener(std::weak_ptr<DiscoveryGraph> graph) noexcept
    : graph_(std::move(graph)) {}
    // Publish construction before giving this pointer to Fast DDS, whose
    // listener callback thread is outside DMW's mutex/condition-variable
    // synchronization domain.
    void activate() noexcept { active_.store(true, std::memory_order_release); }
    void close_and_drain() noexcept {
        active_.store(false, std::memory_order_release);
        std::unique_lock lock(mutex_);
        accepting_ = false;
        cv_.wait(lock, [&] { return in_flight_ == 0; });
    }
    void on_participant_discovery(
        eprosima::fastdds::dds::DomainParticipant*,
        eprosima::fastrtps::rtps::ParticipantDiscoveryInfo&& info) override {
        guard([&](DiscoveryGraph& graph) {
            graph.apply_participant(
                info.info.m_guid.guidPrefix,
                (info.status ==
                     eprosima::fastrtps::rtps::ParticipantDiscoveryInfo::REMOVED_PARTICIPANT ||
                 info.status ==
                     eprosima::fastrtps::rtps::ParticipantDiscoveryInfo::DROPPED_PARTICIPANT)
                    ? DiscoveryChange::Removed
                    : DiscoveryChange::Added);
        });
    }
    void on_subscriber_discovery(
        eprosima::fastdds::dds::DomainParticipant*,
        eprosima::fastrtps::rtps::ReaderDiscoveryInfo&& info) override {
        endpoint(
            info.info.guid(), EndpointKind::Reader, info.info.topicName().to_string(),
            info.info.typeName().to_string(), discovery_qos_or_unknown(info.info.m_qos),
            info.status == eprosima::fastrtps::rtps::ReaderDiscoveryInfo::REMOVED_READER);
    }
    void on_publisher_discovery(
        eprosima::fastdds::dds::DomainParticipant*,
        eprosima::fastrtps::rtps::WriterDiscoveryInfo&& info) override {
        endpoint(
            info.info.guid(), EndpointKind::Writer, info.info.topicName().to_string(),
            info.info.typeName().to_string(), discovery_qos_or_unknown(info.info.m_qos),
            info.status == eprosima::fastrtps::rtps::WriterDiscoveryInfo::REMOVED_WRITER);
    }

private:
    template <class F>
    void guard(F&& callback) noexcept {
        if (!active_.load(std::memory_order_acquire)) return;
        {
            std::lock_guard lock(mutex_);
            if (!accepting_) return;
            ++in_flight_;
        }
        if (auto graph = graph_.lock()) callback(*graph);
        {
            std::lock_guard lock(mutex_);
            if (--in_flight_ == 0) cv_.notify_all();
        }
    }
    void endpoint(
        const eprosima::fastrtps::rtps::GUID_t& guid, EndpointKind kind, std::string topic,
        std::string type, Qos qos, bool removed) noexcept {
        guard([&](DiscoveryGraph& graph) {
            graph.apply_endpoint(
                guid, kind, std::move(topic), std::move(type),
                removed ? DiscoveryChange::Removed : DiscoveryChange::Added, std::move(qos));
        });
    }
    std::weak_ptr<DiscoveryGraph> graph_;
    std::atomic<bool> active_{false};
    RankedMutex<LockRank::ListenerState> mutex_;
    std::condition_variable_any cv_;
    bool accepting_{true};
    std::size_t in_flight_{0};
};
/// Move-only RAII handle that removes one local endpoint association when the
/// owning public endpoint is destroyed.
class LocalEndpointRegistration {
public:
    LocalEndpointRegistration() noexcept = default;

    LocalEndpointRegistration(
        std::shared_ptr<DiscoveryGraph> graph, eprosima::fastrtps::rtps::GUID_t guid) noexcept
    : graph_(std::move(graph)), guid_(guid), armed_(graph_ != nullptr) {}

    ~LocalEndpointRegistration() noexcept { reset(); }

    LocalEndpointRegistration(const LocalEndpointRegistration&) = delete;
    LocalEndpointRegistration& operator=(const LocalEndpointRegistration&) = delete;

    LocalEndpointRegistration(LocalEndpointRegistration&& other) noexcept
    : graph_(std::move(other.graph_)),
      guid_(other.guid_),
      armed_(std::exchange(other.armed_, false)) {}

    LocalEndpointRegistration& operator=(LocalEndpointRegistration&& other) noexcept {
        if (this != &other) {
            reset();
            graph_ = std::move(other.graph_);
            guid_ = other.guid_;
            armed_ = std::exchange(other.armed_, false);
        }
        return *this;
    }

    void reset() noexcept {
        if (armed_ && graph_) graph_->remove_local_endpoint(guid_);
        armed_ = false;
        graph_.reset();
    }

private:
    std::shared_ptr<DiscoveryGraph> graph_;
    eprosima::fastrtps::rtps::GUID_t guid_{};
    bool armed_{false};
};

/// Publish one local endpoint association and return its teardown handle.
inline LocalEndpointRegistration register_local_endpoint(
    const std::shared_ptr<DiscoveryGraph>& graph,
    const eprosima::fastrtps::rtps::GUID_t& guid, EndpointKind kind, std::string topic,
    std::string type, Qos qos, std::uint64_t node_id) {
    graph->add_local_endpoint(
        guid, kind, std::move(topic), std::move(type), node_id, std::move(qos));
    return LocalEndpointRegistration(graph, guid);
}

}  // namespace dmw::impl

#endif  // DMW_IMPL__DISCOVERY_GRAPH_HPP_
