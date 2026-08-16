#include "impl/client_impl.hpp"

#include <memory>
#include <new>
#include <string_view>
#include <utility>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/rtps/common/WriteParams.h>

#include "dmw/error.hpp"
#include "impl/fastdds/identity.hpp"
#include "impl/fastdds/process_runtime.hpp"
#include "impl/fastdds/return_code.hpp"
#include "impl/client_impl.hpp"
#include "impl/temporary_sample.hpp"

namespace dmw {

Client::Impl::~Impl() noexcept {
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
                state_->subscriber()->delete_datareader(response_reader_);
            } catch (...) {
                // The Context container remains the conservative ownership barrier.
            }
        }
        if (!listener_detached) {
            impl::fastdds::DmwProcessRuntime::instance().retain_reader_listener(
                std::move(response_listener_));
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
                state_->publisher()->delete_datawriter(request_writer_);
            }
        } catch (...) {
            listener_detached = false;
        }
        if (!listener_detached) {
            impl::fastdds::DmwProcessRuntime::instance().retain_writer_listener(
                std::move(request_listener_));
        }
        request_writer_ = nullptr;
    }
}

Result<RequestId> Client::Impl::send_request(const void* request) {
    if (request == nullptr)
        return Result<RequestId>::failure(
            Error(ErrorCode::InvalidArgument, "Request must not be null"));
    const auto operation = state_->try_acquire_operation();
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
            Error(ErrorCode::DdsError, "Fast DDS request write failed"));
    auto request_id = impl::fastdds::request_id_from_identity(params.sample_identity());
    if (!request_id) {
        return Result<RequestId>::failure(
            Error(ErrorCode::DdsError, "Fast DDS request write returned an unknown sequence"));
    }
    request_id->client_gid = impl::fastdds::to_gid(response_reader_->guid());
    return Result<RequestId>::success(*request_id);
}

Result<TakeStatus> Client::Impl::take_response(void* response, RequestId& request_id) {
    if (response == nullptr)
        return Result<TakeStatus>::failure(
            Error(ErrorCode::InvalidArgument, "Response must not be null"));
    const auto operation = state_->try_acquire_operation();
    if (!operation)
        return Result<TakeStatus>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    auto remaining = response_reader_->get_unread_count();
    while (remaining-- != 0U) {
        auto sample = impl::TemporarySample::create(response_type_);
        if (!sample) {
            return Result<TakeStatus>::failure(std::move(sample.error()));
        }
        eprosima::fastdds::dds::SampleInfo info;
        const auto result = response_reader_->take_next_sample(sample.value().data(), &info);
        if (result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_NO_DATA)
            return Result<TakeStatus>::success(TakeStatus::NoData);
        if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK)
            return Result<TakeStatus>::failure(
                impl::fastdds::to_error(result, "Fast DDS response take failed"));
        if (!info.valid_data) continue;
        const auto& related_guid = info.related_sample_identity.writer_guid();
        if (related_guid != request_writer_->guid() && related_guid != response_reader_->guid()) {
            continue;
        }
        const auto response_id =
            impl::fastdds::request_id_from_identity(info.related_sample_identity);
        if (!response_id) continue;
        auto committed = sample.value().commit_to(response);
        if (!committed) {
            return Result<TakeStatus>::failure(std::move(committed.error()));
        }
        request_id = *response_id;
        return Result<TakeStatus>::success(TakeStatus::Taken);
    }
    return Result<TakeStatus>::success(TakeStatus::NoData);
}

Result<bool> Client::Impl::service_is_available() const {
    const auto operation = state_->try_acquire_operation();
    if (!operation)
        return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));

    if (match_state_->is_degraded()) {
        return Result<bool>::failure(
            Error(ErrorCode::DdsError, "Service discovery state is unavailable"));
    }
    try {
        return Result<bool>::success(match_state_->has_candidate());
    } catch (const std::bad_alloc&) {
        // The service state is snapshotted before lower-ranked discovery
        // registries are inspected.  Keep allocation failure inside Result.
        return Result<bool>::failure(
            Error(ErrorCode::ResourceExhausted, "Service availability snapshot allocation failed"));
    }
}

}  // namespace dmw
