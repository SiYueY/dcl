#ifndef DMW_IMPL__CLIENT_IMPL_HPP_
#define DMW_IMPL__CLIENT_IMPL_HPP_

#include <memory>
#include <string>

#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>

#include "dmw/client.hpp"
#include "impl/service_state.hpp"

namespace dmw {

class Client::Impl {
public:
    Impl(
        std::shared_ptr<impl::fastdds::ContextState> state,
        eprosima::fastdds::dds::DataWriter* request_writer,
        eprosima::fastdds::dds::DataReader* response_reader, std::string service_name,
        MessageType response_type, std::shared_ptr<impl::ServiceMatchState> match_state,
        std::unique_ptr<impl::RequestWriterMatchListener> request_listener,
        std::unique_ptr<impl::ResponseReaderMatchListener> response_listener,
        impl::fastdds::ContextState::TopicLease request_topic_lease,
        impl::fastdds::ContextState::TopicLease response_topic_lease) noexcept
    : state_(state),
      request_writer_(request_writer),
      response_reader_(response_reader),
      service_name_(std::move(service_name)),
      response_type_(std::move(response_type)),
      match_state_(std::move(match_state)),
      request_listener_(std::move(request_listener)),
      response_listener_(std::move(response_listener)),
      response_wait_state_(
          std::make_shared<impl::ReaderWaitState>(std::move(state), response_reader)),
      request_topic_lease_(std::move(request_topic_lease)),
      response_topic_lease_(std::move(response_topic_lease)) {}
    ~Impl() noexcept;

    std::string_view service_name() const noexcept { return service_name_; }
    Result<RequestId> send_request(const void* request);
    Result<TakeStatus> take_response(void* response, RequestId& request_id);
    Result<bool> service_is_available() const;

    std::shared_ptr<impl::fastdds::ContextState> state_;
    eprosima::fastdds::dds::DataWriter* request_writer_;
    eprosima::fastdds::dds::DataReader* response_reader_;
    std::string service_name_;
    MessageType response_type_;
    std::shared_ptr<impl::ServiceMatchState> match_state_;
    std::unique_ptr<impl::RequestWriterMatchListener> request_listener_;
    std::unique_ptr<impl::ResponseReaderMatchListener> response_listener_;
    std::shared_ptr<impl::ReaderWaitState> response_wait_state_;
    impl::fastdds::ContextState::TopicLease request_topic_lease_;
    impl::fastdds::ContextState::TopicLease response_topic_lease_;
};

}  // namespace dmw

#endif  // DMW_IMPL__CLIENT_IMPL_HPP_
