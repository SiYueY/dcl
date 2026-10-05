import asyncio
import threading

import pytest

from dclpy import ActionClient, ActionServer, Context, Node, AsyncIOExecutor, ExecutorState
from dclpy.exceptions import DclpyTimeoutError, InvalidStateError
from mfr3duo_msgs_dclpy.action import Move
from std_msgs_dclpy.msg import String


def test_callback_owner_thread_and_managed_self_wait_rejected_before_mutation():
    async def run():
        context = Context(domain_id=209)
        node = Node("async_owner_test", context=context)
        executor = AsyncIOExecutor(context)
        received = asyncio.Event()
        callback_threads = []
        async def callback(message):
            callback_threads.append(threading.get_ident())
            with pytest.raises(InvalidStateError):
                await executor.shutdown_async()
            assert executor.state is ExecutorState.RUNNING
            with pytest.raises(InvalidStateError):
                await context.shutdown_async()
            assert context.ok()
            received.set()
        node.create_subscription(String, "/async_owner_topic", callback, 10)
        publisher = node.create_publisher(String, "/async_owner_topic", 10)
        executor.add_node(node)
        executor.start()
        try:
            async with asyncio.timeout(5):
                while not received.is_set():
                    await publisher.publish_async(String(data="owner loop"))
                    await asyncio.sleep(0.05)
            assert callback_threads and set(callback_threads) == {threading.get_ident()}
            assert await executor.shutdown_async(timeout=5)
            assert executor._wait_thread.is_alive() is False
        finally:
            await executor.shutdown_async(timeout=5)
            await context.shutdown_async(timeout=5)
    asyncio.run(run())


def test_shutdown_timeout_keeps_task_work_lease_and_one_shielded_drain():
    async def run():
        context = Context(domain_id=210)
        node = Node("async_shutdown_test", context=context)
        executor = AsyncIOExecutor(context, max_callback_tasks=1)
        started, cleaning, release = asyncio.Event(), asyncio.Event(), asyncio.Event()
        async def callback(message):
            started.set()
            try:
                await asyncio.Event().wait()
            finally:
                cleaning.set()
                await release.wait()
        subscriber = node.create_subscription(String, "/async_shutdown_topic", callback, 10)
        publisher = node.create_publisher(String, "/async_shutdown_topic", 10)
        executor.add_node(node)
        executor.start()
        try:
            async with asyncio.timeout(5):
                while not started.is_set():
                    await publisher.publish_async(String(data="task"))
                    await asyncio.sleep(0.05)
            subscriber.close()
            with pytest.raises(DclpyTimeoutError):
                await executor.shutdown_async(timeout=0.02)
            await asyncio.wait_for(cleaning.wait(), 5)
            drain = executor._drain_task
            assert not subscriber.wait_closed(0)
            assert executor.state is ExecutorState.STOPPING
            release.set()
            assert await executor.shutdown_async(timeout=5)
            assert executor._drain_task is drain
            assert subscriber.wait_closed(1)
        finally:
            release.set()
            await executor.shutdown_async(timeout=5)
            await context.shutdown_async(timeout=5)
    asyncio.run(run())


def test_context_closes_attached_children_before_owner_barrier_and_registry_unload():
    async def run():
        context = Context(domain_id=211)
        node = Node("async_context_test", context=context)
        executor = AsyncIOExecutor(context)
        publisher = node.create_publisher(String, "/async_context_topic", 10)
        executor.add_node(node)
        executor.start()
        futures = [publisher.publish_async(String(data=str(index))) for index in range(20)]
        assert await context.shutdown_async(timeout=5)
        assert all(future.done() for future in futures)
        assert executor.state is ExecutorState.STOPPED
        assert publisher.wait_closed(0)
        assert not executor._wait_thread.is_alive()
    asyncio.run(run())


def test_callback_capacity_does_not_hold_ack_or_native_completion_progress():
    async def run():
        context = Context(domain_id=212)
        node = Node("capacity_test", context=context)
        executor = AsyncIOExecutor(context, max_callback_tasks=1)
        matched, first, release = asyncio.Event(), asyncio.Event(), asyncio.Event()
        messages = []
        async def callback(message):
            if message.data == "discovery":
                matched.set()
                return
            messages.append(message.data)
            first.set()
            await release.wait()
        node.create_subscription(String, "/capacity_topic", callback, 10)
        publisher = node.create_publisher(String, "/capacity_topic", 10)
        executor.add_node(node)
        executor.start()
        try:
            async with asyncio.timeout(5):
                while not matched.is_set():
                    await publisher.publish_async(String(data="discovery"))
                    await asyncio.sleep(0.05)
                for value in ("one", "two", "three"):
                    await publisher.publish_async(String(data=value))
                await first.wait()
                await asyncio.sleep(0.05)
                assert messages == ["one"]
                # A blocked user Task has ACKed its ready batch already;
                # subsequent native I/O completes through the control route.
                assert await publisher.publish_async(String(data="four")) is None
                release.set()
                while len(messages) != 4:
                    await asyncio.sleep(0.01)
            assert messages == ["one", "two", "three", "four"]
        finally:
            release.set()
            await executor.shutdown_async(timeout=5)
            await context.shutdown_async(timeout=5)
    asyncio.run(run())


def test_context_shutdown_joins_two_owners_on_different_event_loops():
    async def run():
        foreign = asyncio.new_event_loop()
        ready = threading.Event()
        def foreign_main():
            asyncio.set_event_loop(foreign)
            foreign.call_soon(ready.set)
            foreign.run_forever()
        thread = threading.Thread(target=foreign_main, name="test-foreign-owner")
        thread.start()
        await asyncio.to_thread(ready.wait)
        context = Context(domain_id=213)
        local_owner = AsyncIOExecutor(context)
        foreign_owner = AsyncIOExecutor(context, loop=foreign)
        local_node = Node("local_owner", context=context)
        foreign_node = Node("foreign_owner", context=context)
        local_publisher = local_node.create_publisher(String, "/local_owner_topic", 10)
        foreign_publisher = foreign_node.create_publisher(String, "/foreign_owner_topic", 10)
        local_owner.add_node(local_node)
        foreign_owner.add_node(foreign_node)
        local_owner.start()
        async def start_foreign():
            foreign_owner.start()
        await asyncio.wrap_future(asyncio.run_coroutine_threadsafe(start_foreign(), foreign))
        called = []
        try:
            local_future = local_publisher.publish_async(String(data="local"))
            foreign_future = foreign_publisher.publish_async(String(data="foreign"))
            local_future.add_done_callback(lambda done: called.append(("local", threading.get_ident())))
            foreign_future.add_done_callback(lambda done: called.append(("foreign", threading.get_ident())))
            assert await context.shutdown_async(timeout=5)
            assert local_future.done() and foreign_future.done()
            assert local_owner.state is foreign_owner.state is ExecutorState.STOPPED
            assert set(called) == {("local", threading.get_ident()), ("foreign", thread.ident)}
            assert local_publisher.wait_closed(0) and foreign_publisher.wait_closed(0)
        finally:
            await context.shutdown_async(timeout=5)
            foreign.call_soon_threadsafe(foreign.stop)
            await asyncio.to_thread(thread.join)
            foreign.close()
    asyncio.run(run())


def test_async_action_execute_keeps_work_lease_until_terminal_cleanup():
    async def run():
        context = Context(domain_id=216)
        server_node = Node("async_action_server", context=context)
        client_node = Node("async_action_client", context=context)
        entered = asyncio.Event()

        async def execute(handle):
            entered.set()
            await asyncio.sleep(0)
            return Move.Result()

        ActionServer(server_node, Move, "/async_action", execute)
        client = ActionClient(client_node, Move, "/async_action")
        executor = AsyncIOExecutor(context)
        executor.add_node(server_node)
        executor.add_node(client_node)
        executor.start()
        try:
            goal = await client.send_goal_async(Move.Goal())
            await asyncio.wait_for(entered.wait(), 5)
            result = await goal.get_result_async()
            assert result.status == 6
            assert await executor.shutdown_async(timeout=5)
        finally:
            await executor.shutdown_async(timeout=5)
            await context.shutdown_async(timeout=5)
    asyncio.run(run())


def test_async_action_pending_result_is_delivered_after_terminal_transition():
    async def run():
        context = Context(domain_id=218)
        server_node = Node("pending_result_server", context=context)
        client_node = Node("pending_result_client", context=context)
        release = asyncio.Event()

        async def execute(handle):
            await release.wait()
            return Move.Result()

        ActionServer(server_node, Move, "/pending_result", execute)
        client = ActionClient(client_node, Move, "/pending_result")
        executor = AsyncIOExecutor(context)
        executor.add_node(server_node)
        executor.add_node(client_node)
        executor.start()
        try:
            goal = await client.send_goal_async(Move.Goal())
            result = goal.get_result_async()
            await asyncio.sleep(0.05)
            assert not result.done()
            release.set()
            assert (await asyncio.wait_for(result, 5)).status == 6
        finally:
            release.set()
            await executor.shutdown_async(timeout=5)
            await context.shutdown_async(timeout=5)
    asyncio.run(run())
