#include "impl/client_impl.hpp"

#include <memory>
#include <new>
#include <string_view>
#include <utility>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/rtps/common/WriteParams.h>

#include "dmw/error.hpp"
#include "impl/identity.hpp"
#include "impl/process_lifetime.hpp"
#include "impl/qos.hpp"
#include "impl/return_code.hpp"
#include "impl/temporary_sample.hpp"

namespace dmw {

Client::Impl::~Impl() noexcept {
    service_subscription_.close_and_drain();
    if (shutdown_callback_id_ != 0) {
        context_->unregister_shutdown_callback(shutdown_callback_id_);
        shutdown_callback_id_ = 0;
    }
    if (response_reader_ != nullptr) {
        bool listener_detached = false;
        try {
            listener_detached = response_reader_->set_listener(nullptr) ==
                                eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
            if (listener_detached) response_listener_->close_and_drain();
        } catch (...) {
            listener_detached = false;
        }
        // WaitSet detachment is independent of listener ownership.
        const bool reader_closed = response_wait_state_->close();
        if (listener_detached && reader_closed) {
            try {
                context_->subscriber()->delete_datareader(response_reader_);
            } catch (...) {
                // The Context container remains the conservative ownership barrier.
            }
        }
        if (!listener_detached) {
            impl::ProcessLifetime::instance().retain_reader_listener(std::move(response_listener_));
        }
        response_reader_ = nullptr;
    }
    if (request_writer_ != nullptr) {
        bool listener_detached = false;
        try {
            listener_detached = request_writer_->set_listener(nullptr) ==
                                eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
            if (listener_detached) {
                request_listener_->close_and_drain();
                context_->publisher()->delete_datawriter(request_writer_);
            }
        } catch (...) {
            listener_detached = false;
        }
        if (!listener_detached) {
            impl::ProcessLifetime::instance().retain_writer_listener(std::move(request_listener_));
        }
        request_writer_ = nullptr;
    }
}

Result<RequestId> Client::Impl::write_request(const void* request) {
    if (request == nullptr)
        return Result<RequestId>::failure(
            Error(ErrorCode::InvalidArgument, "Request must not be null"));
    const auto operation = context_->try_acquire_operation();
    if (!operation)
        return Result<RequestId>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    eprosima::fastrtps::rtps::WriteParams params;
    // ROS 2 Fast DDS service requests carry the response reader GUID in the
    // related identity.  It lets the Server correlate a response with the
    // exact reader instead of merely with the Client's request writer.
    params.related_sample_identity().writer_guid() = response_reader_->guid();
    if (!request_writer_->write(const_cast<void*>(request), params))
        return Result<RequestId>::failure(
            Error(ErrorCode::DDSError, "Fast DDS request write failed"));
    auto request_id = impl::to_request_id(params.sample_identity());
    if (!request_id) {
        return Result<RequestId>::failure(
            Error(ErrorCode::DDSError, "Fast DDS request write returned an unknown sequence"));
    }
    request_id->client_gid = impl::to_gid(response_reader_->guid());
    return Result<RequestId>::success(*request_id);
}

Result<bool> Client::Impl::read_response(void* response, RequestId& request_id) {
    if (response == nullptr)
        return Result<bool>::failure(
            Error(ErrorCode::InvalidArgument, "Response must not be null"));
    const auto operation = context_->try_acquire_operation();
    if (!operation)
        return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    std::lock_guard read_lock(response_read_mutex_);
    auto remaining = response_reader_->get_unread_count();
    while (remaining-- != 0U) {
        if (!response_scratch_) {
            auto sample = impl::TemporarySample::create(response_type_);
            if (!sample) return Result<bool>::failure(std::move(sample.error()));
            response_scratch_ = std::make_unique<impl::TemporarySample>(std::move(sample.value()));
        }
        eprosima::fastdds::dds::SampleInfo info;
        const auto result = response_reader_->take_next_sample(response_scratch_->data(), &info);
        if (result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_NO_DATA)
            return Result<bool>::success(false);
        if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK)
            return Result<bool>::failure(impl::to_error(result, "Fast DDS response take failed"));
        if (!info.valid_data) continue;
        const auto& related_guid = info.related_sample_identity.writer_guid();
        if (related_guid != request_writer_->guid() && related_guid != response_reader_->guid()) {
            continue;
        }
        const auto response_id = impl::to_request_id(info.related_sample_identity);
        if (!response_id) continue;
        auto committed = response_scratch_->commit_to(response);
        if (!committed) {
            return Result<bool>::failure(std::move(committed.error()));
        }
        request_id = *response_id;
        return Result<bool>::success(true);
    }
    return Result<bool>::success(false);
}

Result<bool> Client::Impl::service_is_available() const {
    const auto operation = context_->try_acquire_operation();
    if (!operation)
        return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));

    if (request_state_->is_degraded()) {
        return Result<bool>::failure(
            Error(ErrorCode::DDSError, "Service discovery context is unavailable"));
    }
    return Result<bool>::success(request_state_->is_available());
}

Result<Qos> Client::Impl::request_actual_qos() const {
    const auto operation = context_->try_acquire_operation();
    if (!operation)
        return Result<Qos>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    eprosima::fastdds::dds::DataWriterQos qos;
    const auto result = request_writer_->get_qos(qos);
    if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
        return Result<Qos>::failure(
            impl::to_error(result, "Fast DDS client request writer QoS query failed"));
    }
    return impl::from_neutral_qos(qos);
}

Result<Qos> Client::Impl::response_actual_qos() const {
    const auto operation = context_->try_acquire_operation();
    if (!operation)
        return Result<Qos>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    eprosima::fastdds::dds::DataReaderQos qos;
    const auto result = response_reader_->get_qos(qos);
    if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
        return Result<Qos>::failure(
            impl::to_error(result, "Fast DDS client response reader QoS query failed"));
    }
    return impl::from_neutral_qos(qos);
}

Result<bool> Client::Impl::wait_for_service(WaitTimeout timeout) const {
    const auto operation = context_->try_acquire_operation();
    if (!operation)
        return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));

    const auto deadline = timeout.kind() == WaitTimeout::Kind::Finite
                              ? std::chrono::steady_clock::now() + timeout.duration()
                              : std::chrono::steady_clock::time_point::max();
    auto state = service_wait_state_;
    std::unique_lock lock(state->mutex);
    while (true) {
        const auto observed_revision = state->revision.load(std::memory_order_acquire);
        auto available = service_is_available();
        if (!available || available.value() || timeout.kind() == WaitTimeout::Kind::Poll)
            return available;
        if (context_->is_shutdown()) {
            return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
        }
        if (timeout.kind() == WaitTimeout::Kind::Finite) {
            if (std::chrono::steady_clock::now() >= deadline) return Result<bool>::success(false);
            state->cv.wait_until(lock, deadline, [&] {
                return context_->is_shutdown() ||
                       state->revision.load(std::memory_order_acquire) != observed_revision;
            });
        } else {
            state->cv.wait(lock, [&] {
                return context_->is_shutdown() ||
                       state->revision.load(std::memory_order_acquire) != observed_revision;
            });
        }
    }
}

}  // namespace dmw
