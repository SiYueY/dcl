import time

import pytest

from dclpy import Context, Node, SingleThreadedExecutor
from dclpy import _dclpy as native
from dclpy.exceptions import ResourceExhaustedError, BusyError
from std_msgs_dclpy.msg import String


def test_native_ticket_quota_counts_reservations_and_reserved_control_capacity():
    context = Context(domain_id=206)
    wait_set = native._WaitSet(context._native)
    dispatcher = context._get_dispatcher()
    port = dispatcher.open_route(wait_set)
    tickets = []
    record = native._Completion()
    try:
        for _ in range(224):
            tickets.append(dispatcher.reserve(port, record, False))
        with pytest.raises(ResourceExhaustedError):
            dispatcher.reserve(port, record, False)
        record.kind = native._JobKind.CANCEL_RESPONSE
        for _ in range(32):
            tickets.append(dispatcher.reserve(port, record, True))
        assert dispatcher.outstanding(port) == 256
        with pytest.raises(ResourceExhaustedError):
            dispatcher.reserve(port, record, True)
        with pytest.raises(BusyError):
            dispatcher.retire_route(port)
        released = tickets.pop()
        dispatcher.abandon(released)
        # A free control ticket does not expand the 224 ordinary quota.
        with pytest.raises(ResourceExhaustedError):
            dispatcher.reserve(port, record, False)
    finally:
        for ticket in tickets:
            dispatcher.abandon(ticket)
        dispatcher.retire_route(port)
        wait_set.close()
        context.shutdown(timeout=5)


def test_async_publish_snapshot_and_completion_work_lease():
    context = Context(domain_id=207)
    node = Node("async_publish_test", context=context)
    executor = SingleThreadedExecutor(context)
    messages = []
    node.create_subscription(String, "/async_publish", messages.append, 10)
    publisher = node.create_publisher(String, "/async_publish", 10)
    executor.add_node(node)
    try:
        # Establish a real DDS reader/writer match before the snapshot check.
        deadline = time.monotonic() + 5
        while not messages and time.monotonic() < deadline:
            publisher.publish(String(data="discovery"))
            executor.spin_once(0.1)
        assert messages
        messages.clear()
        message = String(data="frozen")
        future = publisher.publish_async(message)
        message.data = "mutated"
        deadline = time.monotonic() + 5
        while (not future.done() or not messages) and time.monotonic() < deadline:
            executor.spin_once(0.1)
        assert future.result() is None
        assert any(value.data == "frozen" for value in messages)
        # Completing the DDS write does not free the ticket before its ACK.
        assert executor._dispatcher.outstanding(executor._port) == 0
        assert executor.shutdown(timeout=5)
    finally:
        executor.shutdown(timeout=5)
        context.shutdown(timeout=5)
