// SPDX-License-Identifier: Apache-2.0

#include "dmw/client.hpp"

#include <memory>
#include <string_view>
#include <utility>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/rtps/common/WriteParams.h>

#include "dmw/error.hpp"
#include "impl/fastdds/identity.hpp"
#include "impl/service_impl.hpp"
#include "impl/temporary_sample.hpp"

namespace dmw {

Client::Impl::~Impl() noexcept {
    if (response_reader_ != nullptr) {
        try {
            response_reader_->set_listener(nullptr);
        } catch (...) {
            // WaitSet detachment remains required even when listener removal
            // is not confirmed.
        }
        if (response_wait_state_->close()) {
            try {
                state_->subscriber()->delete_datareader(response_reader_);
            } catch (...) {
                // The Context container remains the conservative ownership barrier.
            }
        }
        response_reader_ = nullptr;
    }
    if (request_writer_ != nullptr) {
        try {
            request_writer_->set_listener(nullptr);
            state_->publisher()->delete_datawriter(request_writer_);
        } catch (...) {
            // The Context container remains the conservative ownership barrier.
        }
        request_writer_ = nullptr;
    }
}

Client::Client(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Client::~Client() noexcept = default;

std::string_view Client::service_name() const noexcept { return impl_->service_name_; }

Result<RequestId> Client::send_request(const void* request) {
    if (request == nullptr)
        return Result<RequestId>::failure(
            Error(ErrorCode::InvalidArgument, "Request must not be null"));
    const auto operation = impl_->state_->try_acquire_operation();
    if (!operation)
        return Result<RequestId>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    eprosima::fastrtps::rtps::WriteParams params;
    // ROS 2 Fast DDS service requests carry the response reader GUID in the
    // related identity.  It lets the Server correlate a response with the
    // exact reader instead of merely with the Client's request writer.
    params.related_sample_identity().writer_guid() = impl_->response_reader_->guid();
    if (!impl_->request_writer_->write(const_cast<void*>(request), params))
        return Result<RequestId>::failure(
            Error(ErrorCode::DdsError, "Fast DDS request write failed"));
    auto request_id = impl::fastdds::request_id_from_identity(params.sample_identity());
    if (!request_id) {
        return Result<RequestId>::failure(
            Error(ErrorCode::DdsError, "Fast DDS request write returned an unknown sequence"));
    }
    request_id->client_gid = impl::fastdds::to_gid(impl_->response_reader_->guid());
    return Result<RequestId>::success(*request_id);
}

Result<TakeStatus> Client::take_response(void* response, RequestId& request_id) {
    if (response == nullptr)
        return Result<TakeStatus>::failure(
            Error(ErrorCode::InvalidArgument, "Response must not be null"));
    const auto operation = impl_->state_->try_acquire_operation();
    if (!operation)
        return Result<TakeStatus>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    auto remaining = impl_->response_reader_->get_unread_count();
    while (remaining-- != 0U) {
        auto sample = impl::TemporarySample::create(impl_->response_type_);
        if (!sample) {
            return Result<TakeStatus>::failure(std::move(sample.error()));
        }
        eprosima::fastdds::dds::SampleInfo info;
        const auto result = impl_->response_reader_->take_next_sample(sample.value().data(), &info);
        if (result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_NO_DATA)
            return Result<TakeStatus>::success(TakeStatus::NoData);
        if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK)
            return Result<TakeStatus>::failure(
                Error(ErrorCode::DdsError, "Fast DDS response take failed"));
        if (!info.valid_data) continue;
        const auto& related_guid = info.related_sample_identity.writer_guid();
        if (related_guid != impl_->request_writer_->guid() &&
            related_guid != impl_->response_reader_->guid()) {
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

Result<bool> Client::service_is_available() const {
    const auto operation = impl_->state_->try_acquire_operation();
    if (!operation)
        return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));

    if (impl_->match_state_->is_degraded()) {
        return Result<bool>::failure(
            Error(ErrorCode::DdsError, "Service discovery state is unavailable"));
    }
    return Result<bool>::success(impl_->match_state_->has_candidate());
}

}  // namespace dmw
