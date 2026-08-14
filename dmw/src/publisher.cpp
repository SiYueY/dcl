// SPDX-License-Identifier: Apache-2.0

#include "dmw/publisher.hpp"

#include "dmw/error.hpp"
#include "impl/event_impl.hpp"
#include "impl/endpoint_impl.hpp"
#include "impl/fastdds/process_runtime.hpp"
#include "impl/fastdds/return_code.hpp"

namespace dmw {

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

Publisher::Publisher(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Publisher::~Publisher() noexcept = default;

Result<void> Publisher::publish(const void* message) {
    if (message == nullptr)
        return Result<void>::failure(Error(ErrorCode::InvalidArgument, "Message must not be null"));
    const auto operation = impl_->state_->try_acquire_operation();
    if (!operation)
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    if (!impl_->writer_->write(const_cast<void*>(message))) {
        return Result<void>::failure(Error(ErrorCode::DdsError, "Fast DDS write failed"));
    }
    return Result<void>::success();
}

std::string_view Publisher::topic_name() const noexcept { return impl_->topic_name_; }
const MessageType& Publisher::message_type() const noexcept { return impl_->type_; }

Result<std::size_t> Publisher::matched_subscriber_count() const {
    const auto operation = impl_->state_->try_acquire_operation();
    if (!operation) {
        return Result<std::size_t>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    eprosima::fastdds::dds::PublicationMatchedStatus status;
    const auto result = impl_->writer_->get_publication_matched_status(status);
    if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
        return Result<std::size_t>::failure(
            impl::fastdds::return_code_error(result, "Fast DDS matched subscription query failed"));
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(status.current_count));
}

Result<std::unique_ptr<Event>> Publisher::create_event(EventType type) {
    if (type != EventType::LivelinessLost && type != EventType::OfferedDeadlineMissed &&
        type != EventType::OfferedIncompatibleQos) {
        return Result<std::unique_ptr<Event>>::failure(
            Error(ErrorCode::InvalidArgument, "EventType is not valid for Publisher"));
    }
    const auto operation = impl_->state_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<Event>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    if (!impl_->event_parent_->alive.load(std::memory_order_acquire)) {
        return Result<std::unique_ptr<Event>>::failure(
            Error(ErrorCode::ParentDestroyed, "Publisher is destroyed"));
    }
    auto wait_state = std::make_shared<GuardConditionState>(impl_->state_);
    auto attached = impl_->event_parent_->attach(*impl_->writer_);
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
