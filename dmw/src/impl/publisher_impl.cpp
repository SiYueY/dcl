#include "impl/publisher_impl.hpp"

#include <cstdint>
#include <limits>

#include <fastdds/rtps/common/Time_t.h>

#include "dmw/error.hpp"
#include "impl/event_impl.hpp"
#include "impl/deadline.hpp"
#include "impl/process_lifetime.hpp"
#include "impl/qos.hpp"
#include "impl/return_code.hpp"

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
                context_->publisher()->delete_datawriter(writer_);
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

Result<void> Publisher::Impl::write(const void* message) {
    if (message == nullptr)
        return Result<void>::failure(Error(ErrorCode::InvalidArgument, "Message must not be null"));
    const auto operation = context_->try_acquire_operation();
    if (!operation)
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    const auto result =
        writer_->write(const_cast<void*>(message), eprosima::fastdds::dds::HANDLE_NIL);
    if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK)
        return Result<void>::failure(impl::to_error(result, "Fast DDS DataWriter write failed"));
    return Result<void>::success();
}

Result<std::size_t> Publisher::Impl::matched_subscriber_count() const {
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<std::size_t>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    eprosima::fastdds::dds::PublicationMatchedStatus status;
    const auto result = writer_->get_publication_matched_status(status);
    if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
        return Result<std::size_t>::failure(
            impl::to_error(result, "Fast DDS matched subscription query failed"));
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(status.current_count));
}

Result<Qos> Publisher::Impl::actual_qos() const {
    const auto operation = context_->try_acquire_operation();
    if (!operation)
        return Result<Qos>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    eprosima::fastdds::dds::DataWriterQos qos;
    const auto result = writer_->get_qos(qos);
    if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
        return Result<Qos>::failure(impl::to_error(result, "Fast DDS writer QoS query failed"));
    }
    return impl::from_neutral_qos(qos);
}

Result<bool> Publisher::Impl::wait_for_all_acked(WaitTimeout timeout) {
    const auto operation = context_->try_acquire_operation();
    if (!operation)
        return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    const auto deadline = impl::steady_deadline(timeout);
    const auto shutdown_check_interval =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::milliseconds(20));
    while (true) {
        if (context_->is_shutdown()) {
            return Result<bool>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
        }
        const auto remaining = timeout.kind() == WaitTimeout::Kind::Finite
                                   ? deadline - std::chrono::steady_clock::now()
                                   : shutdown_check_interval;
        if (timeout.kind() == WaitTimeout::Kind::Finite &&
            remaining <= std::chrono::steady_clock::duration::zero()) {
            return Result<bool>::success(false);
        }
        const auto wait_duration = timeout.kind() == WaitTimeout::Kind::Poll
                                       ? std::chrono::nanoseconds::zero()
                                       : std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::min(remaining, shutdown_check_interval));
        constexpr std::int64_t kNanosecondsPerSecond = 1000000000LL;
        const auto seconds = wait_duration.count() / kNanosecondsPerSecond;
        const auto nanoseconds = wait_duration.count() % kNanosecondsPerSecond;
        if (seconds > std::numeric_limits<std::int32_t>::max()) {
            return Result<bool>::failure(
                Error(ErrorCode::Unsupported, "Acknowledgment timeout exceeds Fast DDS range"));
        }
        const auto duration = eprosima::fastrtps::Duration_t(
            static_cast<std::int32_t>(seconds), static_cast<std::uint32_t>(nanoseconds));
        const auto result = writer_->wait_for_acknowledgments(duration);
        if (result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK)
            return Result<bool>::success(true);
        if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_TIMEOUT) {
            return Result<bool>::failure(
                impl::to_error(result, "Fast DDS acknowledgment wait failed"));
        }
        if (timeout.kind() == WaitTimeout::Kind::Poll) return Result<bool>::success(false);
    }
}

Result<void> Publisher::Impl::assert_liveliness() {
    const auto operation = context_->try_acquire_operation();
    if (!operation)
        return Result<void>::failure(Error(ErrorCode::ContextShutdown, "Context is shut down"));
    auto qos = actual_qos();
    if (!qos) return Result<void>::failure(std::move(qos.error()));
    if (qos.value().liveliness() != LivelinessPolicy::ManualByTopic) {
        return Result<void>::failure(
            Error(ErrorCode::InvalidState, "Liveliness assertion requires ManualByTopic QoS"));
    }
    const auto result = writer_->assert_liveliness();
    if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK)
        return Result<void>::failure(
            impl::to_error(result, "Fast DDS liveliness assertion failed"));
    return Result<void>::success();
}

Result<std::unique_ptr<Event>> Publisher::Impl::create_event(EventType type) {
    if (type != EventType::LivelinessLost && type != EventType::OfferedDeadlineMissed &&
        type != EventType::OfferedIncompatibleQos) {
        return Result<std::unique_ptr<Event>>::failure(
            Error(ErrorCode::InvalidArgument, "EventType is not valid for Publisher"));
    }
    const auto operation = context_->try_acquire_operation();
    if (!operation) {
        return Result<std::unique_ptr<Event>>::failure(
            Error(ErrorCode::ContextShutdown, "Context is shut down"));
    }
    if (!event_parent_->alive.load(std::memory_order_acquire)) {
        return Result<std::unique_ptr<Event>>::failure(
            Error(ErrorCode::ParentDestroyed, "Publisher is destroyed"));
    }
    auto wait_state = std::make_shared<GuardConditionState>(context_);
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
