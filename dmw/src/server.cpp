// SPDX-License-Identifier: Apache-2.0

#include "dmw/server.hpp"

#include <chrono>
#include <cstdint>
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

namespace {

bool is_reader_guid(const eprosima::fastrtps::rtps::GUID_t& guid) noexcept {
    constexpr std::uint8_t kReaderEntityIdMask = 0x04U;
    return guid != eprosima::fastrtps::rtps::GUID_t::unknown() &&
           (guid.entityId.value[3] & kReaderEntityIdMask) != 0U;
}

}  // namespace

Server::Impl::~Impl() noexcept {
    if (response_writer_ != nullptr) {
        try {
            response_writer_->set_listener(nullptr);
            state_->publisher()->delete_datawriter(response_writer_);
        } catch (...) {
            // The Context container remains the conservative ownership barrier.
        }
        response_writer_ = nullptr;
    }
    if (request_reader_ != nullptr) {
        try {
            if (request_wait_state_->close()) {
                state_->subscriber()->delete_datareader(request_reader_);
            }
        } catch (...) {
            // The Context container remains the conservative ownership barrier.
        }
        request_reader_ = nullptr;
    }
}

Server::Server(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Server::~Server() noexcept = default;

std::string_view Server::service_name() const noexcept { return impl_->service_name_; }

Result<TakeStatus> Server::take_request(void* request, RequestId& request_id) {
    if (request == nullptr)
        return Result<TakeStatus>::failure(
            Error(ErrorCode::InvalidArgument, "Request must not be null"));
    const auto operation = impl_->state_->try_acquire_operation();
    if (!operation)
        return Result<TakeStatus>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    auto remaining = impl_->request_reader_->get_unread_count();
    while (remaining-- != 0U) {
        {
            std::lock_guard<std::mutex> lock(impl_->pending_mutex_);
            if (impl_->pending_.size() + impl_->reservations_ >= impl_->max_pending_requests_)
                return Result<TakeStatus>::failure(Error(
                    ErrorCode::ResourceExhausted, "Server pending request capacity is exhausted"));
            ++impl_->reservations_;
        }

        auto sample = impl::TemporarySample::create(impl_->request_type_);
        if (!sample) {
            std::lock_guard<std::mutex> lock(impl_->pending_mutex_);
            --impl_->reservations_;
            return Result<TakeStatus>::failure(std::move(sample.error()));
        }
        eprosima::fastdds::dds::SampleInfo info;
        const auto result = impl_->request_reader_->take_next_sample(sample.value().data(), &info);
        if (result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_NO_DATA) {
            std::lock_guard<std::mutex> lock(impl_->pending_mutex_);
            --impl_->reservations_;
            return Result<TakeStatus>::success(TakeStatus::NoData);
        }
        if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
            std::lock_guard<std::mutex> lock(impl_->pending_mutex_);
            --impl_->reservations_;
            return Result<TakeStatus>::failure(
                Error(ErrorCode::DdsError, "Fast DDS request take failed"));
        }
        if (!info.valid_data) {
            std::lock_guard<std::mutex> lock(impl_->pending_mutex_);
            --impl_->reservations_;
            continue;
        }

        auto id = impl::fastdds::request_id_from_identity(info.sample_identity);
        if (!id) {
            std::lock_guard<std::mutex> lock(impl_->pending_mutex_);
            --impl_->reservations_;
            return Result<TakeStatus>::failure(
                Error(ErrorCode::DdsError, "Fast DDS request has an unknown sequence"));
        }
        auto response_identity = info.sample_identity;
        const auto& response_reader_guid = info.related_sample_identity.writer_guid();
        if (response_reader_guid != eprosima::fastrtps::rtps::GUID_t::unknown()) {
            id->client_gid = impl::fastdds::to_gid(response_reader_guid);
            response_identity.writer_guid() = response_reader_guid;
        }

        bool inserted = false;
        {
            std::lock_guard<std::mutex> lock(impl_->pending_mutex_);
            try {
                inserted =
                    impl_->pending_
                        .emplace(
                            *id,
                            Impl::PendingRequest{response_identity, Impl::PendingPhase::Pending})
                        .second;
            } catch (...) {
                --impl_->reservations_;
                throw;
            }
            --impl_->reservations_;
        }
        if (!inserted) continue;

        try {
            auto committed = sample.value().commit_to(request);
            if (committed) {
                request_id = *id;
                return Result<TakeStatus>::success(TakeStatus::Taken);
            }
            std::lock_guard<std::mutex> lock(impl_->pending_mutex_);
            impl_->pending_.erase(*id);
            return Result<TakeStatus>::failure(std::move(committed.error()));
        } catch (...) {
            std::lock_guard<std::mutex> lock(impl_->pending_mutex_);
            impl_->pending_.erase(*id);
            throw;
        }
    }
    return Result<TakeStatus>::success(TakeStatus::NoData);
}

Result<void> Server::send_response(const RequestId& request_id, const void* response) {
    if (response == nullptr)
        return Result<void>::failure(
            Error(ErrorCode::InvalidArgument, "Response must not be null"));
    const auto operation = impl_->state_->try_acquire_operation();
    if (!operation)
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    eprosima::fastrtps::rtps::SampleIdentity sample_identity;
    {
        std::lock_guard<std::mutex> lock(impl_->pending_mutex_);
        const auto pending = impl_->pending_.find(request_id);
        if (pending == impl_->pending_.end())
            return Result<void>::failure(
                Error(ErrorCode::NotFound, "Request is not pending on this Server"));
        if (pending->second.phase == Impl::PendingPhase::Responding)
            return Result<void>::failure(
                Error(ErrorCode::Busy, "A response for this request is already in progress"));
        pending->second.phase = Impl::PendingPhase::Responding;
        sample_identity = pending->second.sample_identity;
    }

    if (is_reader_guid(sample_identity.writer_guid())) {
        impl::ResponseWriterMatchState::WaitStatus target;
        try {
            target = impl_->response_match_state_->wait_for_match(
                sample_identity.writer_guid(),
                std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
        } catch (...) {
            std::lock_guard<std::mutex> lock(impl_->pending_mutex_);
            const auto pending = impl_->pending_.find(request_id);
            if (pending != impl_->pending_.end())
                pending->second.phase = Impl::PendingPhase::Pending;
            throw;
        }
        if (target != impl::ResponseWriterMatchState::WaitStatus::Matched) {
            std::lock_guard<std::mutex> lock(impl_->pending_mutex_);
            const auto pending = impl_->pending_.find(request_id);
            if (pending != impl_->pending_.end())
                pending->second.phase = Impl::PendingPhase::Pending;
            if (target == impl::ResponseWriterMatchState::WaitStatus::Degraded) {
                return Result<void>::failure(
                    Error(ErrorCode::DdsError, "Response reader discovery state is unavailable"));
            }
            return Result<void>::failure(
                Error(ErrorCode::Timeout, "Response reader did not match before the deadline"));
        }
    }

    eprosima::fastrtps::rtps::WriteParams params;
    params.related_sample_identity(sample_identity);
    try {
        if (impl_->response_writer_->write(const_cast<void*>(response), params)) {
            std::lock_guard<std::mutex> lock(impl_->pending_mutex_);
            const auto pending = impl_->pending_.find(request_id);
            if (pending != impl_->pending_.end()) impl_->pending_.erase(pending);
            return Result<void>::success();
        }
        std::lock_guard<std::mutex> lock(impl_->pending_mutex_);
        const auto pending = impl_->pending_.find(request_id);
        if (pending != impl_->pending_.end()) pending->second.phase = Impl::PendingPhase::Pending;
        return Result<void>::failure(Error(ErrorCode::DdsError, "Fast DDS response write failed"));
    } catch (...) {
        std::lock_guard<std::mutex> lock(impl_->pending_mutex_);
        const auto pending = impl_->pending_.find(request_id);
        if (pending != impl_->pending_.end()) pending->second.phase = Impl::PendingPhase::Pending;
        throw;
    }
}

}  // namespace dmw
