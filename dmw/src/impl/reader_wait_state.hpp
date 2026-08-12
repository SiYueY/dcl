// SPDX-License-Identifier: Apache-2.0

#ifndef DMW__IMPL__READER_WAIT_STATE_HPP_
#define DMW__IMPL__READER_WAIT_STATE_HPP_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include <fastdds/dds/subscriber/DataReader.hpp>

#include "impl/fastdds/context_state.hpp"

namespace dmw {
namespace impl {

/// Keeps a DataReader usable while a non-owning WaitSet registration exists.
struct ReaderWaitState {
    ReaderWaitState(
        std::shared_ptr<fastdds::ContextState> context, eprosima::fastdds::dds::DataReader* value)
    : context_state(std::move(context)), reader(value) {}

    bool is_ready() noexcept {
        std::lock_guard<std::mutex> lock(reader_mutex);
        return !closing.load(std::memory_order_acquire) && reader != nullptr &&
               reader->get_unread_count() != 0U;
    }

    /// Close the logical reader and detach it from its WaitSet before the
    /// owning endpoint deletes the DDS DataReader.  A false result means the
    /// caller must retain the DDS entity because its StatusCondition may
    /// still be attached to a Fast DDS WaitSet.
    bool close() noexcept {
        std::function<bool()> callback;
        {
            std::lock_guard<std::mutex> callback_lock(callback_mutex);
            closing.store(true, std::memory_order_release);
            callback = detach_callback;
        }
        const bool detached = !callback || callback();
        {
            std::lock_guard<std::mutex> lock(reader_mutex);
            reader = nullptr;
        }
        notify_wait_set();
        return detached;
    }

    void notify_wait_set() noexcept {
        std::lock_guard<std::mutex> lock(callback_mutex);
        if (wake_callback) {
            wake_callback();
        }
    }

    bool detach_wait_set() noexcept {
        std::function<bool()> callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            callback = detach_callback;
        }
        return !callback || callback();
    }

    std::shared_ptr<fastdds::ContextState> context_state;
    std::atomic<bool> closing{false};
    std::atomic<std::uint64_t> wait_set_id{0};
    std::atomic<std::uint64_t> registration_id{0};
    std::mutex reader_mutex;
    eprosima::fastdds::dds::DataReader* reader;
    std::mutex callback_mutex;
    std::function<void()> wake_callback;
    std::function<bool()> detach_callback;
};

}  // namespace impl
}  // namespace dmw

#endif  // DMW__IMPL__READER_WAIT_STATE_HPP_
