#ifndef DMW_IMPL__CLIENT_IMPL_HPP_
#define DMW_IMPL__CLIENT_IMPL_HPP_

#include <memory>
#include <string>

#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>

#include "dmw/client.hpp"
#include "impl/context.hpp"
#include "impl/reader_wait_state.hpp"
#include "impl/request.hpp"
#include "impl/response.hpp"

namespace dmw {

class Client::Impl {
public:
    Impl(
        std::shared_ptr<impl::Context> context, std::string service_name, MessageType response_type,
        std::shared_ptr<impl::RequestState> request_state, impl::Topic request_topic,
        eprosima::fastdds::dds::DataWriter* request_writer,
        std::unique_ptr<impl::RequestWriterListener> request_listener, impl::Topic response_topic,
        eprosima::fastdds::dds::DataReader* response_reader,
        std::unique_ptr<impl::ResponseReaderListener> response_listener) noexcept
    : context_(context),
      service_name_(std::move(service_name)),
      response_type_(std::move(response_type)),
      request_state_(std::move(request_state)),
      request_topic_(std::move(request_topic)),
      request_writer_(request_writer),
      request_listener_(std::move(request_listener)),
      response_topic_(std::move(response_topic)),
      response_reader_(response_reader),
      response_listener_(std::move(response_listener)),
      response_wait_state_(
          std::make_shared<impl::ReaderWaitState>(std::move(context), response_reader)) {}
    ~Impl() noexcept;

    std::string_view service_name() const noexcept { return service_name_; }
    Result<RequestId> write_request(const void* request);
    Result<bool> read_response(void* response, RequestId& request_id);
    Result<bool> service_is_available() const;
    const std::shared_ptr<impl::ReaderWaitState>& wait_state() const noexcept {
        return response_wait_state_;
    }

private:
    std::shared_ptr<impl::Context> context_;
    std::string service_name_;
    MessageType response_type_;
    std::shared_ptr<impl::RequestState> request_state_;
    impl::Topic request_topic_;
    eprosima::fastdds::dds::DataWriter* request_writer_;
    std::unique_ptr<impl::RequestWriterListener> request_listener_;
    impl::Topic response_topic_;
    eprosima::fastdds::dds::DataReader* response_reader_;
    std::unique_ptr<impl::ResponseReaderListener> response_listener_;
    std::shared_ptr<impl::ReaderWaitState> response_wait_state_;
};

}  // namespace dmw

#endif  // DMW_IMPL__CLIENT_IMPL_HPP_
