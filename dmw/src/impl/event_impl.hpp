// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__IMPL__EVENT_IMPL_HPP_
#define DMW__IMPL__EVENT_IMPL_HPP_

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
        wait_state_->closing.store(true, std::memory_order_release);
        wait_state_->detach_wait_set();
        wait_state_->notify_wait_set();
    }

    std::shared_ptr<impl::EventParentState> parent_;
    EventType type_;
    std::shared_ptr<GuardConditionState> wait_state_;
    impl::EventParentState::Snapshot cursor_;
    std::uint64_t registration_id_;
};

}  // namespace dmw

#endif  // DMW__IMPL__EVENT_IMPL_HPP_
