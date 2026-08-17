#ifndef DMW_IMPL__PARTICIPANT_OBSERVATION_HPP_
#define DMW_IMPL__PARTICIPANT_OBSERVATION_HPP_

#include <atomic>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fastdds/dds/domain/DomainParticipantListener.hpp>

#include "impl/lock_rank.hpp"

namespace dmw::impl {

enum class ParticipantLifecycle { Active, Removed };
enum class DiscoveryCapability { Healthy, Degraded };

struct ParticipantObservationEntry {
    eprosima::fastrtps::rtps::GuidPrefix_t prefix;
    std::atomic<ParticipantLifecycle> lifecycle{ParticipantLifecycle::Active};
    std::atomic<std::uint64_t> generation{0};
};

enum class RemoteEndpointKind { Reader, Writer };
enum class RemoteEndpointLifecycle { Active, Removed };
enum class RemoteEndpointObservationState { Unknown, Active, Removed, Degraded };
enum class ServicePairObservation { Unknown, Complete, Incomplete, Degraded };

struct RemoteEndpointObservation {
    eprosima::fastrtps::rtps::GUID_t guid;
    RemoteEndpointKind kind;
    std::string topic_name;
    std::string type_name;
    std::shared_ptr<ParticipantObservationEntry> participant;
    RemoteEndpointLifecycle lifecycle{RemoteEndpointLifecycle::Active};
};

/// Exact response-reader lifecycle authority.  Unlike the general endpoint
/// graph, this registry is queried by Server::send_response and keeps reader
/// tombstones for the complete Context lifetime.
class TargetReaderObservationRegistry {
public:
    struct Snapshot {
        RemoteEndpointObservationState state{RemoteEndpointObservationState::Unknown};
        std::shared_ptr<ParticipantObservationEntry> participant;
    };
    void observe(
        const eprosima::fastrtps::rtps::GUID_t& guid,
        std::shared_ptr<ParticipantObservationEntry> participant, bool removed) {
        std::lock_guard lock(mutex_);
        if (capability_.load(std::memory_order_acquire) != DiscoveryCapability::Healthy) return;
        for (auto& entry : entries_) {
            if (entry.guid != guid) continue;
            if (removed)
                entry.lifecycle = RemoteEndpointLifecycle::Removed;
            else if (entry.lifecycle == RemoteEndpointLifecycle::Removed) {
                capability_.store(DiscoveryCapability::Degraded, std::memory_order_release);
            }
            return;
        }
        entries_.push_back(RemoteEndpointObservation{
            guid,
            RemoteEndpointKind::Reader,
            {},
            {},
            std::move(participant),
            removed ? RemoteEndpointLifecycle::Removed : RemoteEndpointLifecycle::Active});
    }

    void observe_noexcept(
        const eprosima::fastrtps::rtps::GUID_t& guid,
        std::shared_ptr<ParticipantObservationEntry> participant, bool removed) noexcept {
        try {
            observe(guid, std::move(participant), removed);
        } catch (...) {
            capability_.store(DiscoveryCapability::Degraded, std::memory_order_release);
        }
        notify_dependents();
    }

    Snapshot snapshot(const eprosima::fastrtps::rtps::GUID_t& guid) const noexcept {
        if (capability_.load(std::memory_order_acquire) != DiscoveryCapability::Healthy) {
            return Snapshot{RemoteEndpointObservationState::Degraded, {}};
        }
        std::lock_guard lock(mutex_);
        for (const auto& entry : entries_) {
            if (entry.guid == guid) {
                return Snapshot{
                    entry.lifecycle == RemoteEndpointLifecycle::Removed
                        ? RemoteEndpointObservationState::Removed
                        : RemoteEndpointObservationState::Active,
                    entry.participant};
            }
        }
        return Snapshot{};
    }

    RemoteEndpointObservationState lookup(
        const eprosima::fastrtps::rtps::GUID_t& guid) const noexcept {
        return snapshot(guid).state;
    }

    DiscoveryCapability capability() const noexcept {
        return capability_.load(std::memory_order_acquire);
    }

    void add_dependency_wake(std::function<void()> wake) {
        std::lock_guard lock(mutex_);
        dependency_wakes_.push_back(std::move(wake));
    }

private:
    void notify_dependents() noexcept {
        std::vector<std::function<void()>> wakes;
        try {
            std::lock_guard lock(mutex_);
            wakes = dependency_wakes_;
        } catch (...) {
            capability_.store(DiscoveryCapability::Degraded, std::memory_order_release);
            return;
        }
        for (const auto& wake : wakes) {
            try {
                wake();
            } catch (...) {
                capability_.store(DiscoveryCapability::Degraded, std::memory_order_release);
            }
        }
    }

    mutable RankedMutex<LockRank::TargetReader> mutex_;
    std::atomic<DiscoveryCapability> capability_{DiscoveryCapability::Healthy};
    std::vector<RemoteEndpointObservation> entries_;
    std::vector<std::function<void()>> dependency_wakes_;
};

class RemoteEndpointRegistry {
public:
    void observe(
        const eprosima::fastrtps::rtps::GUID_t& guid, RemoteEndpointKind kind, std::string topic,
        std::string type, std::shared_ptr<ParticipantObservationEntry> participant, bool removed) {
        std::lock_guard lock(mutex_);
        if (capability_.load(std::memory_order_acquire) != DiscoveryCapability::Healthy) return;
        for (auto& entry : entries_) {
            if (entry.guid == guid) {
                if (removed)
                    entry.lifecycle = RemoteEndpointLifecycle::Removed;
                else if (entry.lifecycle == RemoteEndpointLifecycle::Removed) {
                    capability_.store(DiscoveryCapability::Degraded, std::memory_order_release);
                }
                return;
            }
        }
        entries_.push_back(RemoteEndpointObservation{
            guid, kind, std::move(topic), std::move(type), std::move(participant),
            removed ? RemoteEndpointLifecycle::Removed : RemoteEndpointLifecycle::Active});
    }
    void observe_noexcept(
        const eprosima::fastrtps::rtps::GUID_t& guid, RemoteEndpointKind kind,
        const std::string& topic, const std::string& type,
        std::shared_ptr<ParticipantObservationEntry> participant, bool removed) noexcept {
        try {
            observe(guid, kind, topic, type, std::move(participant), removed);
        } catch (...) {
            capability_.store(DiscoveryCapability::Degraded, std::memory_order_release);
        }
    }
    DiscoveryCapability capability() const noexcept {
        return capability_.load(std::memory_order_acquire);
    }
    void degrade() noexcept {
        capability_.store(DiscoveryCapability::Degraded, std::memory_order_release);
    }

    RemoteEndpointObservationState lookup(
        const eprosima::fastrtps::rtps::GUID_t& guid) const noexcept {
        if (capability() != DiscoveryCapability::Healthy) {
            return RemoteEndpointObservationState::Degraded;
        }
        std::lock_guard lock(mutex_);
        for (const auto& entry : entries_) {
            if (entry.guid != guid) continue;
            return entry.lifecycle == RemoteEndpointLifecycle::Removed
                       ? RemoteEndpointObservationState::Removed
                       : RemoteEndpointObservationState::Active;
        }
        return RemoteEndpointObservationState::Unknown;
    }

    void add_dependency_wake(std::function<void()> wake) {
        std::lock_guard lock(mutex_);
        dependency_wakes_.push_back(std::move(wake));
    }

    /// Returns an exact service pairing verdict for one remote participant.
    /// A participant with relevant endpoint discovery must have both the
    /// request reader and response writer; otherwise a stale or partial graph
    /// cannot be advertised as an available service.
    ServicePairObservation service_pair(
        const eprosima::fastrtps::rtps::GuidPrefix_t& participant_prefix,
        const std::string& request_topic, const std::string& request_type,
        const std::string& response_topic, const std::string& response_type) const noexcept {
        if (capability() != DiscoveryCapability::Healthy) return ServicePairObservation::Degraded;
        std::lock_guard lock(mutex_);
        bool relevant = false;
        bool request_reader = false;
        bool response_writer = false;
        for (const auto& entry : entries_) {
            if (entry.guid.guidPrefix != participant_prefix ||
                entry.lifecycle != RemoteEndpointLifecycle::Active || !entry.participant ||
                entry.participant->lifecycle.load(std::memory_order_acquire) ==
                    ParticipantLifecycle::Removed) {
                continue;
            }
            if (entry.topic_name == request_topic && entry.type_name == request_type) {
                relevant = true;
                request_reader = request_reader || entry.kind == RemoteEndpointKind::Reader;
            }
            if (entry.topic_name == response_topic && entry.type_name == response_type) {
                relevant = true;
                response_writer = response_writer || entry.kind == RemoteEndpointKind::Writer;
            }
        }
        if (request_reader && response_writer) return ServicePairObservation::Complete;
        return relevant ? ServicePairObservation::Incomplete : ServicePairObservation::Unknown;
    }

public:
    void notify_dependents() noexcept {
        std::vector<std::function<void()>> wakes;
        try {
            std::lock_guard lock(mutex_);
            wakes = dependency_wakes_;
        } catch (...) {
            degrade();
            return;
        }
        for (const auto& wake : wakes) {
            try {
                wake();
            } catch (...) {
                degrade();
            }
        }
    }

private:
    mutable RankedMutex<LockRank::RemoteEndpoint> mutex_;
    std::atomic<DiscoveryCapability> capability_{DiscoveryCapability::Healthy};
    std::vector<RemoteEndpointObservation> entries_;
    std::vector<std::function<void()>> dependency_wakes_;
};

/// The sole Context-lifetime authority for remote participant tombstones.
class ParticipantObservationRegistry {
public:
    std::shared_ptr<ParticipantObservationEntry> observe(
        const eprosima::fastrtps::rtps::GuidPrefix_t& prefix, bool removed) {
        std::lock_guard lock(mutex_);
        if (capability_.load(std::memory_order_acquire) != DiscoveryCapability::Healthy) return {};
        for (const auto& entry : entries_) {
            if (std::memcmp(&entry->prefix, &prefix, sizeof(prefix)) == 0) {
                if (!removed && entry->lifecycle.load(std::memory_order_acquire) ==
                                    ParticipantLifecycle::Removed) {
                    // GuidPrefix reuse is explicitly outside the V1 deployment
                    // contract.  Never revive a tombstone: make discovery
                    // unusable so callers surface DdsError instead.
                    capability_.store(DiscoveryCapability::Degraded, std::memory_order_release);
                    return entry;
                }
                if (removed && entry->lifecycle.load(std::memory_order_acquire) !=
                                   ParticipantLifecycle::Removed) {
                    const auto generation = entry->generation.load(std::memory_order_acquire);
                    if (generation == UINT64_MAX) {
                        capability_.store(DiscoveryCapability::Degraded, std::memory_order_release);
                        return {};
                    }
                    entry->lifecycle.store(
                        ParticipantLifecycle::Removed, std::memory_order_release);
                    entry->generation.store(generation + 1, std::memory_order_release);
                }
                return entry;
            }
        }
        auto entry = std::make_shared<ParticipantObservationEntry>();
        entry->prefix = prefix;
        if (removed) {
            entry->lifecycle.store(ParticipantLifecycle::Removed, std::memory_order_release);
            entry->generation.store(1, std::memory_order_release);
        }
        entries_.push_back(entry);
        return entry;
    }

    std::shared_ptr<ParticipantObservationEntry> lookup(
        const eprosima::fastrtps::rtps::GuidPrefix_t& prefix) const noexcept {
        std::lock_guard lock(mutex_);
        for (const auto& entry : entries_) {
            if (entry->prefix == prefix) return entry;
        }
        return {};
    }

    void observe_noexcept(
        const eprosima::fastrtps::rtps::GuidPrefix_t& prefix, bool removed) noexcept {
        try {
            (void)observe(prefix, removed);
            notify_dependents();
        } catch (...) {
            capability_.store(DiscoveryCapability::Degraded, std::memory_order_release);
        }
    }

    DiscoveryCapability capability() const noexcept {
        return capability_.load(std::memory_order_acquire);
    }
    void degrade() noexcept {
        capability_.store(DiscoveryCapability::Degraded, std::memory_order_release);
    }

    void add_dependency_wake(std::function<void()> wake) {
        std::lock_guard lock(mutex_);
        dependency_wakes_.push_back(std::move(wake));
    }

private:
    mutable RankedMutex<LockRank::ParticipantObservation> mutex_;
    std::atomic<DiscoveryCapability> capability_{DiscoveryCapability::Healthy};
    std::vector<std::shared_ptr<ParticipantObservationEntry>> entries_;
    std::vector<std::function<void()>> dependency_wakes_;

    void notify_dependents() noexcept {
        std::vector<std::function<void()>> wakes;
        try {
            std::lock_guard lock(mutex_);
            wakes = dependency_wakes_;
        } catch (...) {
            degrade();
            return;
        }
        for (const auto& wake : wakes) {
            try {
                wake();
            } catch (...) {
                degrade();
            }
        }
    }
};

class ParticipantObservationListener final
: public eprosima::fastdds::dds::DomainParticipantListener {
public:
    ParticipantObservationListener(
        std::weak_ptr<ParticipantObservationRegistry> registry,
        std::weak_ptr<RemoteEndpointRegistry> endpoints,
        std::weak_ptr<TargetReaderObservationRegistry> target_readers) noexcept
    : registry_(std::move(registry)),
      endpoints_(std::move(endpoints)),
      target_readers_(std::move(target_readers)) {}

    void close_and_drain() noexcept {
        std::unique_lock lock(callback_mutex_);
        accepting_callbacks_ = false;
        callback_cv_.wait(lock, [this] { return callbacks_in_flight_ == 0; });
    }

    void on_participant_discovery(
        eprosima::fastdds::dds::DomainParticipant*,
        eprosima::fastrtps::rtps::ParticipantDiscoveryInfo&& info) override {
        CallbackGuard guard(*this);
        if (!guard) return;
        if (const auto registry = registry_.lock()) {
            registry->observe_noexcept(
                info.info.m_guid.guidPrefix,
                info.status ==
                        eprosima::fastrtps::rtps::ParticipantDiscoveryInfo::REMOVED_PARTICIPANT ||
                    info.status ==
                        eprosima::fastrtps::rtps::ParticipantDiscoveryInfo::DROPPED_PARTICIPANT);
        }
    }

    void on_subscriber_discovery(
        eprosima::fastdds::dds::DomainParticipant*,
        eprosima::fastrtps::rtps::ReaderDiscoveryInfo&& info) override {
        CallbackGuard guard(*this);
        if (!guard) return;
        observe_endpoint(
            info.info.guid(), RemoteEndpointKind::Reader, info.info.topicName().to_string(),
            info.info.typeName().to_string(),
            info.status == eprosima::fastrtps::rtps::ReaderDiscoveryInfo::REMOVED_READER);
    }
    void on_publisher_discovery(
        eprosima::fastdds::dds::DomainParticipant*,
        eprosima::fastrtps::rtps::WriterDiscoveryInfo&& info) override {
        CallbackGuard guard(*this);
        if (!guard) return;
        observe_endpoint(
            info.info.guid(), RemoteEndpointKind::Writer, info.info.topicName().to_string(),
            info.info.typeName().to_string(),
            info.status == eprosima::fastrtps::rtps::WriterDiscoveryInfo::REMOVED_WRITER);
    }

private:
    class CallbackGuard {
    public:
        explicit CallbackGuard(ParticipantObservationListener& listener) noexcept
        : listener_(&listener) {
            std::lock_guard lock(listener.callback_mutex_);
            if (listener.accepting_callbacks_)
                ++listener.callbacks_in_flight_;
            else
                listener_ = nullptr;
        }
        ~CallbackGuard() noexcept {
            if (!listener_) return;
            std::lock_guard lock(listener_->callback_mutex_);
            if (--listener_->callbacks_in_flight_ == 0) listener_->callback_cv_.notify_all();
        }
        explicit operator bool() const noexcept { return listener_ != nullptr; }

    private:
        ParticipantObservationListener* listener_;
    };
    void observe_endpoint(
        const eprosima::fastrtps::rtps::GUID_t& guid, RemoteEndpointKind kind,
        const std::string& topic, const std::string& type, bool removed) noexcept {
        const auto registry = registry_.lock();
        const auto endpoints = endpoints_.lock();
        if (!registry || !endpoints) return;
        try {
            const auto participant = registry->observe(guid.guidPrefix, false);
            if (!participant) return;
            endpoints->observe(guid, kind, topic, type, participant, removed);
            endpoints->notify_dependents();
            if (kind == RemoteEndpointKind::Reader) {
                if (const auto targets = target_readers_.lock()) {
                    targets->observe_noexcept(guid, participant, removed);
                }
            }
        } catch (...) {
            registry->degrade();
        }
    }
    std::weak_ptr<ParticipantObservationRegistry> registry_;
    std::weak_ptr<RemoteEndpointRegistry> endpoints_;
    std::weak_ptr<TargetReaderObservationRegistry> target_readers_;
    RankedMutex<LockRank::ListenerState> callback_mutex_;
    std::condition_variable_any callback_cv_;
    bool accepting_callbacks_{true};
    std::size_t callbacks_in_flight_{0};
};

}  // namespace dmw::impl

#endif
