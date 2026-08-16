#include <cassert>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "impl/fastdds/context_state.hpp"
#include "impl/reader_wait_state.hpp"

namespace dmw {
namespace impl {

class ReaderWaitStateTestAccess {
public:
    static void set_claim_in_progress(const std::shared_ptr<ReaderWaitState>& state, bool value) {
        std::lock_guard lock(state->callback_mutex);
        state->claim_in_progress = value;
        if (!value) state->callback_cv.notify_all();
    }
};

}  // namespace impl
}  // namespace dmw

int main() {
    auto state = std::make_shared<dmw::impl::fastdds::ContextState>(
        nullptr, nullptr, nullptr, nullptr, 17U, dmw::RuntimeMode::DDS);
    int first_count = 0;
    int removed_count = 0;
    int last_count = 0;
    const auto first = state->register_shutdown_callback([&] { ++first_count; });
    const auto removed = state->register_shutdown_callback([&] { ++removed_count; });
    assert(first != 0);
    assert(removed != 0);
    state->unregister_shutdown_callback(removed);

    const auto reentrant = state->register_shutdown_callback([&] {
        state->unregister_shutdown_callback(first);
        throw 1;
    });
    const auto last = state->register_shutdown_callback([&] { ++last_count; });
    assert(reentrant != 0);
    assert(last != 0);

    state->shutdown();
    assert(state->is_shutdown());
    assert(first_count == 1);
    assert(removed_count == 0);
    assert(last_count == 1);

    state->shutdown();
    assert(first_count == 1);
    assert(state->register_shutdown_callback([] {}) == 0);

    auto second_state = std::make_shared<dmw::impl::fastdds::ContextState>(
        nullptr, nullptr, nullptr, nullptr, 18U, dmw::RuntimeMode::DDS);
    auto operation = second_state->try_acquire_operation();
    assert(operation);
    std::atomic<bool> shutdown_complete{false};
    std::atomic<bool> concurrent_shutdown_complete{false};
    std::thread shutdown_thread([&] {
        second_state->shutdown();
        shutdown_complete.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(second_state->is_shutdown());
    assert(!second_state->try_acquire_operation());
    assert(!shutdown_complete.load(std::memory_order_acquire));
    std::thread concurrent_shutdown_thread([&] {
        second_state->shutdown();
        concurrent_shutdown_complete.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(!concurrent_shutdown_complete.load(std::memory_order_acquire));
    operation = {};
    shutdown_thread.join();
    concurrent_shutdown_thread.join();
    assert(shutdown_complete.load(std::memory_order_acquire));
    assert(concurrent_shutdown_complete.load(std::memory_order_acquire));

    eprosima::fastrtps::rtps::GuidPrefix_t prefix;
    prefix.value[0] = 42;
    const auto observations = second_state->participant_observations();
    const auto active = observations->observe(prefix, false);
    assert(active);
    assert(active->lifecycle.load() == dmw::impl::ParticipantLifecycle::Active);
    const auto tombstone = observations->observe(prefix, true);
    assert(tombstone == active);
    assert(tombstone->lifecycle.load() == dmw::impl::ParticipantLifecycle::Removed);
    // Tombstones are terminal under the frozen GuidPrefix deployment constraint.
    assert(observations->observe(prefix, false) == tombstone);
    assert(tombstone->lifecycle.load() == dmw::impl::ParticipantLifecycle::Removed);
    assert(observations->capability() == dmw::impl::DiscoveryCapability::Degraded);

    auto reader_wait_state = std::make_shared<dmw::impl::ReaderWaitState>(second_state, nullptr);
    dmw::impl::ReaderWaitStateTestAccess::set_claim_in_progress(reader_wait_state, true);
    std::atomic<bool> reader_closed{false};
    std::thread reader_close_thread([&] {
        assert(reader_wait_state->close());
        reader_closed.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(!reader_closed.load(std::memory_order_acquire));
    dmw::impl::ReaderWaitStateTestAccess::set_claim_in_progress(reader_wait_state, false);
    reader_close_thread.join();
    assert(reader_closed.load(std::memory_order_acquire));
    return 0;
}
