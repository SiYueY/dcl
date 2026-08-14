// SPDX-License-Identifier: Apache-2.0

#include "dmw/subscriber.hpp"

#include <cstdint>
#include <limits>
#include <vector>

#include <fastdds/dds/subscriber/SampleInfo.hpp>

#include "dmw/error.hpp"
#include "impl/event_impl.hpp"
#include "impl/endpoint_impl.hpp"
#include "impl/fastdds/identity.hpp"
#include "impl/fastdds/process_runtime.hpp"
#include "impl/fastdds/return_code.hpp"
#include "impl/temporary_sample.hpp"

namespace dmw {

namespace {

std::int64_t to_nanoseconds(const eprosima::fastrtps::rtps::Time_t& time) noexcept {
    constexpr std::int64_t kNanosecondsPerSecond = 1000000000LL;
    if (time.seconds() < 0 ||
        time.seconds() > std::numeric_limits<std::int64_t>::max() / kNanosecondsPerSecond) {
        return 0;
    }
    return static_cast<std::int64_t>(time.seconds()) * kNanosecondsPerSecond + time.nanosec();
}

}  // namespace

Subscriber::Impl::~Impl() noexcept {
    event_parent_->close();
    if (reader_ != nullptr) {
        bool listener_detached = false;
        try {
            listener_detached = reader_->set_listener(nullptr) ==
                                eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
            if (listener_detached) event_parent_->drain_listeners();
        } catch (...) {
            listener_detached = false;
        }
        if (!listener_detached) event_parent_->quarantine_listeners();
        const bool reader_closed = wait_state_->close();
        if (listener_detached && reader_closed) {
            try {
                state_->subscriber()->delete_datareader(reader_);
            } catch (...) {
                // See Publisher::Impl::~Impl(): a Context container is the
                // final ownership barrier when individual deletion fails.
            }
        }
        reader_ = nullptr;
    }
}

Subscriber::Subscriber(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Subscriber::~Subscriber() noexcept = default;

Result<TakeStatus> Subscriber::take(void* message, MessageInfo& info) {
    if (message == nullptr)
        return Result<TakeStatus>::failure(
            Error(ErrorCode::InvalidArgument, "Message must not be null"));
    const auto operation = impl_->state_->try_acquire_operation();
    if (!operation)
        return Result<TakeStatus>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    auto remaining = impl_->reader_->get_unread_count();
    while (remaining-- != 0U) {
        auto sample = impl::TemporarySample::create(impl_->type_);
        if (!sample) return Result<TakeStatus>::failure(std::move(sample.error()));
        eprosima::fastdds::dds::SampleInfo sample_info;
        const auto result = impl_->reader_->take_next_sample(sample.value().data(), &sample_info);
        if (result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_NO_DATA)
            return Result<TakeStatus>::success(TakeStatus::NoData);
        if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK)
            return Result<TakeStatus>::failure(
                impl::fastdds::return_code_error(result, "Fast DDS take failed"));
        if (!sample_info.valid_data) continue;

        auto committed = sample.value().commit_to(message);
        if (!committed) return Result<TakeStatus>::failure(std::move(committed.error()));

        MessageInfo updated;
        updated.source_timestamp_ns = to_nanoseconds(sample_info.source_timestamp);
        updated.received_timestamp_ns = to_nanoseconds(sample_info.reception_timestamp);
        if (sample_info.sample_identity.writer_guid() != eprosima::fastrtps::rtps::c_Guid_Unknown) {
            updated.publisher_gid =
                impl::fastdds::to_gid(sample_info.sample_identity.writer_guid());
        } else if (sample_info.publication_handle.isDefined()) {
            updated.publisher_gid = impl::fastdds::to_gid(
                eprosima::fastrtps::rtps::iHandle2GUID(sample_info.publication_handle));
        }
        updated.publication_sequence_number = impl::fastdds::publication_sequence_number(
            sample_info.sample_identity.sequence_number());
        info = updated;
        return Result<TakeStatus>::success(TakeStatus::Taken);
    }
    return Result<TakeStatus>::success(TakeStatus::NoData);
}

std::string_view Subscriber::topic_name() const noexcept { return impl_->topic_name_; }
const MessageType& Subscriber::message_type() const noexcept { return impl_->type_; }

Result<std::size_t> Subscriber::matched_publisher_count() const {
    const auto operation = impl_->state_->try_acquire_operation();
    if (!operation) {
        return Result<std::size_t>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    eprosima::fastdds::dds::SubscriptionMatchedStatus status;
    const auto result = impl_->reader_->get_subscription_matched_status(status);
    if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
        return Result<std::size_t>::failure(
            impl::fastdds::return_code_error(result, "Fast DDS matched publication query failed"));
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(status.current_count));
}

Result<std::unique_ptr<Event>> Subscriber::create_event(EventType type) {
    if (type != EventType::LivelinessChanged && type != EventType::RequestedDeadlineMissed &&
        type != EventType::RequestedIncompatibleQos && type != EventType::MessageLost) {
        return Result<std::unique_ptr<Event>>::failure(
            Error(ErrorCode::InvalidArgument, "EventType is not valid for Subscriber"));
    }
    const auto operation = impl_->state_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<Event>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    if (!impl_->event_parent_->alive.load(std::memory_order_acquire)) {
        return Result<std::unique_ptr<Event>>::failure(
            Error(ErrorCode::ParentDestroyed, "Subscriber is destroyed"));
    }
    auto wait_state = std::make_shared<GuardConditionState>(impl_->state_);
    auto attached = impl_->event_parent_->attach(*impl_->reader_);
    if (!attached) return Result<std::unique_ptr<Event>>::failure(std::move(attached.error()));
    const auto cursor = impl_->event_parent_->snapshot(type);
    const auto registration_id = impl_->event_parent_->register_event(type, wait_state);
    if (registration_id == 0) {
        return Result<std::unique_ptr<Event>>::failure(
            Error(ErrorCode::ResourceExhausted, "Event registration IDs are exhausted"));
    }
    std::unique_ptr<Event::Impl> event_impl;
    try {
        event_impl = std::make_unique<Event::Impl>(
            impl_->event_parent_, type, std::move(wait_state), cursor, registration_id);
    } catch (...) {
        impl_->event_parent_->unregister_event(registration_id);
        throw;
    }
    return Result<std::unique_ptr<Event>>::success(
        std::unique_ptr<Event>(new Event(std::move(event_impl))));
}

}  // namespace dmw
