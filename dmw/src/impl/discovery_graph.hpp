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

#include "impl/lock_rank.hpp"

namespace dmw::impl {

enum class DiscoveryChange { Added, Removed };
enum class DiscoveryHealth { Healthy, Unavailable };
enum class EndpointKind { Reader, Writer };
enum class EndpointState { Unknown, Active, Removed, Unavailable };
enum class ServiceState { Unknown, Complete, Incomplete, Unavailable };

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
    std::shared_ptr<const ParticipantRecord> participant;
    DiscoveryChange lifecycle{DiscoveryChange::Added};
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
                auto participant = participant_for(prefix);
                if (change == DiscoveryChange::Added &&
                    participant->lifecycle == DiscoveryChange::Removed)
                    return false;
                if (change == DiscoveryChange::Removed &&
                    participant->lifecycle != DiscoveryChange::Removed) {
                    if (participant->generation == UINT64_MAX) return false;
                    participant->lifecycle = change;
                    ++participant->generation;
                }
                return true;
            });
        } catch (...) {
            unavailable();
        }
    }
    void apply_endpoint(
        const eprosima::fastrtps::rtps::GUID_t& guid, EndpointKind kind, std::string topic,
        std::string type, DiscoveryChange change) noexcept {
        try {
            mutate([&] {
                auto participant = participant_for(guid.guidPrefix);
                if (change == DiscoveryChange::Added &&
                    participant->lifecycle == DiscoveryChange::Removed)
                    return false;
                auto found = std::find_if(
                    endpoints_.begin(), endpoints_.end(),
                    [&](const EndpointRecord& value) { return value.guid == guid; });
                if (found == endpoints_.end())
                    endpoints_.push_back(
                        {guid, kind, std::move(topic), std::move(type), std::move(participant),
                         change});
                else if (
                    change == DiscoveryChange::Added &&
                    found->lifecycle == DiscoveryChange::Removed)
                    return false;
                else {
                    found->kind = kind;
                    found->topic = std::move(topic);
                    found->type = std::move(type);
                    found->lifecycle = change;
                }
                return true;
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

private:
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
        {
            std::lock_guard lock(mutex_);
            if (health() != DiscoveryHealth::Healthy || !change()) {
                health_.store(DiscoveryHealth::Unavailable, std::memory_order_release);
            } else {
                if (revision_ == UINT64_MAX)
                    health_.store(DiscoveryHealth::Unavailable, std::memory_order_release);
                else
                    ++revision_;
            }
        }
        notify();
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
    std::vector<std::shared_ptr<ParticipantRecord>> participants_;
    std::vector<EndpointRecord> endpoints_;
    std::vector<std::shared_ptr<Subscriber>> subscriptions_;
};

class DiscoveryListener final : public eprosima::fastdds::dds::DomainParticipantListener {
public:
    explicit DiscoveryListener(std::weak_ptr<DiscoveryGraph> graph) noexcept
    : graph_(std::move(graph)) {}
    void close_and_drain() noexcept {
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
            info.info.typeName().to_string(),
            info.status == eprosima::fastrtps::rtps::ReaderDiscoveryInfo::REMOVED_READER);
    }
    void on_publisher_discovery(
        eprosima::fastdds::dds::DomainParticipant*,
        eprosima::fastrtps::rtps::WriterDiscoveryInfo&& info) override {
        endpoint(
            info.info.guid(), EndpointKind::Writer, info.info.topicName().to_string(),
            info.info.typeName().to_string(),
            info.status == eprosima::fastrtps::rtps::WriterDiscoveryInfo::REMOVED_WRITER);
    }

private:
    template <class F>
    void guard(F&& callback) noexcept {
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
        std::string type, bool removed) noexcept {
        guard([&](DiscoveryGraph& graph) {
            graph.apply_endpoint(
                guid, kind, std::move(topic), std::move(type),
                removed ? DiscoveryChange::Removed : DiscoveryChange::Added);
        });
    }
    std::weak_ptr<DiscoveryGraph> graph_;
    RankedMutex<LockRank::ListenerState> mutex_;
    std::condition_variable_any cv_;
    bool accepting_{true};
    std::size_t in_flight_{0};
};
}  // namespace dmw::impl
#endif
