#ifndef DMW_IMPL__EVENT_IMPL_HPP_
#define DMW_IMPL__EVENT_IMPL_HPP_

#include <cstdint>
#include <memory>

#include "dmw/event.hpp"
#include "impl/event_parent_state.hpp"

namespace dmw {

class Event::Impl {
public:
    Impl(
        std::shared_ptr<impl::EventParentState> parent, EventType event_type,
        std::shared_ptr<GuardConditionState> wait_state, impl::EventParentState::Snapshot cursor,
        std::uint64_t registration_id) noexcept
    : parent_(std::move(parent)),
      type_(event_type),
      wait_state_(std::move(wait_state)),
      cursor_(std::move(cursor)),
      registration_id_(registration_id) {}

    ~Impl() noexcept {
        parent_->unregister_event(registration_id_);
        wait_state_->close();
    }

    EventType type() const noexcept { return type_; }
    Result<bool> take(EventInfo& info);
    const std::shared_ptr<GuardConditionState>& wait_state() const noexcept { return wait_state_; }

private:
    std::shared_ptr<impl::EventParentState> parent_;
    EventType type_;
    std::shared_ptr<GuardConditionState> wait_state_;
    impl::EventParentState::Snapshot cursor_;
    std::uint64_t registration_id_;
};

}  // namespace dmw

#endif  // DMW_IMPL__EVENT_IMPL_HPP_
