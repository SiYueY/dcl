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
#include "impl/fastdds/process_runtime.hpp"
#include "impl/fastdds/return_code.hpp"
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
        bool listener_detached = false;
        try {
            listener_detached = response_writer_->set_listener(nullptr) ==
                                eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
            if (listener_detached) response_match_listener_->close_and_drain();
        } catch (...) {
            listener_detached = false;
        }
        if (listener_detached) {
            try {
            state_->publisher()->delete_datawriter(response_writer_);
            } catch (...) {
                // The Context container remains the conservative ownership barrier.
            }
        } else {
            impl::fastdds::DmwProcessRuntime::instance().retain_writer_listener(
                std::move(response_match_listener_));
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
        bool became_full = false;
        if (!impl_->reserve_request_slot(became_full))
            return Result<TakeStatus>::failure(
                Error(ErrorCode::ResourceExhausted, "Server pending request capacity is exhausted"));
        if (became_full) impl_->request_wait_state_->set_blocking_enabled(false);

        auto sample = impl::TemporarySample::create(impl_->request_type_);
        if (!sample) {
            if (impl_->release_request_reservation())
                impl_->request_wait_state_->set_blocking_enabled(true);
            return Result<TakeStatus>::failure(std::move(sample.error()));
        }
        eprosima::fastdds::dds::SampleInfo info;
        const auto result = impl_->request_reader_->take_next_sample(sample.value().data(), &info);
        if (result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_NO_DATA) {
            if (impl_->release_request_reservation())
                impl_->request_wait_state_->set_blocking_enabled(true);
            return Result<TakeStatus>::success(TakeStatus::NoData);
        }
        if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
            if (impl_->release_request_reservation())
                impl_->request_wait_state_->set_blocking_enabled(true);
            return Result<TakeStatus>::failure(
                impl::fastdds::return_code_error(result, "Fast DDS request take failed"));
        }
        if (!info.valid_data) {
            if (impl_->release_request_reservation())
                impl_->request_wait_state_->set_blocking_enabled(true);
            continue;
        }

        auto id = impl::fastdds::request_id_from_identity(info.sample_identity);
        if (!id) {
            if (impl_->release_request_reservation())
                impl_->request_wait_state_->set_blocking_enabled(true);
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
            std::lock_guard lock(impl_->pending_mutex_);
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
            if (impl_->release_pending_request(*id))
                impl_->request_wait_state_->set_blocking_enabled(true);
            return Result<TakeStatus>::failure(std::move(committed.error()));
        } catch (...) {
            if (impl_->release_pending_request(*id))
                impl_->request_wait_state_->set_blocking_enabled(true);
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
        std::lock_guard lock(impl_->pending_mutex_);
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

    const bool has_target_reader = is_reader_guid(sample_identity.writer_guid());
    const auto response_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    if (has_target_reader) {
        impl::ResponseWriterMatchState::WaitStatus target;
        try {
            target = impl_->response_match_state_->wait_for_match(
                sample_identity.writer_guid(), response_deadline);
        } catch (...) {
            std::lock_guard lock(impl_->pending_mutex_);
            const auto pending = impl_->pending_.find(request_id);
            if (pending != impl_->pending_.end())
                pending->second.phase = Impl::PendingPhase::Pending;
            throw;
        }
        if (target != impl::ResponseWriterMatchState::WaitStatus::Matched) {
            if (target == impl::ResponseWriterMatchState::WaitStatus::Removed) {
                if (impl_->release_pending_request(request_id))
                    impl_->request_wait_state_->set_blocking_enabled(true);
                return Result<void>::success();
            }
            std::lock_guard lock(impl_->pending_mutex_);
            const auto pending = impl_->pending_.find(request_id);
            if (pending != impl_->pending_.end()) pending->second.phase = Impl::PendingPhase::Pending;
            if (target == impl::ResponseWriterMatchState::WaitStatus::Degraded) {
                return Result<void>::failure(
                    Error(ErrorCode::DdsError, "Response reader discovery state is unavailable"));
            }
            return Result<void>::failure(
                Error(ErrorCode::Timeout, "Response reader did not match before the deadline"));
        }
    }

    // Commit-point recheck: an operation guard only prevents destruction, not
    // transition to Context shutdown or a terminal discovery update while the
    // response target wait was in progress.
    if (impl_->state_->is_shutdown()) {
        std::lock_guard lock(impl_->pending_mutex_);
        const auto pending = impl_->pending_.find(request_id);
        if (pending != impl_->pending_.end()) pending->second.phase = Impl::PendingPhase::Pending;
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    if (has_target_reader) {
        auto final_target = impl_->response_match_state_->check_target(sample_identity.writer_guid());
        if (final_target == impl::ResponseWriterMatchState::WaitStatus::Timeout) {
            try {
                // A match may disappear transiently between the first wait
                // and the write commit.  Continue only until the original
                // absolute deadline; never start a second 100 ms interval.
                final_target = impl_->response_match_state_->wait_for_match(
                    sample_identity.writer_guid(), response_deadline);
            } catch (...) {
                std::lock_guard lock(impl_->pending_mutex_);
                const auto pending = impl_->pending_.find(request_id);
                if (pending != impl_->pending_.end()) pending->second.phase = Impl::PendingPhase::Pending;
                throw;
            }
        }
        if (final_target == impl::ResponseWriterMatchState::WaitStatus::Removed) {
            if (impl_->release_pending_request(request_id))
                impl_->request_wait_state_->set_blocking_enabled(true);
            return Result<void>::success();
        }
        if (final_target != impl::ResponseWriterMatchState::WaitStatus::Matched) {
            std::lock_guard lock(impl_->pending_mutex_);
            const auto pending = impl_->pending_.find(request_id);
            if (pending != impl_->pending_.end()) pending->second.phase = Impl::PendingPhase::Pending;
            if (final_target == impl::ResponseWriterMatchState::WaitStatus::Degraded) {
                return Result<void>::failure(
                    Error(ErrorCode::DdsError, "Response reader discovery state is unavailable"));
            }
            return Result<void>::failure(
                Error(ErrorCode::Timeout, "Response reader lost its match before write"));
        }
    }

    eprosima::fastrtps::rtps::WriteParams params;
    params.related_sample_identity(sample_identity);
    try {
        if (impl_->response_writer_->write(const_cast<void*>(response), params)) {
            if (impl_->release_pending_request(request_id))
                impl_->request_wait_state_->set_blocking_enabled(true);
            return Result<void>::success();
        }
        std::lock_guard lock(impl_->pending_mutex_);
        const auto pending = impl_->pending_.find(request_id);
        if (pending != impl_->pending_.end()) pending->second.phase = Impl::PendingPhase::Pending;
        return Result<void>::failure(Error(ErrorCode::DdsError, "Fast DDS response write failed"));
    } catch (...) {
        std::lock_guard lock(impl_->pending_mutex_);
        const auto pending = impl_->pending_.find(request_id);
        if (pending != impl_->pending_.end()) pending->second.phase = Impl::PendingPhase::Pending;
        throw;
    }
}

}  // namespace dmw
