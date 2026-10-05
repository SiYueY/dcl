"""Executor ownership, bounded dispatch, and control notification barriers."""
from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from enum import Enum, auto
import inspect
import logging
import threading
import time
from typing import Any

from . import _dclpy as _native
from .context import Context, _timeout
from .exceptions import (AlreadyRegisteredError, BusyError, ContextShutdownError,
                         ExecutorStoppedError, InvalidStateError)
from .future import Future

_LOGGER = logging.getLogger(__name__)


class ExecutorState(Enum):
    CREATED = auto()
    RUNNING = auto()
    STOPPING = auto()
    STOPPED = auto()


@dataclass
class _EntityRecord:
    entity: Any
    generation: int
    registration: int | None = None


@dataclass
class _OutboundOperation:
    entity: Any
    future: Future
    generation: int
    ticket: int | None = None
    kind: str = "publish"
    native_work: Any = None
    request_id: Any = None
    send_reconciled: bool = False


class SingleThreadedExecutor:
    def __init__(self, context: Context) -> None:
        context._check_open()
        self.context = context
        self._lock = threading.RLock()
        self._spin_lock = threading.Lock()
        self._state = ExecutorState.CREATED
        self._wait_set = _native._WaitSet(context._native)
        self._nodes: set[Any] = set()
        self._records: dict[Any, _EntityRecord] = {}
        self._registrations: dict[int, _EntityRecord] = {}
        self._notifications = deque()
        self._notification_producers = 0
        self._futures: set[Future] = set()
        self._shutdown_barrier = Future()
        self._cause: BaseException | None = None
        self._spin_thread: int | None = None
        self._cancel_tasks = False
        self._dispatcher = None
        self._port = None
        self._operations: dict[int, _OutboundOperation] = {}
        self._next_operation = 1
        try:
            context._register_owner(self)
        except BaseException:
            self._wait_set.close()
            raise

    @property
    def state(self) -> ExecutorState:
        with self._lock:
            return self._state

    def _check_admission(self) -> None:
        self.context._check_open()
        if self._state in (ExecutorState.STOPPING, ExecutorState.STOPPED):
            raise ExecutorStoppedError("Executor no longer accepts work")

    def add_node(self, node: Any) -> bool:
        if node.context is not self.context:
            raise InvalidStateError("Node belongs to a different Context")
        with node._lock, self._lock:
            self._check_admission()
            node._check_open()
            if node._owner is self:
                return False
            if node._owner is not None:
                raise AlreadyRegisteredError("Node already has an executor owner")
            generation = node._attachment_generation + 1
            attached = []
            try:
                for entity in tuple(node._entities):
                    if entity._closing:
                        continue
                    self._attach_entity(entity, generation)
                    attached.append(entity)
            except BaseException:
                for entity in reversed(attached):
                    self._detach_record(self._records[entity])
                raise
            node._attachment_generation = generation
            node._owner = self
            self._nodes.add(node)
        self._wait_set.wake()
        return True

    def _attach_entity(self, entity: Any, generation: int) -> None:
        record = _EntityRecord(entity, generation)
        self._records[entity] = record
        try:
            if isinstance(entity._native, _native._Waitable):
                token = self._wait_set.add(entity._native, generation)
                record.registration = token
                self._registrations[token] = record
            entity._owner = self
        except BaseException:
            if record.registration is not None:
                self._wait_set.remove(record.registration)
                self._registrations.pop(record.registration, None)
            self._records.pop(entity, None)
            raise

    def _add_entity(self, entity: Any) -> None:
        with entity.node._lock, self._lock:
            self._check_admission()
            if entity.node._owner is not self:
                raise InvalidStateError("Node attachment changed during entity creation")
            self._attach_entity(entity, entity.node._attachment_generation)
        self._wait_set.wake()

    def _detach_record(self, record: _EntityRecord) -> None:
        if record.registration is not None:
            self._wait_set.remove(record.registration)
            self._registrations.pop(record.registration, None)
            record.registration = None
        record.entity._owner = None
        self._records.pop(record.entity, None)

    def _suspend_registration(self, entity: Any) -> None:
        """Stop native dispatch while an owner-side protocol transaction commits."""
        with self._lock:
            record = self._records.get(entity)
            if record is None or record.registration is None:
                return
            self._wait_set.remove(record.registration)
            self._registrations.pop(record.registration, None)
            record.registration = None

    def _restore_registration(self, entity: Any) -> None:
        with self._lock:
            if self._state is not ExecutorState.RUNNING or not self.context.ok() or entity._closing:
                return
            record = self._records.get(entity)
            if record is None or record.registration is not None:
                return
            token = self._wait_set.add(entity._native, record.generation)
            record.registration = token
            self._registrations[token] = record
        self._wait_set.wake()

    def remove_node(self, node: Any) -> bool:
        with node._lock, self._lock:
            if node._owner is not self:
                return False
            records = [record for record in self._records.values() if record.entity.node is node]
            if self._operations or self._notification_producers or self._notifications or any(not f.done() for f in self._futures):
                raise BusyError("Owner still has accepted work or Future notifications")
            if any(not record.entity._native.quiescent() for record in records):
                raise BusyError("Node still has active dispatch or work leases")
            for record in records:
                self._detach_record(record)
            self._nodes.remove(node)
            node._owner = None
        self._wait_set.wake()
        return True

    def _ensure_route(self) -> None:
        if self._dispatcher is None:
            dispatcher = self.context._get_dispatcher()
            port = dispatcher.open_route(self._wait_set)
            self._dispatcher, self._port = dispatcher, port

    def _submit_publish(self, entity: Any, snapshot: Any) -> Future:
        with self._lock:
            self._check_admission()
            record = self._records.get(entity)
            if record is None or entity._owner is not self:
                raise InvalidStateError("Publisher needs an attached executor owner")
            self._ensure_route()
            future = Future(executor=self)
            operation_id = self._next_operation
            self._next_operation += 1
            operation = _OutboundOperation(entity, future, record.generation)
            self._operations[operation_id] = operation
            completion = _native._Completion()
            completion.attachment_generation = record.generation
            completion.operation_id = operation_id
            completion.entity_id = entity._native.entity_id
            try:
                operation.ticket = self._dispatcher.publish(self._port, completion, entity._native, snapshot)
            except BaseException as error:
                self._operations.pop(operation_id, None)
                future._try_set_exception(error)
            return future

    def _submit_request(self, entity: Any, snapshot: Any) -> Future:
        with self._lock:
            self._check_admission()
            record = self._records.get(entity)
            if record is None or entity._owner is not self:
                raise InvalidStateError("Client needs an attached executor owner")
            self._ensure_route()
            future = Future(executor=self)
            operation_id = self._next_operation
            self._next_operation += 1
            operation = _OutboundOperation(entity, future, record.generation, kind="request")
            self._operations[operation_id] = operation
            entity._unreconciled.add(operation_id)
            completion = _native._Completion()
            completion.attachment_generation = record.generation
            completion.operation_id = operation_id
            completion.entity_id = entity._native.entity_id
            try:
                operation.native_work = _native._ClientRequestWork(entity._native, snapshot)
                future._set_cancel_hook(operation.native_work.cancel)
                future.add_done_callback(lambda done: self._operation_terminal(operation_id))
                operation.ticket = self._dispatcher.send_request(self._port, completion, operation.native_work)
            except BaseException as error:
                entity._retire_candidate(operation_id)
                self._operations.pop(operation_id, None)
                future._try_set_exception(error)
            return future

    def _submit_action_request(self, entity: Any, snapshot: Any, request_kind: Any) -> Future:
        with self._lock:
            self._check_admission()
            record = self._records.get(entity)
            if record is None or entity._owner is not self:
                raise InvalidStateError("ActionClient needs an attached executor owner")
            self._ensure_route()
            future = Future(executor=self)
            operation_id = self._next_operation
            self._next_operation += 1
            operation = _OutboundOperation(entity, future, record.generation, kind="action_request")
            self._operations[operation_id] = operation
            entity._unreconciled.add(operation_id)
            completion = _native._Completion()
            completion.attachment_generation = record.generation
            completion.operation_id = operation_id
            completion.entity_id = entity._native.entity_id
            try:
                operation.native_work = _native._ActionRequestWork(entity._native, snapshot, request_kind)
                future._set_cancel_hook(operation.native_work.cancel)
                future.add_done_callback(lambda done: self._operation_terminal(operation_id))
                operation.ticket = self._dispatcher.send_action_request(
                    self._port, completion, operation.native_work, request_kind)
            except BaseException as error:
                entity._retire_candidate(operation_id)
                self._operations.pop(operation_id, None)
                future._try_set_exception(error)
            return future

    def _operation_terminal(self, operation_id: int) -> None:
        with self._lock:
            operation = self._operations.get(operation_id)
            if operation is None or operation.kind not in ("request", "action_request"):
                return
            operation.entity._cancel_operation(operation_id, operation)
            if operation.send_reconciled:
                self._operations.pop(operation_id)

    def _complete_request(self, operation_id: int, response: Any) -> None:
        with self._lock:
            operation = self._operations.pop(operation_id)
        operation.future._try_set_result(response)

    def _complete_operation(self, operation_id: int, value: Any) -> None:
        with self._lock:
            operation = self._operations.pop(operation_id, None)
        if operation is not None:
            operation.future._try_set_result(value)

    def _fail_operation(self, operation_id: int, error: BaseException) -> None:
        with self._lock:
            operation = self._operations.pop(operation_id, None)
        if operation is not None:
            operation.future._try_set_exception(error)

    def _cancel_entity_operations(self, entity: Any, cause: BaseException) -> None:
        with self._lock:
            operations = tuple((key, value) for key, value in self._operations.items() if value.entity is entity)
        for operation_id, operation in operations:
            if operation.kind in ("request", "action_request"):
                entity._cancel_operation(operation_id, operation)
                if operation.send_reconciled:
                    self._operations.pop(operation_id, None)
            operation.future._try_set_exception(cause)

    def _drain_completions(self) -> None:
        if self._dispatcher is None:
            return
        while True:
            entry = self._dispatcher.peek(self._port)
            if entry is None:
                return
            ticket, completion = entry
            with self._lock:
                operation = self._operations.get(completion.operation_id)
                record = next((candidate for candidate in self._records.values()
                               if candidate.entity._native.entity_id == completion.entity_id), None)
            keep = False
            try:
                if completion.kind in (_native._JobKind.SERVICE_RESPONSE,
                                       _native._JobKind.DISCARD_REQUEST):
                    if (record is None or record.generation != completion.attachment_generation or
                            record.entity._dispatch_kind != "service"):
                        raise InvalidStateError("Service completion violates its captured owner identity")
                    try:
                        completion.check()
                    except BaseException as error:
                        # A response that exhausted delivery/discard cleanup is
                        # a service protocol fault. Keep the executor control
                        # path alive and close only this endpoint.
                        _LOGGER.error("Service delivery failed", exc_info=(type(error), error,
                                                                             error.__traceback__))
                        record.entity._request_close(error)
                    if completion.cleanup_failed:
                        record.entity._request_close(
                            InvalidStateError("Service discard cleanup failed permanently"))
                    continue
                if completion.kind in (_native._JobKind.GOAL_RESPONSE,
                                       _native._JobKind.CANCEL_RESPONSE,
                                       _native._JobKind.RESULT_RESPONSE):
                    if (record is None or record.generation != completion.attachment_generation or
                            record.entity._dispatch_kind != "action_server"):
                        raise InvalidStateError("ActionServer completion violates its captured owner identity")
                    record.entity._protocol_completion(ticket, completion, self)
                    continue
                if completion.kind in (_native._JobKind.FEEDBACK, _native._JobKind.STATUS):
                    if (record is None or record.generation != completion.attachment_generation or
                            record.entity._dispatch_kind != "action_server"):
                        raise InvalidStateError("Action publication completion violates its owner identity")
                    if completion.kind == _native._JobKind.STATUS:
                        record.entity._status_completion(ticket, completion)
                    else:
                        completion.check()
                    continue
                if (operation is None or operation.ticket != ticket or
                        completion.executor_id != self._port.executor_id or
                        completion.entity_id != operation.entity._native.entity_id or
                        completion.attachment_generation != operation.generation):
                    raise InvalidStateError("Native completion violates its captured owner identity")
                keep = False
                if operation.kind in ("request", "action_request"):
                    keep = operation.entity._send_committed(completion.operation_id, operation, completion)
                    operation.send_reconciled = True
                    operation.ticket = None
                else:
                    try:
                        completion.check()
                    except BaseException as error:
                        operation.future._try_set_exception(error)
                    else:
                        operation.future._try_set_result(None)
            finally:
                # The job retains its sample and WorkLease through the ACK,
                # including canceled Futures and STOPPING owner cleanup.
                self._dispatcher.acknowledge(self._port, ticket)
                with self._lock:
                    if operation is None or not keep:
                        self._operations.pop(completion.operation_id, None)


    def _entity_closing(self, entity: Any) -> None:
        # Native close denies new dispatch immediately. Physical unregister
        # belongs to the owner control path, including shutdown control.
        self._wake_control()

    def _register_future(self, future: Future) -> None:
        with self._lock:
            self._check_admission()
            self._futures.add(future)

    def _begin_future_notification(self) -> None:
        with self._lock:
            if self._state is ExecutorState.STOPPED:
                raise ExecutorStoppedError("Future notification owner has stopped")
            self._notification_producers += 1

    def _end_future_notification(self) -> None:
        with self._lock:
            self._notification_producers -= 1
        self._wake_control()

    def _schedule_done_callback(self, callback: Any, future: Future) -> None:
        with self._lock:
            if self._state is ExecutorState.STOPPED:
                raise ExecutorStoppedError("Future callback owner has stopped")
            self._notifications.append((callback, future))
        self._wake_control()

    def _schedule_action_feedback(self, entity: Any, awaitable: Any) -> None:
        if inspect.iscoroutine(awaitable):
            awaitable.close()
        raise InvalidStateError("Coroutine feedback callbacks require AsyncIOExecutor")

    def _wake_control(self) -> None:
        with self._lock:
            if self._state is not ExecutorState.STOPPED:
                self._wait_set.wake()

    def _control(self) -> None:
        with self._lock:
            closing = [record for record in self._records.values() if record.entity._closing]
            stopping = self._state is ExecutorState.STOPPING
            records = tuple(self._records.values()) if stopping else tuple(closing)
            cause = self._cause
        for record in records:
            self._cancel_entity_operations(record.entity, cause if stopping else record.entity._cause)
        self._drain_completions()
        for record in closing:
            with self._lock:
                if record.registration is not None:
                    self._wait_set.remove(record.registration)
                    self._registrations.pop(record.registration, None)
                    record.registration = None
            if record.entity._native.state == _native._EntityState.CLOSED:
                record.entity._retire_bindings()
                with self._lock:
                    if self._records.get(record.entity) is record:
                        self._detach_record(record)
        while True:
            with self._lock:
                if not self._notifications:
                    break
                callback, future = self._notifications.popleft()
            future._invoke(callback)

    def _in_owner_thread(self) -> bool:
        return self._spin_thread == threading.get_ident()

    def _is_managed_current_task(self) -> bool:
        return False

    def spin_once(self, timeout_sec: float | None = None) -> None:
        timeout_sec = _timeout(timeout_sec)
        if not self._spin_lock.acquire(blocking=False):
            raise BusyError("Executor already has a spin/control owner")
        try:
            with self._lock:
                if self._state is ExecutorState.STOPPED:
                    raise ExecutorStoppedError("Executor has stopped")
                if self._state is ExecutorState.CREATED:
                    self._state = ExecutorState.RUNNING
                self._spin_thread = threading.get_ident()
            self._control()
            if self.state is ExecutorState.STOPPING:
                self._drain_shutdown()
                return
            batch = self._wait_set.wait(timeout_sec)
            try:
                for ready in batch.entries:
                    with self._lock:
                        record = self._registrations.get(ready.registration)
                        admitted = self._state is ExecutorState.RUNNING
                    if record is None or record.generation != ready.attachment_generation:
                        raise InvalidStateError("Ready batch has a stale owner generation")
                    if not admitted or not self.context.ok() or record.entity._closing:
                        continue
                    if record.entity._dispatch_kind == "client":
                        record.entity._take_response(ready.pin)
                        continue
                    if record.entity._dispatch_kind == "action_client":
                        record.entity._take_ready(ready.pin, self)
                        continue
                    if record.entity._dispatch_kind == "action_server":
                        record.entity._take_ready(ready.pin, self)
                        continue
                    if record.entity._dispatch_kind == "service":
                        self._ensure_route()
                        completion = _native._Completion()
                        completion.attachment_generation = record.generation
                        completion.entity_id = record.entity._native.entity_id
                        work = _native._ServiceRequestWork(self._dispatcher, self._port,
                                                           completion, record.entity._native)
                        try:
                            request = work.receive(record.entity._native, ready.pin)
                            if request is not None:
                                response = record.entity.service_type.Response()
                                record.entity._invoke_sync(request, response, work)
                        finally:
                            work.release()
                        continue
                    message = record.entity._take(ready.pin)
                    if message is None or not self.context.ok() or record.entity._closing:
                        continue
                    result = record.entity.callback(message)
                    if inspect.isawaitable(result):
                        if inspect.iscoroutine(result):
                            result.close()
                        raise InvalidStateError("Coroutine callbacks require AsyncIOExecutor")
            finally:
                batch.release()
            self._control()
            if self.state is ExecutorState.STOPPING:
                self._drain_shutdown()
        finally:
            self._spin_thread = None
            self._spin_lock.release()

    def spin(self) -> None:
        while self.state is not ExecutorState.STOPPED:
            self.spin_once()

    def _request_shutdown(self, *, cause: BaseException | None, cancel_tasks: bool) -> None:
        with self._lock:
            if self._state is ExecutorState.STOPPED:
                return
            self._cancel_tasks |= bool(cancel_tasks)
            if isinstance(cause, ContextShutdownError) or self._cause is None:
                self._cause = cause or ExecutorStoppedError("Executor shutdown was requested")
            self._state = ExecutorState.STOPPING
            if self._dispatcher is not None:
                self._dispatcher.stop_route(self._port)
        self._wake_control()

    def request_shutdown(self, *, cancel_tasks: bool = True) -> None:
        self._request_shutdown(cause=None, cancel_tasks=cancel_tasks)

    def _drain_shutdown(self) -> None:
        with self._lock:
            futures = tuple(self._futures)
            cause = self._cause
        for entity in tuple(self._records):
            self._cancel_entity_operations(entity, cause)
        for future in futures:
            future._try_set_exception(cause)
        self._control()
        with self._lock:
            if self._notification_producers or self._notifications or self._operations:
                return
            records = tuple(self._records.values())
            for record in records:
                if not record.entity._native.quiescent():
                    return
            for record in records:
                self._detach_record(record)
            if self._dispatcher is not None:
                self._dispatcher.retire_route(self._port)
                self._dispatcher = self._port = None
            self._wait_set.close()
            self._state = ExecutorState.STOPPED
            self._futures.clear()
        self._shutdown_barrier._try_set_result(True)

    async def _shutdown_join(self) -> bool:
        import asyncio
        return await asyncio.to_thread(self.shutdown, cancel_tasks=self._cancel_tasks)

    def shutdown(self, *, cancel_tasks: bool = True, timeout: float | None = None) -> bool:
        if self._in_owner_thread():
            raise InvalidStateError("Blocking shutdown cannot wait on its spin owner")
        timeout = _timeout(timeout)
        deadline = None if timeout is None else time.monotonic() + timeout
        self.request_shutdown(cancel_tasks=cancel_tasks)
        # A never-spun executor has no application owner: this calling thread
        # supplies the control owner. An existing spin loop keeps ownership.
        if self._spin_lock.acquire(blocking=False):
            try:
                self._spin_thread = threading.get_ident()
                while self.state is not ExecutorState.STOPPED:
                    self._drain_shutdown()
                    if deadline is not None and time.monotonic() >= deadline:
                        break
                    if self.state is not ExecutorState.STOPPED:
                        self._wait_set.wait(0.01).release()
            finally:
                self._spin_thread = None
                self._spin_lock.release()
        remaining = None if deadline is None else max(0.0, deadline - time.monotonic())
        try:
            return self._shutdown_barrier._blocking_result(remaining)
        except TimeoutError as error:
            from .exceptions import DclpyTimeoutError
            raise DclpyTimeoutError("Executor shutdown has not completed") from error
