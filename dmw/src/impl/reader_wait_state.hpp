// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__IMPL__READER_WAIT_STATE_HPP_
#define DMW__IMPL__READER_WAIT_STATE_HPP_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include <fastdds/dds/subscriber/DataReader.hpp>

#include "impl/fastdds/context_state.hpp"
#include "impl/lock_rank.hpp"

namespace dmw {
namespace impl {

/// Keeps a DataReader usable while a non-owning WaitSet registration exists.
struct ReaderWaitState {
    enum class Lifecycle { Open, DeleteDeferredByWaitSet, Closed };
    ReaderWaitState(
        std::shared_ptr<fastdds::ContextState> context, eprosima::fastdds::dds::DataReader* value)
    : context_state(std::move(context)), reader(value) {}

    bool is_ready() noexcept {
        std::lock_guard<std::mutex> lock(reader_mutex);
        return blocking_enabled.load(std::memory_order_acquire) &&
               !closing.load(std::memory_order_acquire) && reader != nullptr &&
               reader->get_unread_count() != 0U;
    }

    /// Close the logical reader and detach it from its WaitSet before the
    /// owning endpoint deletes the DDS DataReader.  A false result records
    /// DeleteDeferredByWaitSet and deliberately retains the reader pointer:
    /// the final successful WaitSet detach will retry its DDS deletion.
    bool close() noexcept {
        std::function<bool()> callback;
        {
            std::unique_lock callback_lock(callback_mutex);
            closing.store(true, std::memory_order_release);
            callback_cv.wait(callback_lock, [this] { return !claim_in_progress; });
            callback = detach_callback;
        }
        const bool detached = !callback || callback();
        if (detached) {
            std::lock_guard<std::mutex> lock(reader_mutex);
            reader = nullptr;
            lifecycle.store(Lifecycle::Closed, std::memory_order_release);
        } else {
            lifecycle.store(Lifecycle::DeleteDeferredByWaitSet, std::memory_order_release);
        }
        notify_wait_set();
        return detached;
    }

    /// Called by the WaitSet after it has conclusively detached the native
    /// StatusCondition and cleared this reader's registration identity.
    /// Failure remains a conservative Context-teardown retention; no raw DDS
    /// pointer is released until Fast DDS confirms deletion.
    void complete_deferred_delete() noexcept {
        if (lifecycle.load(std::memory_order_acquire) != Lifecycle::DeleteDeferredByWaitSet ||
            wait_set_id.load(std::memory_order_acquire) != 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(reader_mutex);
        if (reader == nullptr) {
            lifecycle.store(Lifecycle::Closed, std::memory_order_release);
            return;
        }
        try {
            if (context_state->subscriber()->delete_datareader(reader) ==
                eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
                reader = nullptr;
                lifecycle.store(Lifecycle::Closed, std::memory_order_release);
            }
        } catch (...) {
            // Retain the reader under its Context container.  The process
            // runtime/participant barrier remains the final safe owner.
        }
    }

    /// A failed native WaitSet detach is not safe to tear down piecemeal:
    /// Fast DDS may still retain the StatusCondition.  Keep the WaitSet state
    /// (and therefore this reader/context) alive as a deliberate
    /// process-lifetime cycle until a later successful detach can break it.
    void quarantine_wait_set(std::shared_ptr<void> state) noexcept {
        std::lock_guard lock(callback_mutex);
        quarantined_wait_set = std::move(state);
    }

    void notify_wait_set() noexcept {
        std::lock_guard lock(callback_mutex);
        if (wake_callback) {
            wake_callback();
        }
    }

    bool detach_wait_set() noexcept {
        std::function<bool()> callback;
        {
            std::lock_guard lock(callback_mutex);
            callback = detach_callback;
        }
        return !callback || callback();
    }

    /// Server capacity changes must remove an unread request reader from the
    /// native WaitSet.  The logical flag is published first, so a concurrent
    /// waiter cannot report it ready while the native detach is in flight.
    void set_blocking_enabled(bool enabled) noexcept {
        if (blocking_enabled.exchange(enabled, std::memory_order_acq_rel) == enabled) return;
        std::function<void(bool)> callback;
        {
            std::lock_guard lock(callback_mutex);
            callback = topology_callback;
        }
        if (callback) callback(enabled);
        notify_wait_set();
    }

    std::shared_ptr<fastdds::ContextState> context_state;
    std::atomic<bool> closing{false};
    std::atomic<std::uint64_t> wait_set_id{0};
    std::atomic<std::uint64_t> registration_id{0};
    std::atomic<bool> blocking_enabled{true};
    std::atomic<Lifecycle> lifecycle{Lifecycle::Open};
    std::mutex reader_mutex;
    eprosima::fastdds::dds::DataReader* reader;
    RankedMutex<LockRank::WaitableLocal> callback_mutex;
    std::condition_variable_any callback_cv;
    bool claim_in_progress{false};
    std::function<void()> wake_callback;
    std::function<bool()> detach_callback;
    std::function<void(bool)> topology_callback;
    std::shared_ptr<void> quarantined_wait_set;
};

}  // namespace impl
}  // namespace dmw

#endif  // DMW__IMPL__READER_WAIT_STATE_HPP_
