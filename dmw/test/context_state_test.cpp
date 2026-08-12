// SPDX-License-Identifier: Apache-2.0

#include <cassert>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "impl/fastdds/context_state.hpp"

int main() {
    auto state = std::make_shared<dmw::impl::fastdds::ContextState>(
        nullptr, nullptr, nullptr, nullptr, 17U, dmw::CompatibilityProfile::NativeDds);
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
        nullptr, nullptr, nullptr, nullptr, 18U, dmw::CompatibilityProfile::NativeDds);
    auto operation = second_state->try_acquire_operation();
    assert(operation);
    std::atomic<bool> shutdown_complete{false};
    std::thread shutdown_thread([&] {
        second_state->shutdown();
        shutdown_complete.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(second_state->is_shutdown());
    assert(!second_state->try_acquire_operation());
    assert(!shutdown_complete.load(std::memory_order_acquire));
    operation = {};
    shutdown_thread.join();
    assert(shutdown_complete.load(std::memory_order_acquire));
    return 0;
}
