#ifndef DMW_IMPL__SERVER_IMPL_HPP_
#define DMW_IMPL__SERVER_IMPL_HPP_

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/rtps/common/SampleIdentity.h>

#include "dmw/server.hpp"
#include "impl/service_state.hpp"

namespace dmw {

class Server::Impl {
public:
    enum class PendingPhase { Pending, Responding };

    struct PendingRequest {
        eprosima::fastrtps::rtps::SampleIdentity sample_identity;
        PendingPhase phase{PendingPhase::Pending};
    };

    Impl(
        std::shared_ptr<impl::fastdds::ContextState> state,
        eprosima::fastdds::dds::DataReader* request_reader,
        eprosima::fastdds::dds::DataWriter* response_writer, std::string service_name,
        std::size_t max_pending_requests, MessageType request_type,
        std::shared_ptr<impl::ResponseWriterMatchState> response_match_state,
        std::unique_ptr<impl::ResponseWriterMatchListener> response_match_listener,
        impl::fastdds::ContextState::TopicLease request_topic_lease,
        impl::fastdds::ContextState::TopicLease response_topic_lease) noexcept
    : state_(state),
      request_reader_(request_reader),
      response_writer_(response_writer),
      service_name_(std::move(service_name)),
      max_pending_requests_(max_pending_requests),
      request_type_(std::move(request_type)),
      response_match_state_(std::move(response_match_state)),
      response_match_listener_(std::move(response_match_listener)),
      request_wait_state_(
          std::make_shared<impl::ReaderWaitState>(std::move(state), request_reader)),
      request_topic_lease_(std::move(request_topic_lease)),
      response_topic_lease_(std::move(response_topic_lease)) {}
    ~Impl() noexcept;

    std::string_view service_name() const noexcept { return service_name_; }
    Result<TakeStatus> take_request(void* request, RequestId& request_id);
    Result<void> send_response(const RequestId& request_id, const void* response);

    bool reserve_request_slot(bool& became_full) noexcept {
        std::lock_guard lock(pending_mutex_);
        if (pending_.size() + reservations_ >= max_pending_requests_) return false;
        ++reservations_;
        became_full = pending_.size() + reservations_ == max_pending_requests_;
        return true;
    }

    bool release_request_reservation() noexcept {
        std::lock_guard lock(pending_mutex_);
        --reservations_;
        return pending_.size() + reservations_ + 1 == max_pending_requests_;
    }

    bool release_pending_request(const RequestId& request_id) noexcept {
        std::lock_guard lock(pending_mutex_);
        const auto was_full = pending_.size() + reservations_ == max_pending_requests_;
        pending_.erase(request_id);
        return was_full;
    }

    std::shared_ptr<impl::fastdds::ContextState> state_;
    eprosima::fastdds::dds::DataReader* request_reader_;
    eprosima::fastdds::dds::DataWriter* response_writer_;
    std::string service_name_;
    std::size_t max_pending_requests_;
    MessageType request_type_;
    std::shared_ptr<impl::ResponseWriterMatchState> response_match_state_;
    std::unique_ptr<impl::ResponseWriterMatchListener> response_match_listener_;
    std::shared_ptr<impl::ReaderWaitState> request_wait_state_;
    impl::fastdds::ContextState::TopicLease request_topic_lease_;
    impl::fastdds::ContextState::TopicLease response_topic_lease_;
    impl::RankedMutex<impl::LockRank::PendingRequest> pending_mutex_;
    std::size_t reservations_{0};
    std::unordered_map<RequestId, PendingRequest, RequestIdHash> pending_;
};

}  // namespace dmw

#endif  // DMW_IMPL__SERVER_IMPL_HPP_
