#ifndef DMW_IMPL__RESPONSE_HPP_
#define DMW_IMPL__RESPONSE_HPP_

#include "impl/request.hpp"

namespace dmw {
namespace impl {

class ResponseReaderListener final : public eprosima::fastdds::dds::DataReaderListener {
public:
    explicit ResponseReaderListener(std::weak_ptr<RequestState> state) noexcept
    : state_(std::move(state)) {}

    void on_subscription_matched(
        eprosima::fastdds::dds::DataReader*,
        const eprosima::fastdds::dds::SubscriptionMatchedStatus& status) override {
        const auto callback = callbacks_.enter();
        if (!callback) return;
        if (const auto state = state_.lock()) {
            try {
                state->observe_response_writer(
                    status.last_publication_handle, status.current_count_change);
            } catch (...) {
                state->degrade();
            }
        }
    }

    void close_and_drain() noexcept { callbacks_.close_and_drain(); }

private:
    std::weak_ptr<RequestState> state_;
    CallbackInFlightGate callbacks_;
};

/// Tracks response readers observed by one response writer.
class ResponseState {
public:
    /// Removed is terminal for an exact response reader: writing a response
    /// after its reader has disappeared is neither useful nor required.
    enum class TargetStatus { Ready, Removed, TimedOut, Unavailable };

    explicit ResponseState(
        std::weak_ptr<ParticipantObservationRegistry> participants = {},
        std::weak_ptr<TargetReaderObservationRegistry> target_readers = {}) noexcept
    : participants_(std::move(participants)), target_readers_(std::move(target_readers)) {}

    void observe_reader(
        const eprosima::fastdds::dds::InstanceHandle_t& handle, std::int32_t change) {
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

    TargetStatus wait_for_target(
        const eprosima::fastrtps::rtps::GUID_t& reader,
        std::chrono::steady_clock::time_point deadline) {
        auto target = target_snapshot(reader);
        std::unique_lock lock(mutex_);
        const auto ready = [this, &reader] {
            return std::any_of(
                readers_.begin(), readers_.end(),
                [&reader](const ReaderCount& candidate) { return candidate.guid == reader; });
        };
        if (degraded_) return TargetStatus::Unavailable;
        while (true) {
            if (degraded_ || participant_degraded(target)) return TargetStatus::Unavailable;
            if (target_removed(target)) return TargetStatus::Removed;
            if (ready()) return TargetStatus::Ready;
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                return TargetStatus::TimedOut;
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
    /// never waits: it closes the interval between wait_for_target() and DDS
    /// write where discovery may have reported a terminal removal.
    TargetStatus check_target(const eprosima::fastrtps::rtps::GUID_t& reader) const noexcept {
        const auto target = target_snapshot(reader);
        std::lock_guard lock(mutex_);
        if (degraded_ || participant_degraded(target)) return TargetStatus::Unavailable;
        if (target_removed(target)) return TargetStatus::Removed;
        return std::any_of(
                   readers_.begin(), readers_.end(),
                   [&reader](const ReaderCount& candidate) { return candidate.guid == reader; })
                   ? TargetStatus::Ready
                   : TargetStatus::TimedOut;
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
        auto snapshot =
            targets ? targets->snapshot(reader) : TargetReaderObservationRegistry::Snapshot{};
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

    bool participant_degraded(
        const TargetReaderObservationRegistry::Snapshot& target) const noexcept {
        const auto participants = participants_.lock();
        const auto targets = target_readers_.lock();
        return (participants && participants->capability() != DiscoveryCapability::Healthy) ||
               (targets && targets->capability() != DiscoveryCapability::Healthy) ||
               target.state == RemoteEndpointObservationState::Degraded;
    }

    static bool target_removed(const TargetReaderObservationRegistry::Snapshot& target) noexcept {
        return target.state == RemoteEndpointObservationState::Removed ||
               (target.participant &&
                target.participant->lifecycle.load(std::memory_order_acquire) ==
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

class ResponseWriterListener final : public eprosima::fastdds::dds::DataWriterListener {
public:
    explicit ResponseWriterListener(std::weak_ptr<ResponseState> state) noexcept
    : state_(std::move(state)) {}

    void on_publication_matched(
        eprosima::fastdds::dds::DataWriter*,
        const eprosima::fastdds::dds::PublicationMatchedStatus& status) override {
        const auto callback = callbacks_.enter();
        if (!callback) return;
        if (const auto state = state_.lock()) {
            try {
                state->observe_reader(status.last_subscription_handle, status.current_count_change);
            } catch (...) {
                state->degrade();
            }
        }
    }

    void close_and_drain() noexcept { callbacks_.close_and_drain(); }

private:
    std::weak_ptr<ResponseState> state_;
    CallbackInFlightGate callbacks_;
};

}  // namespace impl
}  // namespace dmw

#endif  // DMW_IMPL__RESPONSE_HPP_
