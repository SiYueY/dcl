// SPDX-License-Identifier: Apache-2.0

#include "impl/event_parent_state.hpp"

#include <algorithm>
#include <utility>

#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>

#include "dmw/error.hpp"

namespace dmw {
namespace impl {

namespace {

std::size_t event_index(EventType type) noexcept { return static_cast<std::size_t>(type); }

QosPolicyKind to_qos_policy(eprosima::fastdds::dds::QosPolicyId_t policy) noexcept {
    using namespace eprosima::fastdds::dds;
    switch (policy) {
        case HISTORY_QOS_POLICY_ID:
            return QosPolicyKind::History;
        case RELIABILITY_QOS_POLICY_ID:
            return QosPolicyKind::Reliability;
        case DURABILITY_QOS_POLICY_ID:
            return QosPolicyKind::Durability;
        case DEADLINE_QOS_POLICY_ID:
            return QosPolicyKind::Deadline;
        case LIFESPAN_QOS_POLICY_ID:
            return QosPolicyKind::Lifespan;
        case LIVELINESS_QOS_POLICY_ID:
            return QosPolicyKind::Liveliness;
        default:
            return QosPolicyKind::Unknown;
    }
}

EventInfo empty_event_info(EventType type) {
    switch (type) {
        case EventType::LivelinessChanged:
            return LivelinessChangedInfo{};
        case EventType::LivelinessLost:
            return LivelinessLostInfo{};
        case EventType::RequestedIncompatibleQos:
        case EventType::OfferedIncompatibleQos:
            return IncompatibleQosInfo{};
        case EventType::MessageLost:
            return MessageLostInfo{};
        case EventType::RequestedDeadlineMissed:
        case EventType::OfferedDeadlineMissed:
            return DeadlineMissedInfo{};
    }
    return DeadlineMissedInfo{};
}

void accumulate(EventInfo& total, const EventInfo& update) noexcept {
    if (auto* value = std::get_if<DeadlineMissedInfo>(&total)) {
        const auto& delta = std::get<DeadlineMissedInfo>(update);
        value->total_count = delta.total_count;
        value->total_count_change += delta.total_count_change;
    } else if (auto* value = std::get_if<LivelinessLostInfo>(&total)) {
        const auto& delta = std::get<LivelinessLostInfo>(update);
        value->total_count = delta.total_count;
        value->total_count_change += delta.total_count_change;
    } else if (auto* value = std::get_if<LivelinessChangedInfo>(&total)) {
        const auto& delta = std::get<LivelinessChangedInfo>(update);
        value->alive_count = delta.alive_count;
        value->not_alive_count = delta.not_alive_count;
        value->alive_count_change += delta.alive_count_change;
        value->not_alive_count_change += delta.not_alive_count_change;
    } else if (auto* value = std::get_if<IncompatibleQosInfo>(&total)) {
        const auto& delta = std::get<IncompatibleQosInfo>(update);
        value->total_count = delta.total_count;
        value->total_count_change += delta.total_count_change;
        value->last_policy = delta.last_policy;
    } else {
        auto& message_lost = std::get<MessageLostInfo>(total);
        const auto& delta = std::get<MessageLostInfo>(update);
        message_lost.total_count = delta.total_count;
        message_lost.total_count_change += delta.total_count_change;
    }
}

}  // namespace

class EventParentState::WriterListener final : public eprosima::fastdds::dds::DataWriterListener {
public:
    explicit WriterListener(std::weak_ptr<EventParentState> source) noexcept
    : source_(std::move(source)) {}

    void on_offered_deadline_missed(
        eprosima::fastdds::dds::DataWriter*,
        const eprosima::fastdds::dds::OfferedDeadlineMissedStatus& status) override {
        if (const auto source = source_.lock()) {
            source->update(
                EventType::OfferedDeadlineMissed,
                DeadlineMissedInfo{
                    static_cast<std::int32_t>(status.total_count),
                    static_cast<std::int32_t>(status.total_count_change)});
        }
    }

    void on_offered_incompatible_qos(
        eprosima::fastdds::dds::DataWriter*,
        const eprosima::fastdds::dds::OfferedIncompatibleQosStatus& status) override {
        if (const auto source = source_.lock()) {
            source->update(
                EventType::OfferedIncompatibleQos,
                IncompatibleQosInfo{
                    static_cast<std::int32_t>(status.total_count),
                    static_cast<std::int32_t>(status.total_count_change),
                    to_qos_policy(status.last_policy_id)});
        }
    }

    void on_liveliness_lost(
        eprosima::fastdds::dds::DataWriter*,
        const eprosima::fastdds::dds::LivelinessLostStatus& status) override {
        if (const auto source = source_.lock()) {
            source->update(
                EventType::LivelinessLost,
                LivelinessLostInfo{
                    static_cast<std::int32_t>(status.total_count),
                    static_cast<std::int32_t>(status.total_count_change)});
        }
    }

private:
    std::weak_ptr<EventParentState> source_;
};

class EventParentState::ReaderListener final : public eprosima::fastdds::dds::DataReaderListener {
public:
    explicit ReaderListener(std::weak_ptr<EventParentState> source) noexcept
    : source_(std::move(source)) {}

    void on_requested_deadline_missed(
        eprosima::fastdds::dds::DataReader*,
        const eprosima::fastdds::dds::RequestedDeadlineMissedStatus& status) override {
        if (const auto source = source_.lock()) {
            source->update(
                EventType::RequestedDeadlineMissed,
                DeadlineMissedInfo{
                    static_cast<std::int32_t>(status.total_count),
                    static_cast<std::int32_t>(status.total_count_change)});
        }
    }

    void on_liveliness_changed(
        eprosima::fastdds::dds::DataReader*,
        const eprosima::fastdds::dds::LivelinessChangedStatus& status) override {
        if (const auto source = source_.lock()) {
            source->update(
                EventType::LivelinessChanged,
                LivelinessChangedInfo{
                    status.alive_count, status.not_alive_count, status.alive_count_change,
                    status.not_alive_count_change});
        }
    }

    void on_requested_incompatible_qos(
        eprosima::fastdds::dds::DataReader*,
        const eprosima::fastdds::dds::RequestedIncompatibleQosStatus& status) override {
        if (const auto source = source_.lock()) {
            source->update(
                EventType::RequestedIncompatibleQos,
                IncompatibleQosInfo{
                    static_cast<std::int32_t>(status.total_count),
                    static_cast<std::int32_t>(status.total_count_change),
                    to_qos_policy(status.last_policy_id)});
        }
    }

    void on_sample_lost(
        eprosima::fastdds::dds::DataReader*,
        const eprosima::fastdds::dds::SampleLostStatus& status) override {
        if (const auto source = source_.lock()) {
            source->update(
                EventType::MessageLost,
                MessageLostInfo{
                    static_cast<std::size_t>(std::max(status.total_count, 0)),
                    static_cast<std::size_t>(std::max(status.total_count_change, 0))});
        }
    }

private:
    std::weak_ptr<EventParentState> source_;
};

EventParentState::~EventParentState() noexcept = default;

EventParentState::EventParentState(std::shared_ptr<fastdds::ContextState> context) noexcept
: context_state(std::move(context)) {}

Result<void> EventParentState::attach(eprosima::fastdds::dds::DataWriter& writer) {
    WriterListener* listener = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex);
        listener_cv_.wait(
            lock, [this] { return writer_listener_state_ != ListenerInstallState::Installing; });
        if (writer_listener_state_ == ListenerInstallState::Attached) {
            return Result<void>::success();
        }
        writer_listener_state_ = ListenerInstallState::Installing;
        try {
            writer_listener_ = std::make_unique<WriterListener>(weak_from_this());
            listener = writer_listener_.get();
        } catch (...) {
            writer_listener_state_ = ListenerInstallState::Detached;
            lock.unlock();
            listener_cv_.notify_all();
            throw;
        }
    }

    eprosima::fastrtps::types::ReturnCode_t result;
    try {
        result = writer.set_listener(listener);
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex);
        writer_listener_.reset();
        writer_listener_state_ = ListenerInstallState::Detached;
        listener_cv_.notify_all();
        throw;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
            writer_listener_state_ = ListenerInstallState::Attached;
        } else {
            writer_listener_.reset();
            writer_listener_state_ = ListenerInstallState::Detached;
        }
    }
    listener_cv_.notify_all();
    if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
        return Result<void>::failure(
            Error(ErrorCode::DdsError, "Fast DDS failed to set a writer listener"));
    }
    return Result<void>::success();
}

Result<void> EventParentState::attach(eprosima::fastdds::dds::DataReader& reader) {
    ReaderListener* listener = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex);
        listener_cv_.wait(
            lock, [this] { return reader_listener_state_ != ListenerInstallState::Installing; });
        if (reader_listener_state_ == ListenerInstallState::Attached) {
            return Result<void>::success();
        }
        reader_listener_state_ = ListenerInstallState::Installing;
        try {
            reader_listener_ = std::make_unique<ReaderListener>(weak_from_this());
            listener = reader_listener_.get();
        } catch (...) {
            reader_listener_state_ = ListenerInstallState::Detached;
            lock.unlock();
            listener_cv_.notify_all();
            throw;
        }
    }

    eprosima::fastrtps::types::ReturnCode_t result;
    try {
        result = reader.set_listener(listener);
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex);
        reader_listener_.reset();
        reader_listener_state_ = ListenerInstallState::Detached;
        listener_cv_.notify_all();
        throw;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (result == eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
            reader_listener_state_ = ListenerInstallState::Attached;
        } else {
            reader_listener_.reset();
            reader_listener_state_ = ListenerInstallState::Detached;
        }
    }
    listener_cv_.notify_all();
    if (result != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
        return Result<void>::failure(
            Error(ErrorCode::DdsError, "Fast DDS failed to set a reader listener"));
    }
    return Result<void>::success();
}

std::uint64_t EventParentState::register_event(
    EventType type, const std::shared_ptr<GuardConditionState>& event) {
    std::lock_guard<std::mutex> lock(mutex);
    if (next_event_registration_id_ == 0) {
        return 0;
    }
    const auto registration_id = next_event_registration_id_++;
    events.push_back(EventRecord{registration_id, type, event});
    return registration_id;
}

void EventParentState::unregister_event(std::uint64_t registration_id) noexcept {
    if (registration_id == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    const auto event =
        std::find_if(events.begin(), events.end(), [registration_id](const auto& record) {
            return record.registration_id == registration_id;
        });
    if (event != events.end()) {
        events.erase(event);
    }
}

std::size_t EventParentState::event_registration_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    return events.size();
}

EventParentState::Snapshot EventParentState::snapshot(EventType type) const {
    std::lock_guard<std::mutex> lock(mutex);
    const auto& current = slot(type);
    return Snapshot{
        current.generation == 0 ? empty_event_info(type) : current.info, current.generation};
}

void EventParentState::update(EventType type, EventInfo info) noexcept {
    std::uint64_t notification_limit = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto& current = slot(type);
        if (current.generation == 0) {
            current.info = std::move(info);
        } else {
            accumulate(current.info, info);
        }
        ++current.generation;
        notification_limit = next_event_registration_id_ - 1;
        for (const auto& record : events) {
            if (record.type == type) {
                if (const auto event = record.event.lock()) {
                    event->pending.store(true, std::memory_order_release);
                }
            }
        }
    }

    std::uint64_t previous_registration_id = 0;
    while (previous_registration_id < notification_limit) {
        std::shared_ptr<GuardConditionState> event;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto next = std::find_if(
                events.begin(), events.end(),
                [type, notification_limit, previous_registration_id](const EventRecord& record) {
                    return record.type == type &&
                           record.registration_id > previous_registration_id &&
                           record.registration_id <= notification_limit;
                });
            if (next == events.end()) break;
            previous_registration_id = next->registration_id;
            event = next->event.lock();
        }
        if (event) event->notify_wait_set();
    }
}

EventParentState::EventSlot& EventParentState::slot(EventType type) noexcept {
    return slots_[event_index(type)];
}

const EventParentState::EventSlot& EventParentState::slot(EventType type) const noexcept {
    return slots_[event_index(type)];
}

}  // namespace impl
}  // namespace dmw
