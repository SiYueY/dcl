import asyncio
from concurrent.futures import ThreadPoolExecutor
import threading

import pytest

from dclpy import Future
from dclpy.exceptions import InvalidStateError


def test_one_terminal_commit_and_reentrant_callbacks():
    future = Future()
    terminal = []
    future.add_done_callback(lambda completed: terminal.append(completed.done()))
    future.add_done_callback(lambda completed: completed.add_done_callback(
        lambda inner: terminal.append(inner.done())))
    barrier = threading.Barrier(16)

    def contender(index):
        barrier.wait()
        return future.cancel() if index % 2 else future._try_set_result(index)

    with ThreadPoolExecutor(max_workers=16) as pool:
        winners = list(pool.map(contender, range(16)))
    assert sum(winners) == 1
    assert terminal == [True, True]
    assert future.done()
    with pytest.raises(InvalidStateError):
        future.set_result(100)


def test_waiter_cancellation_does_not_cancel_operation():
    async def run():
        future = Future()

        async def waiter():
            return await future

        first = asyncio.create_task(waiter())
        second = asyncio.create_task(waiter())
        await asyncio.sleep(0)
        first.cancel()
        with pytest.raises(asyncio.CancelledError):
            await first
        assert not future.done()
        future.set_result("response")
        assert await second == "response"
        assert await future == "response"
    asyncio.run(run())


def test_cancellation_and_base_exception_propagation():
    async def run():
        future = Future()
        future.cancel()
        with pytest.raises(asyncio.CancelledError):
            await future
        error = asyncio.CancelledError("callback canceled")
        other = Future()
        other.set_exception(error)
        assert not other.cancelled()
        assert other.exception() is error
        with pytest.raises(asyncio.CancelledError, match="callback canceled"):
            await other
    asyncio.run(run())


def test_callbacks_use_owner_control_scheduler():
    class Owner:
        def __init__(self):
            self.callbacks = []

        def _register_future(self, future):
            self.future = future

        def _begin_future_notification(self):
            pass

        def _end_future_notification(self):
            pass

        def _schedule_done_callback(self, callback, future):
            self.callbacks.append((callback, future))

    owner = Owner()
    future = Future(executor=owner)
    called = []
    future.add_done_callback(lambda completed: called.append(completed.result()))
    with ThreadPoolExecutor(max_workers=1) as pool:
        pool.submit(future.set_result, 7).result()
    assert called == []
    callback, completed = owner.callbacks.pop()
    completed._invoke(callback)
    assert called == [7]


def test_closed_waiter_loop_does_not_change_terminal_state():
    loop = asyncio.new_event_loop()
    future = Future()
    waiter = loop.create_task(future._wait_async())
    loop.run_until_complete(asyncio.sleep(0))
    # Exercise the documented closed-loop completion path without abandoning
    # a pending Task: close scheduling after the bridge has registered.
    original = loop.call_soon_threadsafe

    def closed(*args, **kwargs):
        raise RuntimeError("Event loop is closed")

    loop.call_soon_threadsafe = closed
    try:
        future.set_result(9)
        assert future.result() == 9
    finally:
        loop.call_soon_threadsafe = original
        waiter.cancel()
        loop.run_until_complete(asyncio.gather(waiter, return_exceptions=True))
        loop.close()
