// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__IMPL__SERVICE_MATCH_STATE_HPP_
#define DMW__IMPL__SERVICE_MATCH_STATE_HPP_

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/rtps/common/Guid.h>

namespace dmw {
namespace impl {

/// Tracks request and response matches by remote participant, not merely by count.
class ServiceMatchState {
public:
    void update_request(
        const eprosima::fastdds::dds::InstanceHandle_t& handle, std::int32_t change) {
        update(request_participants_, handle, change);
    }

    void update_response(
        const eprosima::fastdds::dds::InstanceHandle_t& handle, std::int32_t change) {
        update(response_participants_, handle, change);
    }

    bool has_candidate() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (degraded_) return false;
        for (const auto& request : request_participants_) {
            const auto response = std::find_if(
                response_participants_.begin(), response_participants_.end(),
                [&request](const ParticipantCount& candidate) {
                    return candidate.prefix == request.prefix;
                });
            if (response != response_participants_.end()) return true;
        }
        return false;
    }

    bool is_degraded() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return degraded_;
    }

    void degrade() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
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

        std::lock_guard<std::mutex> lock(mutex_);
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

    mutable std::mutex mutex_;
    bool degraded_{false};
    std::vector<ParticipantCount> request_participants_;
    std::vector<ParticipantCount> response_participants_;
};

class RequestWriterMatchListener final : public eprosima::fastdds::dds::DataWriterListener {
public:
    explicit RequestWriterMatchListener(std::weak_ptr<ServiceMatchState> state) noexcept
    : state_(std::move(state)) {}

    void on_publication_matched(
        eprosima::fastdds::dds::DataWriter*,
        const eprosima::fastdds::dds::PublicationMatchedStatus& status) override {
        if (const auto state = state_.lock()) {
            try {
                state->update_request(status.last_subscription_handle, status.current_count_change);
            } catch (...) {
                state->degrade();
            }
        }
    }

private:
    std::weak_ptr<ServiceMatchState> state_;
};

class ResponseReaderMatchListener final : public eprosima::fastdds::dds::DataReaderListener {
public:
    explicit ResponseReaderMatchListener(std::weak_ptr<ServiceMatchState> state) noexcept
    : state_(std::move(state)) {}

    void on_subscription_matched(
        eprosima::fastdds::dds::DataReader*,
        const eprosima::fastdds::dds::SubscriptionMatchedStatus& status) override {
        if (const auto state = state_.lock()) {
            try {
                state->update_response(status.last_publication_handle, status.current_count_change);
            } catch (...) {
                state->degrade();
            }
        }
    }

private:
    std::weak_ptr<ServiceMatchState> state_;
};

/// Tracks the response readers currently matched to one Server response writer.
class ResponseWriterMatchState {
public:
    enum class WaitStatus { Matched, Timeout, Degraded };

    void update(const eprosima::fastdds::dds::InstanceHandle_t& handle, std::int32_t change) {
        if (!handle.isDefined() || change == 0) return;
        const auto guid = eprosima::fastrtps::rtps::iHandle2GUID(handle);
        if (guid == eprosima::fastrtps::rtps::GUID_t::unknown()) return;

        std::lock_guard<std::mutex> lock(mutex_);
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
        std::unique_lock<std::mutex> lock(mutex_);
        const auto matched = [this, &reader] {
            return std::any_of(
                readers_.begin(), readers_.end(),
                [&reader](const ReaderCount& candidate) { return candidate.guid == reader; });
        };
        if (degraded_) return WaitStatus::Degraded;
        if (matched()) return WaitStatus::Matched;
        if (!cv_.wait_until(lock, deadline, [this, &matched] { return degraded_ || matched(); })) {
            return WaitStatus::Timeout;
        }
        return degraded_ ? WaitStatus::Degraded : WaitStatus::Matched;
    }

    void degrade() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        degraded_ = true;
        readers_.clear();
        cv_.notify_all();
    }

private:
    struct ReaderCount {
        eprosima::fastrtps::rtps::GUID_t guid;
        std::size_t count{0};
    };

    std::mutex mutex_;
    std::condition_variable cv_;
    bool degraded_{false};
    std::vector<ReaderCount> readers_;
};

class ResponseWriterMatchListener final : public eprosima::fastdds::dds::DataWriterListener {
public:
    explicit ResponseWriterMatchListener(std::weak_ptr<ResponseWriterMatchState> state) noexcept
    : state_(std::move(state)) {}

    void on_publication_matched(
        eprosima::fastdds::dds::DataWriter*,
        const eprosima::fastdds::dds::PublicationMatchedStatus& status) override {
        if (const auto state = state_.lock()) {
            try {
                state->update(status.last_subscription_handle, status.current_count_change);
            } catch (...) {
                state->degrade();
            }
        }
    }

private:
    std::weak_ptr<ResponseWriterMatchState> state_;
};

}  // namespace impl
}  // namespace dmw

#endif  // DMW__IMPL__SERVICE_MATCH_STATE_HPP_
