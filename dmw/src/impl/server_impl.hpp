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
#include "impl/context.hpp"
#include "impl/reader_wait_state.hpp"
#include "impl/response.hpp"

namespace dmw {

class Server::Impl {
public:
    Impl(
        std::shared_ptr<impl::Context> context, eprosima::fastdds::dds::DataReader* request_reader,
        eprosima::fastdds::dds::DataWriter* response_writer, std::string service_name,
        std::size_t max_pending_requests, MessageType request_type,
        std::shared_ptr<impl::ResponseState> response_state,
        std::unique_ptr<impl::ResponseWriterListener> response_listener, impl::Topic request_topic,
        impl::Topic response_topic) noexcept
    : context_(context),
      request_reader_(request_reader),
      response_writer_(response_writer),
      service_name_(std::move(service_name)),
      max_pending_requests_(max_pending_requests),
      request_type_(std::move(request_type)),
      response_state_(std::move(response_state)),
      response_listener_(std::move(response_listener)),
      request_wait_state_(
          std::make_shared<impl::ReaderWaitState>(std::move(context), request_reader)),
      request_topic_(std::move(request_topic)),
      response_topic_(std::move(response_topic)) {}
    ~Impl() noexcept;

    std::string_view service_name() const noexcept { return service_name_; }
    Result<bool> take_request(void* request, RequestId& request_id);
    Result<void> send_response(const RequestId& request_id, const void* response);
    const std::shared_ptr<impl::ReaderWaitState>& wait_state() const noexcept {
        return request_wait_state_;
    }

private:
    enum class PendingPhase { Pending, Responding };

    struct PendingRequest {
        eprosima::fastrtps::rtps::SampleIdentity sample_identity;
        PendingPhase phase{PendingPhase::Pending};
    };

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

    std::shared_ptr<impl::Context> context_;
    eprosima::fastdds::dds::DataReader* request_reader_;
    eprosima::fastdds::dds::DataWriter* response_writer_;
    std::string service_name_;
    std::size_t max_pending_requests_;
    MessageType request_type_;
    std::shared_ptr<impl::ResponseState> response_state_;
    std::unique_ptr<impl::ResponseWriterListener> response_listener_;
    std::shared_ptr<impl::ReaderWaitState> request_wait_state_;
    impl::Topic request_topic_;
    impl::Topic response_topic_;
    impl::RankedMutex<impl::LockRank::PendingRequest> pending_mutex_;
    std::size_t reservations_{0};
    std::unordered_map<RequestId, PendingRequest, RequestIdHash> pending_;
};

}  // namespace dmw

#endif  // DMW_IMPL__SERVER_IMPL_HPP_
