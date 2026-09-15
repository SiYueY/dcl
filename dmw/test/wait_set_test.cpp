#include <cassert>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <thread>

#include "dmw/context.hpp"
#include "dmw/guard_condition.hpp"
#include "dmw/wait_set.hpp"

namespace {

using namespace std::chrono_literals;

void assert_ready_for(
    std::future<dmw::Result<dmw::WaitResult>>& result,
    const dmw::WaitableRegistration& registration) {
    assert(result.wait_for(2s) == std::future_status::ready);
    auto wait_result = result.get();
    assert(wait_result);
    assert(wait_result.value().status() == dmw::WaitStatus::Ready);
    assert(wait_result.value().ready().size() == 1);
    assert(wait_result.value().ready().front() == registration);
}

}  // namespace

int main() {
    if (std::getenv("DMW_ENABLE_DDS_INTEGRATION") == nullptr) return 0;

    dmw::ContextOptions options;
    options.participant_name = "dmw-wait-set-test";
    auto context = dmw::Context::create(options);
    assert(context);

    auto wait_set = context.value()->create_wait_set();
    auto guard = context.value()->create_guard_condition();
    assert(wait_set);
    assert(guard);
    auto registration = wait_set.value()->add(*guard.value());
    assert(registration);

    auto guard_wait = std::async(std::launch::async, [&] {
        return wait_set.value()->wait(dmw::WaitTimeout::infinite());
    });
    std::this_thread::sleep_for(20ms);
    assert(guard.value()->trigger());
    assert_ready_for(guard_wait, registration.value());

    // A WaitSet blocks while empty. Adding a waitable and triggering it must
    // wake that wait without requiring a polling interval.
    auto topology_wait_set = context.value()->create_wait_set();
    auto topology_guard = context.value()->create_guard_condition();
    assert(topology_wait_set);
    assert(topology_guard);
    auto topology_wait = std::async(std::launch::async, [&] {
        return topology_wait_set.value()->wait(dmw::WaitTimeout::infinite());
    });
    std::this_thread::sleep_for(20ms);
    auto topology_registration = topology_wait_set.value()->add(*topology_guard.value());
    assert(topology_registration);
    assert(topology_guard.value()->trigger());
    assert_ready_for(topology_wait, topology_registration.value());

    auto active_wait = std::async(std::launch::async, [&] {
        return wait_set.value()->wait(dmw::WaitTimeout::infinite());
    });
    std::this_thread::sleep_for(20ms);
    auto concurrent_wait = wait_set.value()->wait(dmw::WaitTimeout::poll());
    assert(!concurrent_wait);
    assert(concurrent_wait.error().code() == dmw::ErrorCode::Busy);
    assert(guard.value()->trigger());
    assert_ready_for(active_wait, registration.value());

    auto shutdown_wait_set = context.value()->create_wait_set();
    assert(shutdown_wait_set);
    auto shutdown_wait = std::async(std::launch::async, [&] {
        return shutdown_wait_set.value()->wait(dmw::WaitTimeout::infinite());
    });
    std::this_thread::sleep_for(20ms);
    assert(context.value()->shutdown());
    assert(shutdown_wait.wait_for(2s) == std::future_status::ready);
    auto shutdown_result = shutdown_wait.get();
    assert(!shutdown_result);
    assert(shutdown_result.error().code() == dmw::ErrorCode::ContextShutdown);

    return 0;
}
