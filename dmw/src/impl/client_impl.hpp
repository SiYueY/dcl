#ifndef DMW_IMPL__CLIENT_IMPL_HPP_
#define DMW_IMPL__CLIENT_IMPL_HPP_

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>

#include "dmw/client.hpp"
#include "impl/context.hpp"
#include "impl/discovery_graph.hpp"
#include "impl/reader_wait_state.hpp"
#include "impl/request.hpp"
#include "impl/response.hpp"
#include "impl/temporary_sample.hpp"

namespace dmw {

class Client::Impl {
public:
    struct ServiceWaitState {
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<std::uint64_t> revision{0};
    };

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
          std::make_shared<impl::ReaderWaitState>(context_, response_reader)),
      service_wait_state_(std::make_shared<ServiceWaitState>()) {
        const std::weak_ptr<ServiceWaitState> weak_state = service_wait_state_;
        service_subscription_ = context_->discovery_graph()->subscribe([weak_state](std::uint64_t) {
            if (const auto state = weak_state.lock()) {
                state->revision.fetch_add(1, std::memory_order_release);
                state->cv.notify_all();
            }
        });
        shutdown_callback_id_ = context_->register_shutdown_callback([weak_state] {
            if (const auto state = weak_state.lock()) {
                state->revision.fetch_add(1, std::memory_order_release);
                state->cv.notify_all();
            }
        });
    }
    ~Impl() noexcept;

    std::string_view service_name() const noexcept { return service_name_; }
    Result<RequestId> write_request(const void* request);
    Result<bool> read_response(void* response, RequestId& request_id);
    Result<bool> service_is_available() const;
    Result<bool> wait_for_service(WaitTimeout timeout) const;
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
    std::shared_ptr<ServiceWaitState> service_wait_state_;
    impl::DiscoveryGraph::Subscription service_subscription_;
    std::uint64_t shutdown_callback_id_{0};
    std::mutex response_read_mutex_;
    std::unique_ptr<impl::TemporarySample> response_scratch_;
};

}  // namespace dmw

#endif  // DMW_IMPL__CLIENT_IMPL_HPP_
