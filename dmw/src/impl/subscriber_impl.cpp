#include "impl/subscriber_impl.hpp"

#include <cstdint>
#include <limits>
#include <vector>

#include <fastdds/dds/subscriber/SampleInfo.hpp>

#include "dmw/error.hpp"
#include "impl/event_impl.hpp"
#include "impl/subscriber_impl.hpp"
#include "impl/identity.hpp"
#include "impl/process_lifetime.hpp"
#include "impl/return_code.hpp"
#include "impl/temporary_sample.hpp"

namespace dmw {

#define impl_ this

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
                context_->subscriber()->delete_datareader(reader_);
            } catch (...) {
                // See Publisher::Impl::~Impl(): a Context container is the
                // final ownership barrier when individual deletion fails.
            }
        }
        reader_ = nullptr;
    }
}

Result<bool> Subscriber::Impl::read(void* message, MessageInfo& info) {
    if (message == nullptr)
        return Result<bool>::failure(Error(ErrorCode::InvalidArgument, "Message must not be null"));
    const auto operation = context_->try_acquire_operation();
    if (!operation)
        return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    auto remaining = reader_->get_unread_count();
    while (remaining-- != 0U) {
        auto sample = impl::TemporarySample::create(type_);
        if (!sample) return Result<bool>::failure(std::move(sample.error()));
        eprosima::fastdds::dds::SampleInfo sample_info;
        const auto result = reader_->take_next_sample(sample.value().data(), &sample_info);
        if (result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_NO_DATA)
            return Result<bool>::success(false);
        if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK)
            return Result<bool>::failure(impl::to_error(result, "Fast DDS take failed"));
        if (!sample_info.valid_data) continue;

        auto committed = sample.value().commit_to(message);
        if (!committed) return Result<bool>::failure(std::move(committed.error()));

        MessageInfo updated;
        updated.writer_timestamp = to_nanoseconds(sample_info.source_timestamp);
        updated.reader_timestamp = to_nanoseconds(sample_info.reception_timestamp);
        if (sample_info.sample_identity.writer_guid() != eprosima::fastrtps::rtps::c_Guid_Unknown) {
            updated.writer_gid = impl::to_gid(sample_info.sample_identity.writer_guid());
        } else if (sample_info.publication_handle.isDefined()) {
            updated.writer_gid = impl::to_gid(
                eprosima::fastrtps::rtps::iHandle2GUID(sample_info.publication_handle));
        }
        updated.to_writer_sequence =
            impl::to_writer_sequence(sample_info.sample_identity.sequence_number());
        info = updated;
        return Result<bool>::success(true);
    }
    return Result<bool>::success(false);
}

Result<std::size_t> Subscriber::Impl::matched_publisher_count() const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<std::size_t>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    eprosima::fastdds::dds::SubscriptionMatchedStatus status;
    const auto result = reader_->get_subscription_matched_status(status);
    if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
        return Result<std::size_t>::failure(
            impl::to_error(result, "Fast DDS matched publication query failed"));
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(status.current_count));
}

Result<std::unique_ptr<Event>> Subscriber::Impl::create_event(EventType type) {
    if (type != EventType::LivelinessChanged && type != EventType::RequestedDeadlineMissed &&
        type != EventType::RequestedIncompatibleQos && type != EventType::MessageLost) {
        return Result<std::unique_ptr<Event>>::failure(
            Error(ErrorCode::InvalidArgument, "EventType is not valid for Subscriber"));
    }
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<Event>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    if (!event_parent_->alive.load(std::memory_order_acquire)) {
        return Result<std::unique_ptr<Event>>::failure(
            Error(ErrorCode::ParentDestroyed, "Subscriber is destroyed"));
    }
    auto wait_state = std::make_shared<GuardConditionState>(context_);
    auto attached = event_parent_->attach(*reader_);
    if (!attached) return Result<std::unique_ptr<Event>>::failure(std::move(attached.error()));
    const auto cursor = event_parent_->snapshot(type);
    const auto registration_id = event_parent_->register_event(type, wait_state);
    if (registration_id == 0) {
        return Result<std::unique_ptr<Event>>::failure(
            Error(ErrorCode::ResourceExhausted, "Event registration IDs are exhausted"));
    }
    std::unique_ptr<Event::Impl> event_impl;
    try {
        event_impl = std::make_unique<Event::Impl>(
            event_parent_, type, std::move(wait_state), cursor, registration_id);
    } catch (...) {
        event_parent_->unregister_event(registration_id);
        throw;
    }
    return Result<std::unique_ptr<Event>>::success(
        std::unique_ptr<Event>(new Event(std::move(event_impl))));
}

#undef impl_
}  // namespace dmw
