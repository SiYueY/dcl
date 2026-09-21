// An ordinary DMW consumer links dmw::dmw only: it must be able to use the
// runtime API without including any Fast DDS header (dmw.md §11.5).

#include <cassert>

#include "dmw/clock.hpp"
#include "dmw/context.hpp"
#include "dmw/graph.hpp"
#include "dmw/graph_event.hpp"
#include "dmw/guard_condition.hpp"
#include "dmw/node.hpp"
#include "dmw/parameter.hpp"
#include "dmw/timer.hpp"
#include "dmw/wait_set.hpp"

int main() {
    dmw::ContextOptions options;
    options.runtime_mode = dmw::RuntimeMode::DDS;
    auto context = dmw::Context::create(options);
    assert(context);
    assert(context.value()->runtime_mode() == dmw::RuntimeMode::DDS);
    assert(context.value()->domain_id() == 0);

    dmw::NodeOptions node_options;
    node_options.node_name = "installed_consumer";
    auto node = context.value()->create_node(node_options);
    assert(node);
    assert(node.value()->fully_qualified_name() == "/installed_consumer");
    assert(node.value()->declare_parameter("gain", dmw::ParameterValue::make_integer(2)));
    const auto gain = node.value()->get_parameter("gain");
    assert(gain && gain.value().value.as_integer() == 2);

    auto clock = context.value()->create_clock(dmw::ClockType::Steady);
    assert(clock && clock.value()->now());
    // Note: `std::chrono::nanoseconds` has no implicit conversion from an
    // integer, so a zero period is spelled with the default-constructed
    // options (or an explicit `std::chrono::nanoseconds{...}`).
    auto timer = context.value()->create_timer(*clock.value(), dmw::TimerOptions{});
    assert(timer);
    const auto ready = timer.value()->is_ready();
    assert(ready && ready.value());

    auto wait_set = context.value()->create_wait_set();
    auto guard = context.value()->create_guard_condition();
    auto event = context.value()->create_graph_event();
    assert(wait_set && guard && event);
    const auto token = wait_set.value()->add(*guard.value());
    assert(token);
    assert(guard.value()->trigger());
    const auto signalled = wait_set.value()->wait(dmw::WaitTimeout::poll());
    assert(signalled);
    assert(signalled.value().status() == dmw::WaitStatus::Ready);
    assert(signalled.value().ready().front().detail_mask == dmw::kWaitableReadyBit);
    assert(wait_set.value()->remove(token.value()));

    const auto snapshot = context.value()->graph_snapshot();
    assert(snapshot);
    assert(snapshot.value().revision == context.value()->graph_revision().value());
    bool saw_local_node = false;
    for (const auto& graph_node : snapshot.value().nodes) {
        if (graph_node.node_name == "installed_consumer") saw_local_node = true;
    }
    assert(saw_local_node);

    assert(context.value()->shutdown());
    return 0;
}
