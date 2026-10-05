from concurrent.futures import ThreadPoolExecutor
import threading
import time

import pytest

from dclpy import Context, Node, Future, SingleThreadedExecutor, ExecutorState
from dclpy.exceptions import BusyError, DclpyTimeoutError, ExecutorStoppedError
from std_msgs_dclpy.msg import String


def test_bounded_topic_dispatch_and_owner_stop():
    context = Context(domain_id=203)
    node = Node("executor_test", context=context)
    executor = SingleThreadedExecutor(context)
    messages = []
    subscriber = node.create_subscription(String, "/executor_topic", messages.append, 10)
    publisher = node.create_publisher(String, "/executor_topic", 10)
    executor.add_node(node)
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not messages:
            publisher.publish(String(data="first"))
            before = len(messages)
            executor.spin_once(0.1)
            assert len(messages) - before <= 1
        assert messages[0].data == "first"
        assert executor.shutdown(timeout=5)
        assert executor.state is ExecutorState.STOPPED
        with pytest.raises(ExecutorStoppedError):
            executor.spin_once(0)
        assert subscriber._native.state.name == "OPEN"
        publisher.publish(String(data="node survives"))
    finally:
        executor.shutdown(timeout=5)
        context.shutdown(timeout=5)


def test_terminal_publication_gap_blocks_stop_and_runs_callbacks_on_control_owner():
    context = Context(domain_id=204)
    executor = SingleThreadedExecutor(context)
    future = Future(executor=executor)
    entered, proceed = threading.Event(), threading.Event()
    callback_threads = []
    future.add_done_callback(lambda completed: callback_threads.append(threading.get_ident()))
    original = executor._schedule_done_callback

    def blocked(callback, completed):
        entered.set()
        assert proceed.wait(5)
        original(callback, completed)

    executor._schedule_done_callback = blocked
    try:
        with ThreadPoolExecutor(max_workers=1) as pool:
            publisher = pool.submit(future.set_result, 17)
            assert entered.wait(5)
            assert future.result() == 17
            with pytest.raises(DclpyTimeoutError):
                executor.shutdown(timeout=0.01)
            assert executor.state is ExecutorState.STOPPING
            proceed.set()
            publisher.result(timeout=5)
        assert executor.shutdown(timeout=5)
        assert callback_threads == [threading.get_ident()]
        # Await bridges do not require the retired owner notification route.
        import asyncio
        async def await_terminal():
            return await future
        assert asyncio.run(await_terminal()) == 17
    finally:
        proceed.set()
        executor.shutdown(timeout=5)
        context.shutdown(timeout=5)


def test_remove_node_busy_with_pending_future():
    context = Context(domain_id=205)
    node = Node("busy_test", context=context)
    executor = SingleThreadedExecutor(context)
    executor.add_node(node)
    future = Future(executor=executor)
    try:
        with pytest.raises(BusyError):
            executor.remove_node(node)
        future.set_result(None)
        executor.spin_once(0)
        assert executor.remove_node(node)
    finally:
        executor.shutdown(timeout=5)
        context.shutdown(timeout=5)
