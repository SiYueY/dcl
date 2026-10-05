from collections import OrderedDict
import uuid
from types import SimpleNamespace
import time

import pytest

from dclpy import ActionClient, ActionServer, Context, GoalResponse
from dclpy import _dclpy as native
from dclpy.action import ClientGoalHandle
from dclpy.exceptions import EntityClosedError, InvalidArgumentError, InvalidStateError
from dclpy.executors import SingleThreadedExecutor
from dclpy.future import Future
from dclpy.node import Node
from mfr3duo_msgs_dclpy.action import Grasp, Move


def test_action_client_goal_admission_cancellation_and_shutdown():
    context = Context(participant_name="dclpy-action-client-test")
    try:
        node = Node("action_client_node", context=context)
        client = ActionClient(node, Move, "/move")
        executor = SingleThreadedExecutor(context)
        executor.add_node(node)
        future = client.send_goal_async(Move.Goal(width=0.01, speed=0.02))
        assert future.cancel()
        executor.spin_once(0.1)
        executor.request_shutdown()
        assert executor.shutdown()
    finally:
        context.shutdown()


def test_action_client_rejects_invalid_and_duplicate_goal_ids():
    context = Context(participant_name="dclpy-action-goal-id-test")
    try:
        node = Node("action_goal_id_node", context=context)
        client = ActionClient(node, Move, "/move")
        executor = SingleThreadedExecutor(context)
        executor.add_node(node)
        goal_id = uuid.uuid4()
        client.send_goal_async(Move.Goal(), goal_id=goal_id)
        with pytest.raises(InvalidArgumentError):
            client.send_goal_async(Move.Goal(), goal_id=goal_id)
        with pytest.raises(InvalidArgumentError):
            client.send_goal_async(Move.Goal(), goal_id=b"short")
        executor.request_shutdown()
        assert executor.shutdown()
    finally:
        context.shutdown()


def test_action_client_requires_an_executor_and_rejects_goal_handle_calls_after_close():
    context = Context(domain_id=223, participant_name="dclpy-action-admission-test")
    try:
        node = Node("action_admission_node", context=context)
        client = ActionClient(node, Move, "/action_admission")
        with pytest.raises(InvalidStateError, match="attached executor"):
            client.send_goal_async(Move.Goal())
        client.close()
        handle = ClientGoalHandle(client, uuid.uuid4(), accepted=True)
        with pytest.raises(EntityClosedError):
            handle.get_result_async()
        with pytest.raises(EntityClosedError):
            handle.cancel_goal_async()
    finally:
        context.shutdown(timeout=5)


def test_action_client_routes_feedback_and_prevents_terminal_status_downgrade():
    identifier = uuid.uuid4()
    delivered = []

    class Native:
        receive_goal_response = staticmethod(lambda pin: None)
        receive_cancel_response = staticmethod(lambda pin: None)
        receive_result_response = staticmethod(lambda pin: None)
        receive_feedback = staticmethod(lambda pin: SimpleNamespace(
            goal_id=SimpleNamespace(uuid=list(identifier.bytes))))
        receive_status = staticmethod(lambda pin: SimpleNamespace(status_list=[
            SimpleNamespace(goal_info=SimpleNamespace(goal_id=SimpleNamespace(uuid=list(identifier.bytes))),
                            status=int(native.GoalState.EXECUTING))]))

    client = ActionClient.__new__(ActionClient)
    client._native = Native()
    client._records = {identifier: ClientGoalHandle(client, identifier, True, native.GoalState.SUCCEEDED)}
    client._feedback_callbacks = {identifier: delivered.append}
    client._take_ready(None, SimpleNamespace(_schedule_action_feedback=lambda entity, value: None))

    assert len(delivered) == 1
    assert client._records[identifier].status == native.GoalState.SUCCEEDED


def test_action_client_retires_tracking_when_goal_send_is_canceled():
    identifier = uuid.uuid4()
    client = ActionClient.__new__(ActionClient)
    client._records = {identifier: ClientGoalHandle(client, identifier)}
    client._feedback_callbacks = {identifier: lambda message: None}
    client._early = {}
    client._tombstones = OrderedDict()
    client._unreconciled = {4}
    work = type("Work", (), {"cancel": lambda self: None})()
    future = type("Future", (), {"_action_goal_id": identifier, "_action_operation": "goal"})()
    client._cancel_operation(4, type("Operation", (), {"native_work": work, "future": future})())

    assert client._records == {}
    assert client._feedback_callbacks == {}
    assert client._unreconciled == set()


def test_action_client_reconciles_a_goal_response_that_precedes_send_completion():
    identifier = uuid.uuid4()
    request_id = object()
    response = SimpleNamespace(accepted=True)
    result = []

    class Native:
        def __init__(self):
            self.goal = True

        def receive_goal_response(self, pin):
            if self.goal:
                self.goal = False
                return request_id, response
            return None

        receive_cancel_response = staticmethod(lambda pin: None)
        receive_result_response = staticmethod(lambda pin: None)

    future = Future()
    future._action_goal_id = identifier
    future._action_operation = "goal"
    operation = SimpleNamespace(future=future)
    client = ActionClient.__new__(ActionClient)
    client._native = Native()
    client._records = {identifier: ClientGoalHandle(client, identifier)}
    client._pending = {}
    client._early = {}
    client._unreconciled = {9}
    client._tombstones = OrderedDict()
    client._feedback_callbacks = {}
    client._closing = False
    client._owner = SimpleNamespace(
        _operations={9: SimpleNamespace(native_work=SimpleNamespace(send_started=True))},
        _complete_operation=lambda operation_id, value: result.append((operation_id, value)),
        _fail_operation=lambda operation_id, error: pytest.fail(str(error)))

    client._take_response(None)
    assert request_id in client._early

    completion = SimpleNamespace(
        request_id=request_id, kind=native._JobKind.GOAL_REQUEST, skipped=False,
        check=lambda: None)
    assert client._send_committed(9, operation, completion) is False
    assert result == [(9, client._records[identifier])]
    assert client._records[identifier].accepted
    assert client._early == {}


def test_action_server_request_work_delivers_rejected_goal_response():
    context = Context(domain_id=211, participant_name="dclpy-action-work-test")
    wait_set = None
    server = None
    executor = None
    try:
        server_node = Node("action_work_server", context=context)
        client_node = Node("action_work_client", context=context)
        server = native._ActionServer(server_node._native, context._bindings, Move, "/action_work", 1_000_000_000)
        wait_set = native._WaitSet(context._native)
        registration = wait_set.add(server, 1)
        client = ActionClient(client_node, Move, "/action_work")
        executor = SingleThreadedExecutor(context)
        executor.add_node(client_node)
        future = client.send_goal_async(Move.Goal())

        request = None
        pin = None
        deadline = time.monotonic() + 5
        while request is None and time.monotonic() < deadline:
            executor.spin_once(0.02)
            batch = wait_set.wait(0.02)
            try:
                for ready in batch.entries:
                    if ready.registration == registration:
                        pin = ready.pin
                        dispatcher = context._get_dispatcher()
                        port = dispatcher.open_route(wait_set)
                        completion = native._Completion()
                        completion.entity_id = server.entity_id
                        work = native._ActionServerRequestWork(
                            dispatcher, port, completion, server, native._ActionServerRequestKind.GOAL)
                        request = work.receive(server, pin)
                        if request is not None:
                            assert work.request_id.sequence_number > 0
                            response = Move.Impl.SendGoalService.Response()
                            response.accepted = False
                            work.respond(server, response)
                        work.release()
                        while dispatcher.peek(port) is None and time.monotonic() < deadline:
                            time.sleep(0.001)
                        ticket, completion = dispatcher.peek(port)
                        completion.check()
                        dispatcher.acknowledge(port, ticket)
                        dispatcher.retire_route(port)
            finally:
                batch.release()
        assert request is not None
        while not future.done() and time.monotonic() < deadline:
            executor.spin_once(0.02)
        assert future.result().accepted is False
    finally:
        if executor is not None:
            executor.shutdown(timeout=5)
        if wait_set is not None:
            wait_set.close()
        if server is not None:
            server.close()
            server.wait_closed(5)
            server.retire_binding()
        context.shutdown(timeout=5)


def test_action_result_payload_rejects_wrong_response_type():
    context = Context(domain_id=212, participant_name="dclpy-action-result-payload-test")
    server = None
    payload = None
    try:
        node = Node("action_result_payload_server", context=context)
        server = native._ActionServer(node._native, context._bindings, Move, "/action_result_payload", 1_000_000_000)
        payload = native._ActionResultPayload(server, Move.Impl.GetResultService.Response())
        assert payload is not None
        with pytest.raises(Exception):
            native._ActionResultPayload(server, Move.Impl.SendGoalService.Response())
    finally:
        if server is not None:
            server.close()
            server.wait_closed(5)
            server.retire_binding()
        del payload
        context.shutdown(timeout=5)


def test_action_server_rejects_goal_through_protocol_lane():
    context = Context(domain_id=213, participant_name="dclpy-action-server-reject-test")
    executor = None
    try:
        server_node = Node("action_server_reject_server", context=context)
        client_node = Node("action_server_reject_client", context=context)
        ActionServer(server_node, Move, "/action_server_reject", lambda handle: Move.Result(),
                     goal_callback=lambda goal: GoalResponse.REJECT)
        client = ActionClient(client_node, Move, "/action_server_reject")
        executor = SingleThreadedExecutor(context)
        executor.add_node(server_node)
        executor.add_node(client_node)
        future = client.send_goal_async(Move.Goal())
        deadline = time.monotonic() + 5
        while not future.done() and time.monotonic() < deadline:
            executor.spin_once(0.02)
        assert future.result().accepted is False
    finally:
        if executor is not None:
            executor.shutdown(timeout=5)
        context.shutdown(timeout=5)


def test_action_server_commits_goal_before_running_execute_callback():
    context = Context(domain_id=214, participant_name="dclpy-action-server-accept-test")
    executor = None
    observed = []
    try:
        server_node = Node("action_server_accept_server", context=context)
        client_node = Node("action_server_accept_client", context=context)

        def execute(handle):
            observed.append(handle.status)
            return Move.Result()

        ActionServer(server_node, Move, "/action_server_accept", execute)
        client = ActionClient(client_node, Move, "/action_server_accept")
        executor = SingleThreadedExecutor(context)
        executor.add_node(server_node)
        executor.add_node(client_node)
        future = client.send_goal_async(Move.Goal())
        deadline = time.monotonic() + 5
        while (not future.done() or not observed) and time.monotonic() < deadline:
            executor.spin_once(0.02)
        assert future.result().accepted is True
        assert observed == [native.GoalState.EXECUTING]
    finally:
        if executor is not None:
            executor.shutdown(timeout=5)
        context.shutdown(timeout=5)


def test_action_server_delivers_retained_terminal_result():
    context = Context(domain_id=215, participant_name="dclpy-action-server-result-test")
    executor = None
    try:
        server_node = Node("action_server_result_server", context=context)
        client_node = Node("action_server_result_client", context=context)
        ActionServer(server_node, Move, "/action_server_result", lambda handle: Move.Result())
        client = ActionClient(client_node, Move, "/action_server_result")
        executor = SingleThreadedExecutor(context)
        executor.add_node(server_node)
        executor.add_node(client_node)
        goal_future = client.send_goal_async(Move.Goal())
        deadline = time.monotonic() + 5
        while not goal_future.done() and time.monotonic() < deadline:
            executor.spin_once(0.02)
        handle = goal_future.result()
        result_future = handle.get_result_async()
        while not result_future.done() and time.monotonic() < deadline:
            executor.spin_once(0.02)
        assert result_future.result().status == int(native.GoalState.ABORTED)
    finally:
        if executor is not None:
            executor.shutdown(timeout=5)
        context.shutdown(timeout=5)


def test_action_server_expiry_turns_late_result_request_into_unknown_goal():
    context = Context(domain_id=217, participant_name="dclpy-action-expiry-test")
    executor = None
    try:
        server_node = Node("action_expiry_server", context=context)
        client_node = Node("action_expiry_client", context=context)
        ActionServer(server_node, Move, "/action_expiry", lambda handle: Move.Result(),
                     result_timeout_sec=0.01)
        client = ActionClient(client_node, Move, "/action_expiry")
        executor = SingleThreadedExecutor(context)
        executor.add_node(server_node)
        executor.add_node(client_node)
        goal = client.send_goal_async(Move.Goal())
        deadline = time.monotonic() + 5
        while not goal.done() and time.monotonic() < deadline:
            executor.spin_once(0.02)
        handle = goal.result()
        time.sleep(0.05)
        for _ in range(10):
            executor.spin_once(0.02)
        result = handle.get_result_async()
        while not result.done() and time.monotonic() < deadline:
            executor.spin_once(0.02)
        assert result.result().status == int(native.GoalState.UNKNOWN)
    finally:
        if executor is not None:
            executor.shutdown(timeout=5)
        context.shutdown(timeout=5)


def test_action_server_round_trips_mfr3duo_grasp():
    context = Context(domain_id=219, participant_name="dclpy-grasp-round-trip-test")
    executor = None
    try:
        server_node = Node("grasp_server", context=context)
        client_node = Node("grasp_client", context=context)

        def execute(handle):
            result = Grasp.Result()
            result.success = True
            return result

        ActionServer(server_node, Grasp, "/left_gripper_controller/grasp", execute)
        client = ActionClient(client_node, Grasp, "/left_gripper_controller/grasp")
        executor = SingleThreadedExecutor(context)
        executor.add_node(server_node)
        executor.add_node(client_node)
        goal = Grasp.Goal(width=0.02, speed=0.03, force=2.0)
        goal.epsilon.inner = 0.001
        goal.epsilon.outer = 0.002
        goal_future = client.send_goal_async(goal)
        deadline = time.monotonic() + 5
        while not goal_future.done() and time.monotonic() < deadline:
            executor.spin_once(0.02)
        handle = goal_future.result()
        result_future = handle.get_result_async()
        while not result_future.done() and time.monotonic() < deadline:
            executor.spin_once(0.02)
        result = result_future.result()
        assert result.status == int(native.GoalState.ABORTED)
        assert result.result.success is True
    finally:
        if executor is not None:
            executor.shutdown(timeout=5)
        context.shutdown(timeout=5)


def test_action_server_coalesces_status_revisions_until_current_delivery_completes():
    server = ActionServer.__new__(ActionServer)
    server._closing = False
    server._status_revision = 0
    server._status_inflight_ticket = 91
    server._status_inflight_revision = 1
    server._assert_owner_thread = lambda: None
    started = []
    server._start_status_publication = lambda: started.append(server._status_revision)

    # A second state change does not allocate another native Status job while
    # the first snapshot owns the bounded protocol slot.
    server._publish_status()
    assert server._status_revision == 1
    assert started == []

    server._status_revision = 2
    server._status_completion(91, SimpleNamespace(check=lambda: None))
    assert started == [2]
