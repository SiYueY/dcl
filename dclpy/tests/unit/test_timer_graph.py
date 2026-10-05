import time

import pytest

from dclpy import ClockType, Context, SingleThreadedExecutor
from dclpy.exceptions import InvalidStateError
from dclpy.node import Node


def test_clock_ros_override_and_timer_consumption():
    context = Context(participant_name="dclpy-timer-test")
    try:
        clock = context.create_clock(ClockType.ROS)
        clock.enable_ros_time_override(True)
        from dclpy import Time
        clock.set_ros_time(Time(123, clock_type=ClockType.ROS))
        assert clock.now().nanoseconds == 123

        node = Node("timer_node", context=context)
        fired = []
        timer = node.create_timer(0.001, fired.append)
        executor = SingleThreadedExecutor(context)
        executor.add_node(node)
        deadline = time.monotonic() + 1.0
        while not fired and time.monotonic() < deadline:
            executor.spin_once(0.05)
        assert fired
        assert fired[0].actual_call_time.clock_type == ClockType.ROS.value
        timer.cancel()
        assert timer.is_canceled()
        timer.reset()
        assert not timer.is_canceled()
        executor.request_shutdown()
        executor.shutdown()
    finally:
        context.shutdown()


def test_manual_and_executor_graph_event_modes_are_exclusive():
    context = Context(participant_name="dclpy-graph-test")
    try:
        event = context.create_graph_event()
        assert event.take() is None
        snapshot = context.get_graph_snapshot()
        assert snapshot.revision == context.get_graph_revision()

        node = Node("graph_node", context=context)
        changes = []
        managed = node.create_graph_event(changes.append)
        with pytest.raises(InvalidStateError):
            managed.take()
        executor = SingleThreadedExecutor(context)
        executor.add_node(node)
        peer = Node("graph_peer", context=context)
        deadline = time.monotonic() + 1.0
        while not changes and time.monotonic() < deadline:
            executor.spin_once(0.05)
        assert changes
        assert changes[0].current_revision > changes[0].previous_revision
        peer.close()
        executor.request_shutdown()
        executor.shutdown()
    finally:
        context.shutdown()
