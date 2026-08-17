#ifndef DMW_IMPL__EVENT_PARENT_STATE_HPP_
#define DMW_IMPL__EVENT_PARENT_STATE_HPP_

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>

#include "dmw/event_info.hpp"
#include "dmw/result.hpp"
#include "impl/guard_condition_impl.hpp"
#include "impl/lock_rank.hpp"

namespace dmw {

namespace impl {

struct EventParentState : std::enable_shared_from_this<EventParentState> {
private:
    struct EventRecord {
        std::uint64_t registration_id;
        EventType type;
        std::weak_ptr<GuardConditionState> event;
    };
    struct EventSlot {
        EventInfo info;
        std::uint64_t generation{0};
    };
    enum class ListenerInstallState { Detached, Installing, Attached };
    class WriterListener;
    class ReaderListener;

public:
    explicit EventParentState(std::shared_ptr<Context> context) noexcept;

    struct Snapshot {
        EventInfo info;
        std::uint64_t generation;
    };

    EventParentState(const EventParentState&) = delete;
    EventParentState& operator=(const EventParentState&) = delete;
    ~EventParentState() noexcept;

    Result<void> attach(eprosima::fastdds::dds::DataWriter& writer);
    Result<void> attach(eprosima::fastdds::dds::DataReader& reader);
    std::uint64_t register_event(EventType type, const std::shared_ptr<GuardConditionState>& event);
    void unregister_event(std::uint64_t registration_id) noexcept;
    std::size_t event_registration_count() const noexcept;
    Snapshot snapshot(EventType type) const;
    bool is_exhausted() const noexcept { return exhausted_.load(std::memory_order_acquire); }
    void update(EventType type, EventInfo info) noexcept;
    /// Called after the owning DDS endpoint has detached its listener and
    /// before the endpoint is deleted.
    void drain_listeners() noexcept;
    /// Transfers listeners whose DDS detach could not be confirmed to the
    /// process quarantine.  Their weak source then expires safely while DDS
    /// retains the raw listener binding in a Context-owned endpoint.
    void quarantine_listeners() noexcept;

    void close() noexcept {
        alive.store(false, std::memory_order_release);
        std::vector<EventRecord> records;
        {
            std::lock_guard lock(mutex);
            records.swap(events);
        }
        for (const auto& record : records) {
            if (const auto event = record.event.lock()) {
                event->close();
            }
        }
    }

    std::shared_ptr<Context> context;
    std::atomic<bool> alive{true};
    mutable RankedMutex<LockRank::EndpointRuntime> mutex;

private:
    EventSlot& slot(EventType type) noexcept;
    const EventSlot& slot(EventType type) const noexcept;

    EventSlot slots_[7];
    std::vector<EventRecord> events;
    std::uint64_t next_event_registration_id_{1};
    std::atomic<bool> exhausted_{false};
    std::condition_variable_any listener_cv_;
    ListenerInstallState writer_listener_state_{ListenerInstallState::Detached};
    ListenerInstallState reader_listener_state_{ListenerInstallState::Detached};
    std::unique_ptr<WriterListener> writer_listener_;
    std::unique_ptr<ReaderListener> reader_listener_;
};

}  // namespace impl
}  // namespace dmw

#endif  // DMW_IMPL__EVENT_PARENT_STATE_HPP_
