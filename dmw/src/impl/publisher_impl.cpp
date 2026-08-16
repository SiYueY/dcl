#include "impl/publisher_impl.hpp"

#include "dmw/error.hpp"
#include "impl/event_impl.hpp"
#include "impl/publisher_impl.hpp"
#include "impl/fastdds/process_runtime.hpp"
#include "impl/fastdds/return_code.hpp"

namespace dmw {

#define impl_ this

Publisher::Impl::~Impl() noexcept {
    event_parent_->close();
    if (writer_ != nullptr) {
        bool listener_detached = false;
        try {
            listener_detached = writer_->set_listener(nullptr) ==
                                eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
            if (listener_detached) {
                event_parent_->drain_listeners();
                state_->publisher()->delete_datawriter(writer_);
            }
        } catch (...) {
            listener_detached = false;
        }
        if (!listener_detached) {
            event_parent_->quarantine_listeners();
        }
        writer_ = nullptr;
    }
}

Result<void> Publisher::Impl::publish(const void* message) {
    if (message == nullptr)
        return Result<void>::failure(Error(ErrorCode::InvalidArgument, "Message must not be null"));
    const auto operation = state_->try_acquire_operation();
    if (!operation)
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    if (!writer_->write(const_cast<void*>(message))) {
        return Result<void>::failure(Error(ErrorCode::DdsError, "Fast DDS write failed"));
    }
    return Result<void>::success();
}

Result<std::size_t> Publisher::Impl::matched_subscriber_count() const {
    const auto operation = state_->try_acquire_operation();
    if (!operation) {
        return Result<std::size_t>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    eprosima::fastdds::dds::PublicationMatchedStatus status;
    const auto result = writer_->get_publication_matched_status(status);
    if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
        return Result<std::size_t>::failure(
            impl::fastdds::to_error(result, "Fast DDS matched subscription query failed"));
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(status.current_count));
}

Result<std::unique_ptr<Event>> Publisher::Impl::create_event(EventType type) {
    if (type != EventType::LivelinessLost && type != EventType::OfferedDeadlineMissed &&
        type != EventType::OfferedIncompatibleQos) {
        return Result<std::unique_ptr<Event>>::failure(
            Error(ErrorCode::InvalidArgument, "EventType is not valid for Publisher"));
    }
    const auto operation = state_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<Event>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    if (!event_parent_->alive.load(std::memory_order_acquire)) {
        return Result<std::unique_ptr<Event>>::failure(
            Error(ErrorCode::ParentDestroyed, "Publisher is destroyed"));
    }
    auto wait_state = std::make_shared<GuardConditionState>(state_);
    auto attached = event_parent_->attach(*writer_);
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
