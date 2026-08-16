#include <cassert>
#include <atomic>
#include <memory>

#include "impl/event_parent_state.hpp"

int main() {
    auto context = std::make_shared<dmw::impl::fastdds::ContextState>(
        nullptr, nullptr, nullptr, nullptr, 0U, dmw::RuntimeMode::DDS);
    auto source = std::make_shared<dmw::impl::EventParentState>(context);

    const auto initial = source->snapshot(dmw::EventType::RequestedDeadlineMissed);
    assert(initial.generation == 0);
    assert(std::get<dmw::DeadlineMissedInfo>(initial.info).total_count_change == 0);

    auto event = std::make_shared<dmw::GuardConditionState>(context);
    std::atomic<std::size_t> wake_count{0};
    event->set_wake_callback([&wake_count] { ++wake_count; });
    const auto registration =
        source->register_event(dmw::EventType::RequestedIncompatibleQos, event);
    assert(registration != 0);
    assert(source->event_registration_count() == 1);

    source->update(
        dmw::EventType::RequestedIncompatibleQos,
        dmw::IncompatibleQosInfo{1, 1, dmw::QosPolicyKind::Reliability});
    assert(event->is_pending());
    assert(wake_count.load() == 1);
    event->clear_pending();
    source->unregister_event(registration);
    assert(source->event_registration_count() == 0);

    source->update(dmw::EventType::RequestedDeadlineMissed, dmw::DeadlineMissedInfo{3, 3});
    source->update(dmw::EventType::RequestedDeadlineMissed, dmw::DeadlineMissedInfo{5, 2});
    const auto deadline = source->snapshot(dmw::EventType::RequestedDeadlineMissed);
    const auto& deadline_info = std::get<dmw::DeadlineMissedInfo>(deadline.info);
    assert(deadline.generation == 2);
    assert(deadline_info.total_count == 5);
    assert(deadline_info.total_count_change == 5);

    source->update(dmw::EventType::LivelinessChanged, dmw::LivelinessChangedInfo{3, 1, 3, 1});
    source->update(dmw::EventType::LivelinessChanged, dmw::LivelinessChangedInfo{2, 2, -1, 1});
    const auto liveliness = source->snapshot(dmw::EventType::LivelinessChanged);
    const auto& liveliness_info = std::get<dmw::LivelinessChangedInfo>(liveliness.info);
    assert(liveliness_info.alive_count == 2);
    assert(liveliness_info.not_alive_count == 2);
    assert(liveliness_info.alive_count_change == 2);
    assert(liveliness_info.not_alive_count_change == 2);

    source->close();
    assert(!source->alive.load());
    return 0;
}
