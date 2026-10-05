"""Action client primitives backed by DMW aggregate Action endpoints."""
from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass
from enum import Enum
import inspect
import logging
import time
import uuid
from typing import Any

from . import _dclpy as _native
from ._entity import Entity
from .exceptions import (ContextShutdownError, EntityClosedError, InterruptedError,
                         InvalidArgumentError, InvalidStateError, ResourceExhaustedError)

_LOGGER = logging.getLogger(__name__)


class GoalResponse(Enum):
    REJECT = 1
    ACCEPT = 2


class CancelResponse(Enum):
    REJECT = 1
    ACCEPT = 2


@dataclass
class _EarlyActionResponse:
    response: Any
    kind: str
    candidates: frozenset[int]


@dataclass
class ClientGoalHandle:
    _client: Any
    goal_id: uuid.UUID
    accepted: bool = False
    status: Any = _native.GoalState.UNKNOWN

    def get_result_async(self) -> Any:
        return self._client._get_result_async(self.goal_id)

    def cancel_goal_async(self) -> Any:
        return self._client._cancel_goal_async(self.goal_id)


class ActionClient(Entity):
    _dispatch_kind = "action_client"

    def __init__(self, node: Any, action_type: type, action_name: str, *,
                 max_goal_records: int = 4096) -> None:
        if type(max_goal_records) is not int or max_goal_records <= 0:
            raise InvalidArgumentError("max_goal_records must be positive")
        node._check_open()
        self.action_type, self.action_name = action_type, action_name
        self._native = _native._ActionClient(node._native, node.context._bindings, action_type, action_name)
        super().__init__(node, self._native)
        self._records: dict[uuid.UUID, ClientGoalHandle] = {}
        self._pending: dict[Any, tuple[int, Any, str]] = {}
        self._early: dict[Any, _EarlyActionResponse] = {}
        self._unreconciled: set[int] = set()
        self._tombstones: OrderedDict[Any, float] = OrderedDict()
        self._feedback_callbacks: dict[uuid.UUID, Any] = {}
        self._max_goal_records = max_goal_records
        node._add_entity(self)

    @staticmethod
    def _uuid(value: Any) -> uuid.UUID:
        if value is None:
            return uuid.uuid4()
        if isinstance(value, uuid.UUID):
            return value
        if isinstance(value, (bytes, bytearray)) and len(value) == 16:
            return uuid.UUID(bytes=bytes(value))
        raise InvalidArgumentError("goal_id must be a UUID or 16-byte value")

    def _request(self, service: Any, **fields: Any) -> Any:
        request = service.Request()
        for name, value in fields.items():
            setattr(request, name, value)
        return request

    def send_goal_async(self, goal: Any, *, feedback_callback: Any = None, goal_id: Any = None) -> Any:
        self._check_open()
        if feedback_callback is not None and not callable(feedback_callback):
            raise TypeError("feedback_callback must be callable")
        identifier = self._uuid(goal_id)
        if identifier in self._records:
            raise InvalidArgumentError("goal_id is already tracked by this ActionClient")
        if len(self._records) >= self._max_goal_records:
            raise ResourceExhaustedError("ActionClient goal record capacity exhausted")
        goal_id_message = self.action_type.Impl.SendGoalService.Request().goal_id
        goal_id_message.uuid = list(identifier.bytes)
        request = self._request(self.action_type.Impl.SendGoalService, goal_id=goal_id_message, goal=goal)
        snapshot = self.context._bindings.snapshot(self.action_type.Impl.SendGoalService.Request, request)
        handle = ClientGoalHandle(self, identifier)
        self._records[identifier] = handle
        if feedback_callback is not None:
            self._feedback_callbacks[identifier] = feedback_callback
        try:
            owner = self._owner
            if owner is None:
                raise InvalidStateError("send_goal_async requires an attached executor")
            future = owner._submit_action_request(self, snapshot, _native._ActionRequestKind.GOAL)
            future._action_goal_id = identifier
            future._action_operation = "goal"
            return future
        except BaseException:
            self._records.pop(identifier, None)
            self._feedback_callbacks.pop(identifier, None)
            raise

    def server_is_ready(self) -> bool:
        self._check_open()
        return self._native.server_is_ready()

    def wait_for_server(self, timeout_sec: float | None = None) -> bool:
        self._check_open()
        try:
            return self._native.wait_for_server(timeout_sec)
        except InterruptedError as error:
            if self._cause is not None:
                raise self._cause from error
            if not self.context.ok():
                raise ContextShutdownError("Context shut down during Action availability wait") from error
            raise EntityClosedError("ActionClient closed during availability wait") from error

    def _get_result_async(self, goal_id: uuid.UUID) -> Any:
        self._check_open()
        owner = self._owner
        if owner is None:
            raise InvalidStateError("get_result_async requires an attached executor")
        message = self.action_type.Impl.GetResultService.Request().goal_id
        message.uuid = list(goal_id.bytes)
        request = self._request(self.action_type.Impl.GetResultService, goal_id=message)
        future = owner._submit_action_request(
            self, self.context._bindings.snapshot(self.action_type.Impl.GetResultService.Request, request),
            _native._ActionRequestKind.RESULT)
        future._action_goal_id = goal_id
        future._action_operation = "result"
        return future

    def _cancel_goal_async(self, goal_id: uuid.UUID) -> Any:
        self._check_open()
        owner = self._owner
        if owner is None:
            raise InvalidStateError("cancel_goal_async requires an attached executor")
        info = self.action_type.Impl.CancelGoalService.Request().goal_info
        info.goal_id.uuid = list(goal_id.bytes)
        request = self._request(self.action_type.Impl.CancelGoalService, goal_info=info)
        future = owner._submit_action_request(
            self, self.context._bindings.snapshot(self.action_type.Impl.CancelGoalService.Request, request),
            _native._ActionRequestKind.CANCEL)
        future._action_goal_id = goal_id
        future._action_operation = "cancel"
        return future

    def _retire_candidate(self, operation_id: int) -> None:
        self._unreconciled.discard(operation_id)
        for request_id, early in tuple(self._early.items()):
            if operation_id not in early.candidates:
                continue
            candidates = early.candidates - {operation_id}
            if candidates:
                early.candidates = candidates
            else:
                self._early.pop(request_id)

    def _tombstone(self, request_id: Any) -> None:
        now = time.monotonic()
        while self._tombstones and next(iter(self._tombstones.values())) <= now:
            self._tombstones.popitem(last=False)
        if request_id in self._tombstones:
            return
        if len(self._tombstones) == 4096:
            self._tombstones.popitem(last=False)
        self._tombstones[request_id] = now + 60

    def _send_committed(self, operation_id: int, operation: Any, completion: Any) -> bool:
        try:
            completion.check()
        except BaseException as error:
            operation.future._try_set_exception(error)
            self._retire_goal_future(operation.future)
            self._retire_candidate(operation_id)
            return False
        if completion.skipped or completion.request_id is None:
            self._retire_goal_future(operation.future)
            self._retire_candidate(operation_id)
            return False
        request_id = completion.request_id
        kind = { _native._JobKind.GOAL_REQUEST: "goal", _native._JobKind.CANCEL_REQUEST: "cancel",
                 _native._JobKind.RESULT_REQUEST: "result" }[completion.kind]
        early = self._early.pop(request_id, None)
        if operation.future.done() or self._closing:
            self._tombstone(request_id)
            self._retire_goal_future(operation.future)
            self._retire_candidate(operation_id)
            return False
        if early is not None:
            if early.kind != kind:
                error = InvalidStateError("Action response kind does not match its request")
                operation.future._try_set_exception(error)
                self._retire_goal_future(operation.future)
                self._tombstone(request_id)
                self._retire_candidate(operation_id)
                return False
            self._tombstone(request_id)
            self._complete_response(operation_id, operation, kind, early.response)
            self._retire_candidate(operation_id)
            return False
        self._pending[request_id] = (operation_id, operation, kind)
        self._retire_candidate(operation_id)
        return True

    def _complete_response(self, operation_id: int, operation: Any, kind: str, response: Any) -> None:
        if kind == "goal":
            accepted = bool(response.accepted)
            handle = self._records.get(operation.future._action_goal_id)
            if handle is None:
                self._owner._fail_operation(
                    operation_id, InvalidStateError("Goal response has no client record"))
            else:
                handle.accepted = accepted
                if accepted and handle.status == _native.GoalState.UNKNOWN:
                    handle.status = _native.GoalState.ACCEPTED
                if not accepted:
                    self._feedback_callbacks.pop(handle.goal_id, None)
                    self._records.pop(handle.goal_id, None)
                self._owner._complete_operation(operation_id, handle)
            return
        if kind == "result":
            try:
                state = _native.GoalState(response.status)
            except (AttributeError, TypeError, ValueError):
                state = _native.GoalState.UNKNOWN
            goal_id = getattr(operation.future, "_action_goal_id", None)
            handle = self._records.get(goal_id)
            if handle is not None and self._terminal(state):
                handle.status = state
                self._feedback_callbacks.pop(goal_id, None)
                self._records.pop(goal_id, None)
        self._owner._complete_operation(operation_id, response)

    def _take_response(self, pin: Any) -> None:
        for receiver, kind in ((self._native.receive_goal_response, "goal"),
                               (self._native.receive_cancel_response, "cancel"),
                               (self._native.receive_result_response, "result")):
            received = receiver(pin)
            if received is None:
                continue
            request_id, response = received
            expiry = self._tombstones.get(request_id)
            if expiry is not None:
                if expiry > time.monotonic():
                    continue
                del self._tombstones[request_id]
            pending = self._pending.pop(request_id, None)
            if pending is not None:
                if pending[2] != kind:
                    self._owner._fail_operation(
                        pending[0], InvalidStateError("Action response kind does not match its request"))
                    continue
                self._tombstone(request_id)
                self._complete_response(pending[0], pending[1], kind, response)
                continue
            candidates = frozenset(
                operation_id for operation_id in self._unreconciled
                if self._owner._operations[operation_id].native_work.send_started)
            if not candidates:
                _LOGGER.debug("Dropping unknown Action response with no unreconciled native send")
                continue
            if request_id in self._early:
                continue
            if len(self._early) == 4096:
                cause = ResourceExhaustedError("Action early response staging capacity exhausted")
                self._request_close(cause)
                self._owner._cancel_entity_operations(self, cause)
                self._early.clear()
                continue
            self._early[request_id] = _EarlyActionResponse(response, kind, candidates)

    @staticmethod
    def _goal_id(message: Any) -> uuid.UUID | None:
        try:
            value = bytes(message.uuid)
            return uuid.UUID(bytes=value) if len(value) == 16 else None
        except (AttributeError, TypeError, ValueError):
            return None

    @staticmethod
    def _terminal(status: Any) -> bool:
        return status in (_native.GoalState.SUCCEEDED, _native.GoalState.CANCELED,
                          _native.GoalState.ABORTED)

    def _take_ready(self, pin: Any, executor: Any) -> None:
        # Take one item from each aggregate sub-channel. Coroutine feedback
        # keeps a WorkLease until its owner-loop task has completed.
        self._take_response(pin)
        feedback = self._native.receive_feedback(pin)
        if feedback is not None:
            identifier = self._goal_id(feedback.goal_id)
            callback = None if identifier is None else self._feedback_callbacks.get(identifier)
            if callback is not None:
                result = callback(feedback)
                if inspect.isawaitable(result):
                    executor._schedule_action_feedback(self, result)
        status = self._native.receive_status(pin)
        if status is None:
            return
        for entry in status.status_list:
            identifier = self._goal_id(entry.goal_info.goal_id)
            handle = None if identifier is None else self._records.get(identifier)
            if handle is None:
                continue
            try:
                state = _native.GoalState(entry.status)
            except (TypeError, ValueError):
                continue
            if self._terminal(handle.status) and not self._terminal(state):
                continue
            handle.status = state

    def _cancel_operation(self, operation_id: int, operation: Any) -> None:
        operation.native_work.cancel()
        request_id = getattr(operation, "request_id", None)
        if request_id is not None:
            self._pending.pop(request_id, None)
            self._early.pop(request_id, None)
            self._tombstone(request_id)
        self._retire_goal_future(operation.future)
        self._retire_candidate(operation_id)

    def _retire_goal_future(self, future: Any) -> None:
        if getattr(future, "_action_operation", None) != "goal":
            return
        identifier = getattr(future, "_action_goal_id", None)
        if identifier is not None:
            self._feedback_callbacks.pop(identifier, None)
            self._records.pop(identifier, None)

    def _retire_bindings(self) -> None:
        self._records.clear(); self._pending.clear(); self._early.clear(); self._unreconciled.clear(); self._tombstones.clear()
        self._feedback_callbacks.clear()
        super()._retire_bindings()


@dataclass
class ServerGoalHandle:
    _server: Any
    goal_id: uuid.UUID
    request: Any
    status: Any = _native.GoalState.ACCEPTED

    @property
    def is_active(self) -> bool:
        return self.status in (_native.GoalState.ACCEPTED, _native.GoalState.EXECUTING,
                               _native.GoalState.CANCELING)

    @property
    def is_cancel_requested(self) -> bool:
        return self.status == _native.GoalState.CANCELING

    def succeed(self, result: Any) -> None:
        self._server._terminal(self, _native._GoalEvent.SUCCEED, result)

    def abort(self, result: Any | None = None) -> None:
        self._server._terminal(self, _native._GoalEvent.ABORT, result)

    def canceled(self, result: Any) -> None:
        self._server._terminal(self, _native._GoalEvent.CANCELED, result)

    def publish_feedback(self, feedback: Any) -> None:
        self._server._publish_feedback(self, feedback)


class ActionServer(Entity):
    _dispatch_kind = "action_server"

    def __init__(self, node: Any, action_type: type, action_name: str, execute_callback: Any, *,
                 goal_callback: Any = None, cancel_callback: Any = None,
                 result_timeout_sec: float = 10.0) -> None:
        if not callable(execute_callback):
            raise TypeError("execute_callback must be callable")
        if goal_callback is not None and not callable(goal_callback):
            raise TypeError("goal_callback must be callable")
        if cancel_callback is not None and not callable(cancel_callback):
            raise TypeError("cancel_callback must be callable")
        if type(result_timeout_sec) not in (int, float) or result_timeout_sec < 0:
            raise InvalidArgumentError("result_timeout_sec must be non-negative")
        node._check_open()
        self.action_type, self.action_name = action_type, action_name
        self._native = _native._ActionServer(
            node._native, node.context._bindings, action_type, action_name,
            int(result_timeout_sec * 1_000_000_000))
        super().__init__(node, self._native)
        self._execute_callback = execute_callback
        self._goal_callback = goal_callback or (lambda goal: GoalResponse.ACCEPT)
        self._cancel_callback = cancel_callback or (lambda handle: CancelResponse.REJECT)
        self._lane_busy = False
        self._protocol: dict[int, tuple[str, Any]] = {}
        self._goals: dict[uuid.UUID, ServerGoalHandle] = {}
        self._results: dict[uuid.UUID, Any] = {}
        self._pending_results: dict[Any, Any] = {}
        # Status has one bounded protocol slot.  A revision that changes
        # during delivery is folded into the next immutable status snapshot.
        self._status_revision = 0
        self._status_inflight_ticket: int | None = None
        self._status_inflight_revision = 0
        node._add_entity(self)

    @staticmethod
    def _uuid_from_goal(message: Any) -> uuid.UUID | None:
        try:
            value = bytes(message.uuid)
            return uuid.UUID(bytes=value) if len(value) == 16 else None
        except (AttributeError, TypeError, ValueError):
            return None

    @staticmethod
    def _native_goal_id(identifier: uuid.UUID) -> Any:
        value = _native._GoalId()
        value.data = list(identifier.bytes)
        return value

    def _begin_lane(self, executor: Any) -> None:
        if self._lane_busy:
            raise InvalidStateError("ActionServer ProtocolLane is already busy")
        self._lane_busy = True
        executor._suspend_registration(self)

    def _finish_lane(self, executor: Any) -> None:
        self._lane_busy = False
        executor._restore_registration(self)

    def _new_work(self, executor: Any, kind: Any) -> Any:
        executor._ensure_route()
        completion = _native._Completion()
        record = executor._records.get(self)
        if record is None:
            raise InvalidStateError("ActionServer has no executor attachment")
        completion.attachment_generation = record.generation
        completion.entity_id = self._native.entity_id
        return _native._ActionServerRequestWork(executor._dispatcher, executor._port,
                                                completion, self._native, kind)

    def _take_ready(self, pin: Any, executor: Any) -> None:
        if self._lane_busy:
            return
        self._begin_lane(executor)
        try:
            expired = self._native.take_expired_goals()
            if expired:
                for native_id in expired:
                    identifier = uuid.UUID(bytes=bytes(native_id.data))
                    self._goals.pop(identifier, None)
                    self._results.pop(identifier, None)
                self._publish_status()
            for kind in (_native._ActionServerRequestKind.GOAL,
                         _native._ActionServerRequestKind.CANCEL,
                         _native._ActionServerRequestKind.RESULT):
                work = self._new_work(executor, kind)
                try:
                    request = work.receive(self._native, pin)
                    if request is None:
                        continue
                    if kind == _native._ActionServerRequestKind.GOAL:
                        self._take_goal(work, request)
                    elif kind == _native._ActionServerRequestKind.CANCEL:
                        self._take_cancel(work, request)
                    else:
                        if self._take_result(work, request, executor):
                            work = None
                    return
                finally:
                    if work is not None:
                        work.release()
            self._finish_lane(executor)
        except BaseException:
            self._finish_lane(executor)
            raise

    def _take_goal(self, work: Any, request: Any) -> None:
        identifier = self._uuid_from_goal(request.goal_id)
        accepted = False
        if identifier is not None and identifier not in self._goals:
            try:
                accepted = self._goal_callback(request.goal) is GoalResponse.ACCEPT
            except BaseException:
                accepted = False
        response = self.action_type.Impl.SendGoalService.Response()
        response.accepted = accepted
        if not accepted or identifier is None:
            work.respond(self._native, response)
            self._protocol[work.ticket] = ("reject", None)
            return
        now = time.time_ns()
        response.stamp.sec = now // 1_000_000_000
        response.stamp.nanosec = now % 1_000_000_000
        info = _native._GoalInfo()
        info.goal_id = self._native_goal_id(identifier)
        info.accepted_stamp_ns = now
        handle = ServerGoalHandle(self, identifier, request.goal)
        work.accept(self._native, response, info, False)
        self._protocol[work.ticket] = ("accept", handle)

    @staticmethod
    def _stamp_ns(stamp: Any) -> int:
        try:
            return int(stamp.sec) * 1_000_000_000 + int(stamp.nanosec)
        except AttributeError:
            return 0

    def _cancel_goal_info(self, info: Any, template: Any) -> Any:
        message = template.__class__()
        message.goal_id.uuid = list(info.goal_id.data)
        message.stamp.sec = info.accepted_stamp_ns // 1_000_000_000
        message.stamp.nanosec = info.accepted_stamp_ns % 1_000_000_000
        return message

    def _take_cancel(self, work: Any, request: Any) -> None:
        response = self.action_type.Impl.CancelGoalService.Response()
        identifier = self._uuid_from_goal(request.goal_info.goal_id)
        stamp_ns = self._stamp_ns(request.goal_info.stamp)
        exact = identifier is not None and identifier.int != 0 and stamp_ns == 0
        if exact:
            try:
                state = self._native.goal_state(self._native_goal_id(identifier))
            except BaseException:
                response.return_code = response.ERROR_UNKNOWN_GOAL_ID
                work.respond(self._native, response)
                self._protocol[work.ticket] = ("cancel", None)
                return
            if state not in (_native.GoalState.ACCEPTED, _native.GoalState.EXECUTING):
                response.return_code = response.ERROR_GOAL_TERMINATED
                work.respond(self._native, response)
                self._protocol[work.ticket] = ("cancel", None)
                return
        native_id = self._native_goal_id(identifier) if identifier is not None else _native._GoalId()
        candidates = self._native.select_cancel_goals(native_id, stamp_ns)
        accepted: list[Any] = []
        try:
            for info in candidates:
                candidate_id = uuid.UUID(bytes=bytes(info.goal_id.data))
                handle = self._goals.get(candidate_id)
                if handle is not None and self._cancel_callback(handle) is CancelResponse.ACCEPT:
                    accepted.append((info, handle))
        except BaseException:
            accepted.clear()
        committed = []
        for info, handle in accepted:
            try:
                transition = self._native.update_goal_state(info.goal_id, _native._GoalEvent.CANCEL)
            except BaseException:
                continue
            handle.status = transition.current
            response.goals_canceling.append(self._cancel_goal_info(info, request.goal_info))
            committed.append(handle)
        response.return_code = response.ERROR_NONE if committed else response.ERROR_REJECTED
        if committed:
            self._publish_status()
        work.respond(self._native, response)
        self._protocol[work.ticket] = ("cancel", None)

    def _unknown_result_payload(self) -> Any:
        response = self.action_type.Impl.GetResultService.Response()
        response.status = int(_native.GoalState.UNKNOWN)
        response.result = self.action_type.Result()
        return _native._ActionResultPayload(self._native, response)

    def _take_result(self, work: Any, request: Any, executor: Any) -> bool:
        identifier = self._uuid_from_goal(request.goal_id)
        if identifier is None:
            work.respond_payload(self._unknown_result_payload())
            self._protocol[work.ticket] = ("result", None)
            return False
        disposition = self._native.register_result_request(
            self._native_goal_id(identifier), work.request_id)
        if disposition == _native._ResultRequestDisposition.PENDING:
            self._pending_results[work.request_id] = work
            self._finish_lane(executor)
            return True
        payload = self._results.get(identifier)
        if disposition == _native._ResultRequestDisposition.UNKNOWN_GOAL or payload is None:
            payload = self._unknown_result_payload()
        work.respond_payload(payload)
        self._protocol[work.ticket] = ("result", None)
        return False

    def _protocol_completion(self, ticket: int, completion: Any, executor: Any) -> None:
        operation = self._protocol.pop(ticket, None)
        if operation is None:
            raise InvalidStateError("ActionServer completion has no protocol owner")
        try:
            completion.check()
            if operation[0] == "accept":
                handle = operation[1]
                self._goals[handle.goal_id] = handle
                self._publish_status()
                self._run_execute(handle)
        finally:
            # A retained GetResult request releases the protocol lane before
            # its response is available.  Its later delivery must still be
            # tracked for completion errors, but cannot unlock a newer
            # request that is using the lane.
            if operation[0] != "result_delivery":
                self._finish_lane(executor)

    def _run_execute(self, handle: ServerGoalHandle) -> None:
        self._assert_owner_thread()
        if not handle.is_active:
            return
        self._native.update_goal_state(self._native_goal_id(handle.goal_id), _native._GoalEvent.EXECUTE)
        handle.status = _native.GoalState.EXECUTING
        try:
            result = self._execute_callback(handle)
            if inspect.isawaitable(result):
                scheduler = getattr(self._owner, "_schedule_action_execute", None)
                if scheduler is None:
                    if inspect.iscoroutine(result):
                        result.close()
                    raise InvalidStateError("Coroutine action execution requires AsyncIOExecutor")
                scheduler(self, handle, result)
                return
        except BaseException:
            result = self.action_type.Result()
        if handle.is_active:
            self.abort_goal(handle, result)

    def _finish_execute(self, handle: ServerGoalHandle, result: Any | None,
                        error: BaseException | None) -> None:
        self._assert_owner_thread()
        if not handle.is_active:
            return
        self.abort_goal(handle, self.action_type.Result() if error is not None else result)

    def _assert_owner_thread(self) -> None:
        if self._owner is None or not self._owner._in_owner_thread():
            raise InvalidStateError("Action goal mutation must run on its owner Executor")

    def abort_goal(self, handle: ServerGoalHandle, result: Any | None = None) -> None:
        self._terminal(handle, _native._GoalEvent.ABORT, result)

    def _terminal(self, handle: ServerGoalHandle, event: Any, result: Any | None) -> None:
        self._assert_owner_thread()
        if self._goals.get(handle.goal_id) is not handle or not handle.is_active:
            raise InvalidStateError("Goal is not active on this ActionServer")
        if result is None:
            result = self.action_type.Result()
        response = self.action_type.Impl.GetResultService.Response()
        response.status = { _native._GoalEvent.SUCCEED: 4, _native._GoalEvent.ABORT: 6,
                            _native._GoalEvent.CANCELED: 5 }[event]
        response.result = result
        payload = _native._ActionResultPayload(self._native, response)
        transition = self._native.update_goal_state(self._native_goal_id(handle.goal_id), event)
        handle.status = transition.current
        self._results[handle.goal_id] = payload
        self._publish_status()
        for request_id in self._native.take_pending_result_requests(self._native_goal_id(handle.goal_id)):
            work = self._pending_results.pop(request_id, None)
            if work is not None:
                self._protocol[work.ticket] = ("result_delivery", None)
                work.respond_payload(payload)
                work.release()

    def _publish(self, message: Any, *, feedback: bool) -> int:
        self._assert_owner_thread()
        owner = self._owner
        owner._ensure_route()
        record = owner._records.get(self)
        if record is None:
            raise InvalidStateError("ActionServer has no executor attachment")
        completion = _native._Completion()
        completion.attachment_generation = record.generation
        completion.entity_id = self._native.entity_id
        return owner._dispatcher.publish_action(owner._port, completion, self._native, message, feedback)

    def _publish_feedback(self, handle: ServerGoalHandle, feedback: Any) -> None:
        self._assert_owner_thread()
        if self._goals.get(handle.goal_id) is not handle or not handle.is_active:
            raise InvalidStateError("Goal is not active on this ActionServer")
        if not isinstance(feedback, self.action_type.Feedback):
            raise TypeError("feedback has the wrong generated Action type")
        message = self.action_type.Impl.FeedbackMessage()
        message.goal_id.uuid = list(handle.goal_id.bytes)
        message.feedback = feedback
        self._publish(message, feedback=True)

    def _publish_status(self) -> None:
        self._assert_owner_thread()
        self._status_revision += 1
        if self._status_inflight_ticket is not None:
            return
        self._start_status_publication()

    def _start_status_publication(self) -> None:
        """Submit the single coalesced Status work item from the owner thread."""
        self._assert_owner_thread()
        if self._closing or self._status_inflight_ticket is not None:
            return
        import importlib
        goal_status_type = importlib.import_module("action_msgs_dclpy.msg").GoalStatus
        message = self.action_type.Impl.GoalStatusMessage()
        for item in self._native.status_snapshot():
            status = goal_status_type()
            status.goal_info.goal_id.uuid = list(item.goal_info.goal_id.data)
            status.goal_info.stamp.sec = item.goal_info.accepted_stamp_ns // 1_000_000_000
            status.goal_info.stamp.nanosec = item.goal_info.accepted_stamp_ns % 1_000_000_000
            status.status = int(item.state)
            message.status_list.append(status)
        self._status_inflight_revision = self._status_revision
        self._status_inflight_ticket = self._publish(message, feedback=False)

    def _status_completion(self, ticket: int, completion: Any) -> None:
        self._assert_owner_thread()
        if ticket != self._status_inflight_ticket:
            raise InvalidStateError("ActionServer Status completion has no in-flight snapshot")
        delivered_revision = self._status_inflight_revision
        self._status_inflight_ticket = None
        try:
            completion.check()
        except BaseException as error:
            # Native I/O has already performed its bounded retry sequence.
            # Keep this revision dirty; the next state change will submit a
            # fresh snapshot without unbounded retry traffic.
            _LOGGER.error("Action status delivery failed", exc_info=(type(error), error,
                                                                        error.__traceback__))
            return
        if self._status_revision > delivered_revision:
            self._start_status_publication()

    def _retire_bindings(self) -> None:
        # ResultPayload owns ABI samples and must be released before Context
        # clears its provider registry, even if Python retains this server.
        self._pending_results.clear()
        self._results.clear()
        self._goals.clear()
        self._protocol.clear()
        self._status_inflight_ticket = None
        super()._retire_bindings()
