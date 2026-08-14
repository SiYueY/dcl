#ifndef DMW_IMPL__SERVICE_MATCH_STATE_HPP_
#define DMW_IMPL__SERVICE_MATCH_STATE_HPP_

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/rtps/common/Guid.h>

#include "impl/lock_rank.hpp"
#include "impl/participant_observation.hpp"

namespace dmw {
namespace impl {

/// Tracks request and response matches by remote participant, not merely by count.
class ServiceMatchState {
public:
    explicit ServiceMatchState(
        std::weak_ptr<ParticipantObservationRegistry> participants = {},
        std::weak_ptr<RemoteEndpointRegistry> endpoints = {}, std::string request_topic = {},
        std::string request_type = {}, std::string response_topic = {},
        std::string response_type = {}) noexcept
    : participants_(std::move(participants)),
      endpoints_(std::move(endpoints)),
      request_topic_(std::move(request_topic)),
      request_type_(std::move(request_type)),
      response_topic_(std::move(response_topic)),
      response_type_(std::move(response_type)) {}
    void update_request(
        const eprosima::fastdds::dds::InstanceHandle_t& handle, std::int32_t change) {
        update(request_participants_, handle, change);
    }

    void update_response(
        const eprosima::fastdds::dds::InstanceHandle_t& handle, std::int32_t change) {
        update(response_participants_, handle, change);
    }

    bool has_candidate() const {
        // Do not hold the service-match lock while entering the participant
        // or endpoint registries.  Those registries have lower ranks, and a
        // matched callback is deliberately only a local snapshot rather than
        // discovery authority.
        std::vector<ParticipantCount> requests;
        std::vector<ParticipantCount> responses;
        std::weak_ptr<ParticipantObservationRegistry> participants;
        std::weak_ptr<RemoteEndpointRegistry> endpoints;
        std::string request_topic;
        std::string request_type;
        std::string response_topic;
        std::string response_type;
        {
            std::lock_guard lock(mutex_);
            if (degraded_) return false;
            requests = request_participants_;
            responses = response_participants_;
            participants = participants_;
            endpoints = endpoints_;
            request_topic = request_topic_;
            request_type = request_type_;
            response_topic = response_topic_;
            response_type = response_type_;
        }

        if (const auto participant_registry = participants.lock()) {
            if (participant_registry->capability() != DiscoveryCapability::Healthy) return false;
        }
        for (const auto& request : requests) {
            const auto response = std::find_if(
                responses.begin(), responses.end(),
                [&request](const ParticipantCount& candidate) {
                    return candidate.prefix == request.prefix;
                });
            if (response == responses.end()) continue;
            if (const auto participant_registry = participants.lock()) {
                if (const auto participant = participant_registry->lookup(request.prefix)) {
                    if (participant->lifecycle.load(std::memory_order_acquire) ==
                        ParticipantLifecycle::Removed) {
                        continue;
                    }
                }
            }
            if (const auto endpoint_registry = endpoints.lock()) {
                const auto pairing = endpoint_registry->service_pair(
                    request.prefix, request_topic, request_type, response_topic, response_type);
                if (pairing == ServicePairObservation::Degraded) return false;
                if (pairing == ServicePairObservation::Incomplete) continue;
                if (pairing == ServicePairObservation::Complete) return true;
                // Once endpoint discovery is installed, a matched callback
                // alone is not authority for service availability.  Wait for
                // the exact request-reader/response-writer graph commit.
                continue;
            }
            return true;
        }
        return false;
    }

    bool is_degraded() const noexcept {
        std::lock_guard lock(mutex_);
        const auto participants = participants_.lock();
        const auto endpoints = endpoints_.lock();
        return degraded_ ||
               (participants && participants->capability() != DiscoveryCapability::Healthy) ||
               (endpoints && endpoints->capability() != DiscoveryCapability::Healthy);
    }

    void degrade() noexcept {
        std::lock_guard lock(mutex_);
        degraded_ = true;
        request_participants_.clear();
        response_participants_.clear();
    }

private:
    using GuidPrefix = eprosima::fastrtps::rtps::GuidPrefix_t;

    struct ParticipantCount {
        GuidPrefix prefix;
        std::size_t count{0};
    };

    void update(
        std::vector<ParticipantCount>& participants,
        const eprosima::fastdds::dds::InstanceHandle_t& handle, std::int32_t change) {
        if (!handle.isDefined() || change == 0) return;
        const auto prefix = eprosima::fastrtps::rtps::iHandle2GUID(handle).guidPrefix;
        if (prefix == eprosima::fastrtps::rtps::c_GuidPrefix_Unknown) return;

        std::lock_guard lock(mutex_);
        const auto found = std::find_if(
            participants.begin(), participants.end(),
            [&prefix](const ParticipantCount& candidate) { return candidate.prefix == prefix; });
        if (change > 0) {
            if (found == participants.end()) {
                participants.push_back(ParticipantCount{prefix, static_cast<std::size_t>(change)});
            } else {
                found->count += static_cast<std::size_t>(change);
            }
            return;
        }
        if (found == participants.end()) return;
        const auto decrement = static_cast<std::size_t>(-static_cast<std::int64_t>(change));
        if (decrement >= found->count) {
            participants.erase(found);
        } else {
            found->count -= decrement;
        }
    }

    mutable RankedMutex<LockRank::ServiceMatch> mutex_;
    bool degraded_{false};
    std::vector<ParticipantCount> request_participants_;
    std::vector<ParticipantCount> response_participants_;
    std::weak_ptr<ParticipantObservationRegistry> participants_;
    std::weak_ptr<RemoteEndpointRegistry> endpoints_;
    std::string request_topic_;
    std::string request_type_;
    std::string response_topic_;
    std::string response_type_;
};

/// Keeps a DDS listener alive until every callback that entered before its
/// removal has returned.  Fast DDS does not make endpoint deletion a
/// substitute for draining user callbacks, so endpoint owners call
/// close_and_drain() after set_listener(nullptr) and before deleting the DDS
/// endpoint.
class CallbackInFlightGate {
public:
    class Guard {
    public:
        Guard() noexcept = default;
        explicit Guard(CallbackInFlightGate* gate) noexcept : gate_(gate) {}
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&& other) noexcept : gate_(std::exchange(other.gate_, nullptr)) {}
        Guard& operator=(Guard&& other) noexcept {
            if (this != &other) {
                release();
                gate_ = std::exchange(other.gate_, nullptr);
            }
            return *this;
        }
        ~Guard() { release(); }
        explicit operator bool() const noexcept { return gate_ != nullptr; }

    private:
        void release() noexcept {
            if (gate_ == nullptr) return;
            std::lock_guard lock(gate_->mutex_);
            --gate_->callbacks_in_flight_;
            if (gate_->callbacks_in_flight_ == 0) gate_->cv_.notify_all();
            gate_ = nullptr;
        }
        CallbackInFlightGate* gate_{nullptr};
    };

    Guard enter() noexcept {
        std::lock_guard lock(mutex_);
        if (!accepting_callbacks_) return Guard{};
        ++callbacks_in_flight_;
        return Guard(this);
    }

    void close_and_drain() noexcept {
        std::unique_lock lock(mutex_);
        accepting_callbacks_ = false;
        cv_.wait(lock, [this] { return callbacks_in_flight_ == 0; });
    }

private:
    mutable RankedMutex<LockRank::ListenerState> mutex_;
    std::condition_variable_any cv_;
    bool accepting_callbacks_{true};
    std::size_t callbacks_in_flight_{0};
};

class RequestWriterMatchListener final : public eprosima::fastdds::dds::DataWriterListener {
public:
    explicit RequestWriterMatchListener(std::weak_ptr<ServiceMatchState> state) noexcept
    : state_(std::move(state)) {}

    void on_publication_matched(
        eprosima::fastdds::dds::DataWriter*,
        const eprosima::fastdds::dds::PublicationMatchedStatus& status) override {
        const auto callback = callbacks_.enter();
        if (!callback) return;
        if (const auto state = state_.lock()) {
            try {
                state->update_request(status.last_subscription_handle, status.current_count_change);
            } catch (...) {
                state->degrade();
            }
        }
    }

    void close_and_drain() noexcept { callbacks_.close_and_drain(); }

private:
    std::weak_ptr<ServiceMatchState> state_;
    CallbackInFlightGate callbacks_;
};

class ResponseReaderMatchListener final : public eprosima::fastdds::dds::DataReaderListener {
public:
    explicit ResponseReaderMatchListener(std::weak_ptr<ServiceMatchState> state) noexcept
    : state_(std::move(state)) {}

    void on_subscription_matched(
        eprosima::fastdds::dds::DataReader*,
        const eprosima::fastdds::dds::SubscriptionMatchedStatus& status) override {
        const auto callback = callbacks_.enter();
        if (!callback) return;
        if (const auto state = state_.lock()) {
            try {
                state->update_response(status.last_publication_handle, status.current_count_change);
            } catch (...) {
                state->degrade();
            }
        }
    }

    void close_and_drain() noexcept { callbacks_.close_and_drain(); }

private:
    std::weak_ptr<ServiceMatchState> state_;
    CallbackInFlightGate callbacks_;
};

/// Tracks the response readers currently matched to one Server response writer.
class ResponseWriterMatchState {
public:
    /// Removed is terminal for an exact response reader: writing a response
    /// after its reader has disappeared is neither useful nor required.
    enum class WaitStatus { Matched, Removed, Timeout, Degraded };

    explicit ResponseWriterMatchState(
        std::weak_ptr<ParticipantObservationRegistry> participants = {},
        std::weak_ptr<TargetReaderObservationRegistry> target_readers = {}) noexcept
    : participants_(std::move(participants)), target_readers_(std::move(target_readers)) {}

    void update(const eprosima::fastdds::dds::InstanceHandle_t& handle, std::int32_t change) {
        if (!handle.isDefined() || change == 0) return;
        const auto guid = eprosima::fastrtps::rtps::iHandle2GUID(handle);
        if (guid == eprosima::fastrtps::rtps::GUID_t::unknown()) return;

        std::lock_guard lock(mutex_);
        const auto found = std::find_if(
            readers_.begin(), readers_.end(),
            [&guid](const ReaderCount& candidate) { return candidate.guid == guid; });
        if (change > 0) {
            if (found == readers_.end()) {
                readers_.push_back(ReaderCount{guid, static_cast<std::size_t>(change)});
            } else {
                found->count += static_cast<std::size_t>(change);
            }
        } else if (found != readers_.end()) {
            const auto decrement = static_cast<std::size_t>(-static_cast<std::int64_t>(change));
            if (decrement >= found->count) {
                readers_.erase(found);
            } else {
                found->count -= decrement;
            }
        }
        cv_.notify_all();
    }

    WaitStatus wait_for_match(
        const eprosima::fastrtps::rtps::GUID_t& reader,
        std::chrono::steady_clock::time_point deadline) {
        auto target = target_snapshot(reader);
        std::unique_lock lock(mutex_);
        const auto matched = [this, &reader] {
            return std::any_of(
                readers_.begin(), readers_.end(),
                [&reader](const ReaderCount& candidate) { return candidate.guid == reader; });
        };
        if (degraded_) return WaitStatus::Degraded;
        while (true) {
            if (degraded_ || participant_degraded(target)) return WaitStatus::Degraded;
            if (target_removed(target)) return WaitStatus::Removed;
            if (matched()) return WaitStatus::Matched;
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                return WaitStatus::Timeout;
            }
            // Never enter Participant/Target registry while holding this
            // target state mutex.  Refresh the stable participant handle
            // outside the lock, then inspect only atomic lifecycle fields.
            lock.unlock();
            target = target_snapshot(reader);
            lock.lock();
        }
    }

    /// Revalidate an exact response target at the write commit point.  This
    /// never waits: it closes the interval between wait_for_match() and DDS
    /// write where discovery may have reported a terminal removal.
    WaitStatus check_target(const eprosima::fastrtps::rtps::GUID_t& reader) const noexcept {
        const auto target = target_snapshot(reader);
        std::lock_guard lock(mutex_);
        if (degraded_ || participant_degraded(target)) return WaitStatus::Degraded;
        if (target_removed(target)) return WaitStatus::Removed;
        return std::any_of(
                   readers_.begin(), readers_.end(),
                   [&reader](const ReaderCount& candidate) { return candidate.guid == reader; })
                   ? WaitStatus::Matched
                   : WaitStatus::Timeout;
    }

    void degrade() noexcept {
        std::lock_guard lock(mutex_);
        degraded_ = true;
        readers_.clear();
        cv_.notify_all();
    }

    void notify_dependency_change() noexcept { cv_.notify_all(); }

private:
    TargetReaderObservationRegistry::Snapshot target_snapshot(
        const eprosima::fastrtps::rtps::GUID_t& reader) const noexcept {
        const auto targets = target_readers_.lock();
        auto snapshot = targets ? targets->snapshot(reader) : TargetReaderObservationRegistry::Snapshot{};
        // This helper is called only before taking (or after releasing) the
        // response-target mutex.  It captures a stable participant handle so
        // the predicate itself needs only an atomic lifecycle read.
        if (!snapshot.participant) {
            if (const auto participants = participants_.lock()) {
                snapshot.participant = participants->lookup(reader.guidPrefix);
            }
        }
        return snapshot;
    }

    bool participant_degraded(const TargetReaderObservationRegistry::Snapshot& target) const noexcept {
        const auto participants = participants_.lock();
        const auto targets = target_readers_.lock();
        return (participants && participants->capability() != DiscoveryCapability::Healthy) ||
               (targets && targets->capability() != DiscoveryCapability::Healthy) ||
               target.state == RemoteEndpointObservationState::Degraded;
    }

    static bool target_removed(
        const TargetReaderObservationRegistry::Snapshot& target) noexcept {
        return target.state == RemoteEndpointObservationState::Removed ||
               (target.participant && target.participant->lifecycle.load(std::memory_order_acquire) ==
                                          ParticipantLifecycle::Removed);
    }

    struct ReaderCount {
        eprosima::fastrtps::rtps::GUID_t guid;
        std::size_t count{0};
    };

    mutable RankedMutex<LockRank::TargetReader> mutex_;
    std::condition_variable_any cv_;
    bool degraded_{false};
    std::vector<ReaderCount> readers_;
    std::weak_ptr<ParticipantObservationRegistry> participants_;
    std::weak_ptr<TargetReaderObservationRegistry> target_readers_;
};

class ResponseWriterMatchListener final : public eprosima::fastdds::dds::DataWriterListener {
public:
    explicit ResponseWriterMatchListener(std::weak_ptr<ResponseWriterMatchState> state) noexcept
    : state_(std::move(state)) {}

    void on_publication_matched(
        eprosima::fastdds::dds::DataWriter*,
        const eprosima::fastdds::dds::PublicationMatchedStatus& status) override {
        const auto callback = callbacks_.enter();
        if (!callback) return;
        if (const auto state = state_.lock()) {
            try {
                state->update(status.last_subscription_handle, status.current_count_change);
            } catch (...) {
                state->degrade();
            }
        }
    }

    void close_and_drain() noexcept { callbacks_.close_and_drain(); }

private:
    std::weak_ptr<ResponseWriterMatchState> state_;
    CallbackInFlightGate callbacks_;
};

}  // namespace impl
}  // namespace dmw

#endif  // DMW_IMPL__SERVICE_MATCH_STATE_HPP_
