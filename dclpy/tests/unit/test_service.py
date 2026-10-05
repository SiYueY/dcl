import time

import pytest

from dclpy import AsyncIOExecutor, Context, Node, SingleThreadedExecutor
from dclpy.exceptions import EntityClosedError
from std_srvs_dclpy.srv import Trigger


def _spin_until(executor, predicate, timeout=5.0):
    deadline = time.monotonic() + timeout
    while not predicate() and time.monotonic() < deadline:
        executor.spin_once(0.05)
    assert predicate()


def test_sync_service_request_response_and_call_time_snapshot():
    context = Context(domain_id=214)
    server_node = Node("sync_service_server", context=context)
    client_node = Node("sync_service_client", context=context)
    server_executor = SingleThreadedExecutor(context)
    client_executor = SingleThreadedExecutor(context)
    calls = []

    def callback(request, response):
        calls.append(request)
        response.success = True
        response.message = "served"
        # The ROS service convention also permits returning None after filling
        # the supplied response object.
        return None

    service = server_node.create_service(Trigger, "/dclpy_sync_trigger", callback)
    client = client_node.create_client(Trigger, "/dclpy_sync_trigger")
    server_executor.add_node(server_node)
    client_executor.add_node(client_node)
    try:
        deadline = time.monotonic() + 5.0
        while not client.service_is_ready() and time.monotonic() < deadline:
            server_executor.spin_once(0.05)
            client_executor.spin_once(0.05)
        assert client.service_is_ready()
        request = Trigger.Request()
        future = client.call_async(request)
        _spin_until(server_executor, lambda: bool(calls))
        _spin_until(client_executor, future.done)
        response = future.result()
        assert response.success is True
        assert response.message == "served"
        assert len(calls) == 1
    finally:
        server_executor.shutdown(timeout=5)
        client_executor.shutdown(timeout=5)
        context.shutdown(timeout=5)


def test_client_close_terminalizes_queued_or_registered_request():
    context = Context(domain_id=215)
    node = Node("client_close_test", context=context)
    executor = SingleThreadedExecutor(context)
    client = node.create_client(Trigger, "/dclpy_absent_service")
    executor.add_node(node)
    try:
        future = client.call_async(Trigger.Request())
        client.close()
        _spin_until(executor, future.done)
        with pytest.raises(EntityClosedError):
            future.result()
    finally:
        executor.shutdown(timeout=5)
        context.shutdown(timeout=5)


def test_async_service_coroutine_holds_response_ticket_until_completion():
    import asyncio

    async def run():
        context = Context(domain_id=216)
        server_node = Node("async_service_server", context=context)
        client_node = Node("async_service_client", context=context)
        server_executor = AsyncIOExecutor(context, max_service_tasks_per_service=1)
        client_executor = AsyncIOExecutor(context)
        entered = asyncio.Event()
        release = asyncio.Event()

        async def callback(request, response):
            entered.set()
            await release.wait()
            response.success = True
            response.message = "async served"
            return response

        service = server_node.create_service(Trigger, "/dclpy_async_trigger", callback)
        client = client_node.create_client(Trigger, "/dclpy_async_trigger")
        server_executor.add_node(server_node)
        client_executor.add_node(client_node)
        server_executor.start()
        client_executor.start()
        try:
            deadline = time.monotonic() + 5.0
            while not client.service_is_ready() and time.monotonic() < deadline:
                await asyncio.sleep(0.02)
            assert client.service_is_ready()
            future = client.call_async(Trigger.Request())
            await asyncio.wait_for(entered.wait(), 5)
            # Service work was accepted and already owns its response ticket,
            # while the coroutine intentionally delays the response.
            assert server_executor._dispatcher.outstanding(server_executor._port) == 1
            release.set()
            response = await asyncio.wait_for(future, 5)
            assert response.success and response.message == "async served"
            deadline = time.monotonic() + 5
            while server_executor._dispatcher.outstanding(server_executor._port) and time.monotonic() < deadline:
                await asyncio.sleep(0.01)
            assert server_executor._dispatcher.outstanding(server_executor._port) == 0
        finally:
            release.set()
            await server_executor.shutdown_async(timeout=5)
            await client_executor.shutdown_async(timeout=5)
            await context.shutdown_async(timeout=5)

    asyncio.run(run())
