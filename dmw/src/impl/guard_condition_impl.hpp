#ifndef DMW_IMPL__GUARD_CONDITION_IMPL_HPP_
#define DMW_IMPL__GUARD_CONDITION_IMPL_HPP_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include "dmw/guard_condition.hpp"
#include "impl/fastdds/context_state.hpp"
#include "impl/lock_rank.hpp"

namespace dmw {

namespace impl {

struct Registration;
}  // namespace impl

class GuardConditionState {
public:
    explicit GuardConditionState(std::shared_ptr<impl::fastdds::ContextState> state) noexcept
    : context_state(std::move(state)) {}

    const std::shared_ptr<impl::fastdds::ContextState>& context() const noexcept {
        return context_state;
    }

    Result<void> trigger();

    bool consume_trigger() noexcept {
        auto consumed = consumed_generation.load(std::memory_order_acquire);
        while (true) {
            const auto triggered = trigger_generation.load(std::memory_order_acquire);
            if (consumed == triggered) return false;
            if (consumed_generation.compare_exchange_weak(
                    consumed, triggered, std::memory_order_acq_rel, std::memory_order_acquire)) {
                pending.store(false, std::memory_order_release);
                return true;
            }
        }
    }

    void notify_wait_set() noexcept {
        std::lock_guard lock(callback_mutex);
        if (wake_callback) {
            wake_callback();
        }
    }

    void detach_wait_set() noexcept {
        std::function<void()> callback;
        {
            std::lock_guard lock(callback_mutex);
            callback = detach_callback;
        }
        if (callback) callback();
    }

    void clear_pending() noexcept { pending.store(false, std::memory_order_release); }

    void set_pending() noexcept { pending.store(true, std::memory_order_release); }

    bool is_pending() const noexcept { return pending.load(std::memory_order_acquire); }

    void set_wake_callback(std::function<void()> callback) {
        std::lock_guard lock(callback_mutex);
        wake_callback = std::move(callback);
    }

    void close() noexcept {
        closing.store(true, std::memory_order_release);
        detach_wait_set();
        notify_wait_set();
    }

private:
    friend struct impl::Registration;

    std::shared_ptr<impl::fastdds::ContextState> context_state;
    // Generations preserve merged-trigger semantics without relying on a
    // boolean transition that can wrap silently under sustained triggering.
    std::atomic<std::uint64_t> trigger_generation{0};
    std::atomic<std::uint64_t> consumed_generation{0};
    std::atomic<bool> pending{false};
    std::atomic<bool> closing{false};
    std::atomic<std::uint64_t> wait_set_id{0};
    std::atomic<std::uint64_t> registration_id{0};
    impl::RankedMutex<impl::LockRank::WaitableLocal> callback_mutex;
    std::function<void()> wake_callback;
    std::function<void()> detach_callback;
};

class GuardCondition::Impl {
public:
    explicit Impl(std::shared_ptr<impl::fastdds::ContextState> state) noexcept
    : state_(std::make_shared<GuardConditionState>(std::move(state))) {}
    ~Impl() noexcept { state_->close(); }

    Result<void> trigger();
    const std::shared_ptr<GuardConditionState>& wait_state() const noexcept { return state_; }

private:
    std::shared_ptr<GuardConditionState> state_;
};

}  // namespace dmw

#endif  // DMW_IMPL__GUARD_CONDITION_IMPL_HPP_
