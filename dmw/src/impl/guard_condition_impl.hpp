// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__IMPL__GUARD_CONDITION_IMPL_HPP_
#define DMW__IMPL__GUARD_CONDITION_IMPL_HPP_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include "dmw/guard_condition.hpp"
#include "impl/fastdds/context_state.hpp"

namespace dmw {

struct GuardConditionState {
    explicit GuardConditionState(std::shared_ptr<impl::fastdds::ContextState> state) noexcept
    : context_state(std::move(state)) {}

    std::shared_ptr<impl::fastdds::ContextState> context_state;
    std::atomic<bool> pending{false};
    std::atomic<bool> closing{false};
    std::atomic<std::uint64_t> wait_set_id{0};
    std::atomic<std::uint64_t> registration_id{0};
    std::mutex callback_mutex;
    std::function<void()> wake_callback;
    std::function<void()> detach_callback;

    void notify_wait_set() noexcept {
        std::lock_guard<std::mutex> lock(callback_mutex);
        if (wake_callback) {
            wake_callback();
        }
    }

    void detach_wait_set() noexcept {
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            callback = detach_callback;
        }
        if (callback) callback();
    }
};

class GuardCondition::Impl {
public:
    explicit Impl(std::shared_ptr<impl::fastdds::ContextState> state) noexcept
    : state_(std::make_shared<GuardConditionState>(std::move(state))) {}
    ~Impl() noexcept {
        state_->closing.store(true, std::memory_order_release);
        state_->detach_wait_set();
        state_->notify_wait_set();
    }

    std::shared_ptr<GuardConditionState> state_;
};

}  // namespace dmw

#endif  // DMW__IMPL__GUARD_CONDITION_IMPL_HPP_
