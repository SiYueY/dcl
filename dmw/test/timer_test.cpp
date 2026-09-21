#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>

#include "dmw/clock.hpp"
#include "dmw/context.hpp"
#include "dmw/timer.hpp"
#include "dmw/wait_set.hpp"
#include "dmw/wait_timeout.hpp"

using namespace std::chrono_literals;

namespace {

void test_creation_contract() {
    auto context = dmw::Context::create({});
    assert(context);
    auto steady = context.value()->create_clock(dmw::ClockType::Steady);
    assert(steady);

    auto negative = context.value()->create_timer(*steady.value(), dmw::TimerOptions{-1ns, true});
    assert(!negative);
    assert(negative.error().code() == dmw::ErrorCode::InvalidArgument);

    auto other = dmw::Context::create({});
    assert(other);
    auto other_clock = other.value()->create_clock(dmw::ClockType::Steady);
    assert(other_clock);
    auto mismatched =
        context.value()->create_timer(*other_clock.value(), dmw::TimerOptions{1ms, true});
    assert(!mismatched);
    assert(mismatched.error().code() == dmw::ErrorCode::InvalidArgument);
}

void test_zero_period_is_always_ready() {
    auto context = dmw::Context::create({});
    assert(context);
    auto steady = context.value()->create_clock(dmw::ClockType::Steady);
    assert(steady);
    auto timer = context.value()->create_timer(*steady.value(), dmw::TimerOptions{0ns, true});
    assert(timer);
    assert(timer.value()->clock_type() == dmw::ClockType::Steady);
    assert(timer.value()->period() == 0ns);

    for (int index = 0; index < 3; ++index) {
        const auto ready = timer.value()->is_ready();
        assert(ready && ready.value());
        dmw::TimerInfo info;
        const auto consumed = timer.value()->consume(info);
        assert(consumed && consumed.value());
        assert(info.expected_call_time.clock_type == dmw::ClockType::Steady);
        assert(info.actual_call_time.clock_type == dmw::ClockType::Steady);
    }
    assert(timer.value()->time_until_next_call().value() == 0ns);
}

void test_autostart_cancel_reset() {
    auto context = dmw::Context::create({});
    assert(context);
    auto steady = context.value()->create_clock(dmw::ClockType::Steady);
    assert(steady);
    auto timer = context.value()->create_timer(*steady.value(), dmw::TimerOptions{10ms, false});
    assert(timer);

    assert(timer.value()->is_canceled().value());
    assert(!timer.value()->is_ready().value());
    const auto until = timer.value()->time_until_next_call();
    assert(!until);
    assert(until.error().code() == dmw::ErrorCode::InvalidState);

    // success + false must leave the caller output untouched.
    dmw::TimerInfo untouched;
    untouched.elapsed_since_last_call = 7ns;
    const auto not_consumed = timer.value()->consume(untouched);
    assert(not_consumed && !not_consumed.value());
    assert(untouched.elapsed_since_last_call == 7ns);

    assert(timer.value()->reset());
    assert(!timer.value()->is_canceled().value());
    assert(timer.value()->time_until_next_call().value() <= 10ms);

    assert(timer.value()->cancel());
    assert(timer.value()->cancel());
    assert(timer.value()->is_canceled().value());
}

void test_missed_period_realignment() {
    auto context = dmw::Context::create({});
    assert(context);
    auto steady = context.value()->create_clock(dmw::ClockType::Steady);
    assert(steady);
    auto timer = context.value()->create_timer(*steady.value(), dmw::TimerOptions{5ms, true});
    assert(timer);

    std::this_thread::sleep_for(30ms);
    dmw::TimerInfo info;
    const auto consumed = timer.value()->consume(info);
    assert(consumed && consumed.value());
    assert(info.elapsed_since_last_call >= 30ms);

    // The next call must land strictly in the future on the period grid
    // rather than reporting every missed cycle.
    const auto remaining = timer.value()->time_until_next_call();
    assert(remaining);
    assert(remaining.value() > 0ns);
    assert(remaining.value() <= 5ms);
}

void test_exchange_period() {
    auto context = dmw::Context::create({});
    assert(context);
    auto steady = context.value()->create_clock(dmw::ClockType::Steady);
    assert(steady);
    auto timer = context.value()->create_timer(*steady.value(), dmw::TimerOptions{5ms, true});
    assert(timer);

    const auto negative = timer.value()->exchange_period(-1ns);
    assert(!negative);
    assert(negative.error().code() == dmw::ErrorCode::InvalidArgument);

    const auto previous = timer.value()->exchange_period(2ms);
    assert(previous);
    assert(previous.value() == 5ms);
    assert(timer.value()->period() == 2ms);
}

void test_concurrent_consume_is_exactly_once() {
    auto context = dmw::Context::create({});
    assert(context);
    auto steady = context.value()->create_clock(dmw::ClockType::Steady);
    assert(steady);
    auto timer = context.value()->create_timer(*steady.value(), dmw::TimerOptions{200ms, true});
    assert(timer);
    std::this_thread::sleep_for(250ms);

    std::atomic<int> successes{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < 4; ++index) {
        threads.emplace_back([&timer, &successes] {
            dmw::TimerInfo info;
            const auto consumed = timer.value()->consume(info);
            if (consumed && consumed.value()) successes.fetch_add(1);
        });
    }
    for (auto& thread : threads) thread.join();
    assert(successes.load() == 1);
    assert(timer.value()->time_until_next_call().value() > 100ms);
}

void test_waitset_deadline_and_cancel() {
    auto context = dmw::Context::create({});
    assert(context);
    auto steady = context.value()->create_clock(dmw::ClockType::Steady);
    assert(steady);
    auto wait_set = context.value()->create_wait_set();
    assert(wait_set);

    auto fired = context.value()->create_timer(*steady.value(), dmw::TimerOptions{20ms, true});
    assert(fired);
    auto registration = wait_set.value()->add(*fired.value());
    assert(registration);
    assert(registration.value().kind() == dmw::WaitableKind::Timer);

    const auto start = std::chrono::steady_clock::now();
    const auto signalled = wait_set.value()->wait(dmw::WaitTimeout::infinite());
    assert(signalled);
    assert(signalled.value().status() == dmw::WaitStatus::Ready);
    assert(signalled.value().ready().size() == 1);
    assert(signalled.value().ready().front().kind == dmw::WaitableKind::Timer);
    assert(signalled.value().ready().front().detail_mask == dmw::kWaitableReadyBit);
    assert(std::chrono::steady_clock::now() - start >= 15ms);
    assert(wait_set.value()->remove(registration.value()));

    // A canceled timer contributes no native deadline, so a finite wait must
    // report Timeout rather than a spurious Ready.
    auto canceled = context.value()->create_timer(*steady.value(), dmw::TimerOptions{1ms, false});
    assert(canceled);
    auto canceled_registration = wait_set.value()->add(*canceled.value());
    assert(canceled_registration);
    const auto finite = dmw::WaitTimeout::finite(20ms);
    assert(finite);
    const auto timed_out = wait_set.value()->wait(finite.value());
    assert(timed_out);
    assert(timed_out.value().status() == dmw::WaitStatus::Timeout);
    assert(wait_set.value()->remove(canceled_registration.value()));

    // cancel() while registered must wake the active wait so it can recompute
    // its earliest deadline.
    auto looping = context.value()->create_timer(*steady.value(), dmw::TimerOptions{5s, true});
    assert(looping);
    auto looping_registration = wait_set.value()->add(*looping.value());
    assert(looping_registration);
    std::thread canceller([&looping] {
        std::this_thread::sleep_for(20ms);
        (void)looping.value()->cancel();
    });
    const auto cancel_timeout = dmw::WaitTimeout::finite(300ms);
    assert(cancel_timeout);
    const auto cancel_wait = wait_set.value()->wait(cancel_timeout.value());
    canceller.join();
    assert(cancel_wait);
    assert(cancel_wait.value().status() == dmw::WaitStatus::Timeout);
    assert(wait_set.value()->remove(looping_registration.value()));
}

void test_ros_clock_update_wakes_waitset() {
    auto context = dmw::Context::create({});
    assert(context);
    auto ros = context.value()->create_clock(dmw::ClockType::Ros);
    assert(ros);
    assert(ros.value()->enable_ros_time_override(true));
    assert(ros.value()->set_ros_time({0, dmw::ClockType::Ros}));

    auto timer = context.value()->create_timer(*ros.value(), dmw::TimerOptions{50ms, true});
    assert(timer);
    assert(timer.value()->clock_type() == dmw::ClockType::Ros);
    assert(!timer.value()->is_ready().value());

    auto wait_set = context.value()->create_wait_set();
    assert(wait_set);
    auto registration = wait_set.value()->add(*timer.value());
    assert(registration);

    std::thread updater([clock = ros.value().get()] {
        std::this_thread::sleep_for(10ms);
        (void)clock->set_ros_time({60'000'000, dmw::ClockType::Ros});
    });
    const auto signalled = wait_set.value()->wait(dmw::WaitTimeout::infinite());
    updater.join();
    assert(signalled);
    assert(signalled.value().status() == dmw::WaitStatus::Ready);
    assert(signalled.value().ready().front().kind == dmw::WaitableKind::Timer);
    assert(signalled.value().ready().front().detail_mask == dmw::kWaitableReadyBit);
    assert(timer.value()->is_ready().value());

    // Backward jumps only change time_until_next_call; consumed history is not
    // rolled back.
    const auto before = timer.value()->time_until_next_call();
    assert(before);
    assert(ros.value()->set_ros_time({10'000'000, dmw::ClockType::Ros}));
    const auto after = timer.value()->time_until_next_call();
    assert(after);
    assert(after.value() > before.value());
    assert(!timer.value()->is_ready().value());
    assert(wait_set.value()->remove(registration.value()));
}

void test_context_shutdown() {
    auto context = dmw::Context::create({});
    assert(context);
    auto steady = context.value()->create_clock(dmw::ClockType::Steady);
    assert(steady);
    auto timer = context.value()->create_timer(*steady.value(), dmw::TimerOptions{1ms, true});
    assert(timer);
    assert(context.value()->shutdown());
    const auto ready = timer.value()->is_ready();
    assert(!ready);
    assert(ready.error().code() == dmw::ErrorCode::ContextShutdown);
    assert(!context.value()->create_timer(*steady.value(), dmw::TimerOptions{1ms, true}));
}

}  // namespace

int main() {
    test_creation_contract();
    test_zero_period_is_always_ready();
    test_autostart_cancel_reset();
    test_missed_period_realignment();
    test_exchange_period();
    test_concurrent_consume_is_exactly_once();
    test_waitset_deadline_and_cancel();
    test_ros_clock_update_wakes_waitset();
    test_context_shutdown();
    return 0;
}
