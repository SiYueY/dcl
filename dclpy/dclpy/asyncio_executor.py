"""Loop-owned callback tasks with a single unacknowledged native ready batch."""
from __future__ import annotations

import asyncio
import inspect
import logging
import threading
import time
from typing import Any

from . import _dclpy as _native
from .context import Context, _timeout
from .exceptions import (BusyError, DclpyTimeoutError, ExecutorStoppedError,
                         InvalidArgumentError, InvalidStateError)
from .executors import ExecutorState, SingleThreadedExecutor

_LOGGER = logging.getLogger(__name__)
# A closed owner loop cannot execute Python cleanup. Retaining the owner keeps
# its tasks, protocol records, providers and native leases intact in this fault.
_FAULTED_OWNERS: set[Any] = set()


class AsyncIOExecutor(SingleThreadedExecutor):
    def __init__(self, context: Context, *, loop: asyncio.AbstractEventLoop | None = None,
                 max_callback_tasks: int = 64, max_service_tasks_per_service: int = 8,
                 max_execute_tasks_per_server: int = 8) -> None:
        for value in (max_callback_tasks, max_service_tasks_per_service, max_execute_tasks_per_server):
            if type(value) is not int or value <= 0:
                raise InvalidArgumentError("Task capacity must be a positive integer")
        self.loop = loop or asyncio.get_running_loop()
        if self.loop.is_closed():
            raise InvalidStateError("Executor cannot bind a closed event loop")
        self._limits = {"general": max_callback_tasks, "service": max_service_tasks_per_service,
                        "execute": max_execute_tasks_per_server}
        self._tasks: dict[asyncio.Task, tuple[Any, Any, tuple[str, Any]]] = {}
        self._task_counts: dict[tuple[str, Any], int] = {}
        self._cancel_requested: set[asyncio.Task] = set()
        self._pending_batch = None
        self._batch_ack = threading.Event()
        self._batch_ack.set()
        self._wait_thread: threading.Thread | None = None
        self._wait_stop = False
        self._drain_task: asyncio.Task | None = None
        self._fault: BaseException | None = None
        self._loop_thread: int | None = None
        super().__init__(context)

    def _on_loop(self) -> bool:
        try:
            return asyncio.get_running_loop() is self.loop
        except RuntimeError:
            return False

    def _in_owner_thread(self) -> bool:
        return self._on_loop() or self._loop_thread == threading.get_ident()

    def _is_managed_current_task(self) -> bool:
        return self._on_loop() and asyncio.current_task() in self._tasks

    def start(self) -> None:
        if not self._on_loop():
            raise InvalidStateError("start must run on the bound event loop")
        with self._lock:
            self._check_admission()
            if self._state is not ExecutorState.CREATED:
                raise InvalidStateError("AsyncIOExecutor can start only once")
            self._state = ExecutorState.RUNNING
            self._loop_thread = threading.get_ident()
            thread = threading.Thread(target=self._wait_main, name="dclpy-asyncio-wait", daemon=True)
            self._wait_thread = thread
        thread.start()
        self._control()

    def _record_fault(self, error: BaseException) -> None:
        with self._lock:
            if self._fault is not None:
                return
            self._fault = error
            self._state = ExecutorState.STOPPING
            _FAULTED_OWNERS.add(self)
        _LOGGER.error("Executor owner cleanup cannot complete: %s", error)
        self._shutdown_barrier._try_set_exception(error)

    def _schedule_loop(self, callback: Any) -> None:
        try:
            self.loop.call_soon_threadsafe(callback)
        except RuntimeError as error:
            self._record_fault(InvalidStateError(f"Executor event loop closed before its shutdown barrier: {error}"))

    def _wake_control(self) -> None:
        super()._wake_control()
        self._schedule_loop(self._run_control)

    def _run_control(self) -> None:
        if self.state is ExecutorState.STOPPED or self._fault is not None:
            return
        self._loop_thread = threading.get_ident()
        try:
            self._control()
        except BaseException as error:
            self._record_fault(error)

    def _wait_main(self) -> None:
        try:
            while True:
                with self._lock:
                    if self._wait_stop:
                        return
                batch = self._wait_set.wait(0.1)
                with self._lock:
                    if self._wait_stop:
                        batch.release()
                        return
                    if self._pending_batch is not None:
                        raise InvalidStateError("Wait thread produced a second unacknowledged batch")
                    self._pending_batch = batch
                    self._batch_ack.clear()
                self._schedule_loop(self._consume_batch)
                # ACK covers native take and Task/job registration. It never
                # waits for application Tasks, including nested service calls.
                self._batch_ack.wait()
        except BaseException as error:
            self._record_fault(error)

    def _task_key(self, entity: Any, kind: str) -> tuple[str, Any]:
        return kind, None if kind == "general" else entity

    def _has_task_capacity(self, entity: Any, kind: str = "general") -> bool:
        return self._task_counts.get(self._task_key(entity, kind), 0) < self._limits[kind]

    def _pause_registration(self, record: Any) -> None:
        self._suspend_registration(record.entity)

    def _restore_registrations(self) -> None:
        if self.state is not ExecutorState.RUNNING or not self.context.ok():
            return
        with self._lock:
            for record in tuple(self._records.values()):
                entity = record.entity
                if (record.registration is None and not entity._closing and
                        isinstance(entity._native, _native._Waitable) and
                        self._has_task_capacity(entity)):
                    token = self._wait_set.add(entity._native, record.generation)
                    record.registration = token
                    self._registrations[token] = record

    def _restore_registration(self, entity: Any) -> None:
        super()._restore_registration(entity)

    def _create_callback_task(self, entity: Any, message: Any, lease: Any,
                              kind: str = "general") -> None:
        key = self._task_key(entity, kind)
        async def invoke() -> None:
            if self.state is not ExecutorState.RUNNING or not self.context.ok() or entity._closing:
                return
            if kind == "service":
                await entity._invoke_async(message, entity.service_type.Response(), lease)
                return
            result = entity.callback(message)
            if inspect.isawaitable(result):
                await result
        coroutine = invoke()
        try:
            task = self.loop.create_task(coroutine)
            self._tasks[task] = (entity, lease, key)
            task.add_done_callback(self._task_done)
        except BaseException:
            coroutine.close()
            raise

    def _schedule_action_feedback(self, entity: Any, awaitable: Any) -> None:
        if not self._has_task_capacity(entity):
            if inspect.iscoroutine(awaitable):
                awaitable.close()
            _LOGGER.warning("Dropping Action feedback because callback capacity is exhausted")
            return
        lease = entity._native.work_lease()
        key = self._task_key(entity, "general")

        async def invoke() -> None:
            await awaitable

        coroutine = invoke()
        self._task_counts[key] = self._task_counts.get(key, 0) + 1
        try:
            task = self.loop.create_task(coroutine)
            self._tasks[task] = (entity, lease, key)
            task.add_done_callback(self._task_done)
        except BaseException:
            coroutine.close()
            lease.release()
            self._task_counts[key] -= 1
            raise

    def _schedule_action_execute(self, server: Any, handle: Any, awaitable: Any) -> None:
        if not self._has_task_capacity(server, "execute"):
            if inspect.iscoroutine(awaitable):
                awaitable.close()
            server._finish_execute(handle, None,
                                   InvalidStateError("Action execute task capacity is exhausted"))
            return
        lease = server._native.work_lease()
        key = self._task_key(server, "execute")

        async def invoke() -> Any:
            return await awaitable

        coroutine = invoke()
        self._task_counts[key] = self._task_counts.get(key, 0) + 1
        try:
            task = self.loop.create_task(coroutine)
        except BaseException:
            coroutine.close()
            lease.release()
            self._task_counts[key] -= 1
            raise

        def complete(done: asyncio.Task) -> None:
            error: BaseException | None = None
            result = None
            try:
                if done.cancelled():
                    error = asyncio.CancelledError()
                else:
                    error = done.exception()
                    if error is None:
                        result = done.result()
                server._finish_execute(handle, result, error)
            except BaseException as cleanup_error:
                self._record_fault(cleanup_error)
            finally:
                lease.release()
                self._task_counts[key] -= 1
                try:
                    self._control()
                    self._restore_registrations()
                except BaseException as control_error:
                    self._record_fault(control_error)

        self._tasks[task] = (server, lease, key)
        task.add_done_callback(lambda done: (self._tasks.pop(done, None), complete(done)))

    def _task_done(self, task: asyncio.Task) -> None:
        entity, lease, key = self._tasks.pop(task)
        self._cancel_requested.discard(task)
        try:
            if not task.cancelled():
                error = task.exception()
                if error is not None:
                    _LOGGER.error("Callback task failed", exc_info=(type(error), error, error.__traceback__))
        finally:
            lease.release()
            self._task_counts[key] -= 1
        try:
            self._control()
            self._restore_registrations()
        except BaseException as error:
            self._record_fault(error)

    def _consume_batch(self) -> None:
        with self._lock:
            batch = self._pending_batch
        if batch is None:
            return
        try:
            self._control()
            for ready in batch.entries:
                with self._lock:
                    record = self._registrations.get(ready.registration)
                # Pausing a channel can retire tokens in this same batch.
                if record is None:
                    continue
                if record.generation != ready.attachment_generation:
                    raise InvalidStateError("Async ready batch crossed an attachment generation")
                entity = record.entity
                if self.state is not ExecutorState.RUNNING or not self.context.ok() or entity._closing:
                    continue
                if entity._dispatch_kind == "client":
                    entity._take_response(ready.pin)
                    continue
                if entity._dispatch_kind == "action_client":
                    entity._take_ready(ready.pin, self)
                    continue
                if entity._dispatch_kind == "action_server":
                    entity._take_ready(ready.pin, self)
                    continue
                kind = "service" if entity._dispatch_kind == "service" else "general"
                if not self._has_task_capacity(entity, kind):
                    self._pause_registration(record)
                    continue
                key = self._task_key(entity, kind)
                if kind == "service":
                    self._ensure_route()
                    completion = _native._Completion()
                    completion.attachment_generation = record.generation
                    completion.entity_id = entity._native.entity_id
                    lease = _native._ServiceRequestWork(self._dispatcher, self._port,
                                                        completion, entity._native)
                else:
                    lease = entity._native.work_lease()
                self._task_counts[key] = self._task_counts.get(key, 0) + 1
                accepted = False
                try:
                    message = (lease.receive(entity._native, ready.pin)
                               if kind == "service" else entity._take(ready.pin))
                    if message is not None:
                        self._create_callback_task(entity, message, lease, kind)
                        accepted = True
                finally:
                    if not accepted:
                        self._task_counts[key] -= 1
                        lease.release()
                if not self._has_task_capacity(entity, kind):
                    self._pause_registration(record)
        except BaseException as error:
            self._record_fault(error)
        finally:
            batch.release()
            with self._lock:
                self._pending_batch = None
            self._batch_ack.set()

    def _request_shutdown(self, *, cause: BaseException | None, cancel_tasks: bool) -> None:
        if self.state is ExecutorState.STOPPED:
            return
        super()._request_shutdown(cause=cause, cancel_tasks=cancel_tasks)
        self._schedule_loop(self._start_drain)

    def _start_drain(self) -> None:
        if self._drain_task is None and self._fault is None and self.state is not ExecutorState.STOPPED:
            self._loop_thread = threading.get_ident()
            # Coordinator is deliberately outside the managed TaskRegistry.
            self._drain_task = self.loop.create_task(self._drain_async())

    async def _drain_async(self) -> None:
        try:
            while True:
                with self._lock:
                    futures = tuple(self._futures)
                for entity in tuple(self._records):
                    self._cancel_entity_operations(entity, self._cause)
                for future in futures:
                    future._try_set_exception(self._cause)
                self._control()
                if self._cancel_tasks:
                    for task in tuple(self._tasks):
                        if task not in self._cancel_requested:
                            self._cancel_requested.add(task)
                            task.cancel()
                with self._lock:
                    ready = (not self._tasks and not self._operations and self._pending_batch is None and
                             not self._notifications and self._notification_producers == 0)
                    if ready:
                        self._wait_stop = True
                        self._wait_set.wake()
                if ready:
                    break
                await asyncio.sleep(0.01)
            if self._wait_thread is not None:
                await asyncio.to_thread(self._wait_thread.join)
            # A terminal Future can still register a notification while the
            # wait thread is joining. Drain and recheck under admission lock.
            while self.state is not ExecutorState.STOPPED:
                self._drain_shutdown()
                if self.state is not ExecutorState.STOPPED:
                    await asyncio.sleep(0.01)
        except BaseException as error:
            self._record_fault(error)

    async def _shutdown_join(self) -> bool:
        return await self._shutdown_barrier

    async def shutdown_async(self, *, cancel_tasks: bool = True,
                             timeout: float | None = None) -> bool:
        if self._is_managed_current_task():
            raise InvalidStateError("Managed Task cannot await its own executor shutdown barrier")
        timeout = _timeout(timeout)
        deadline = None if timeout is None else time.monotonic() + timeout
        self.request_shutdown(cancel_tasks=cancel_tasks)
        waiter = asyncio.create_task(self._shutdown_barrier._wait_async())
        try:
            if deadline is None:
                return await asyncio.shield(waiter)
            return await asyncio.wait_for(asyncio.shield(waiter), max(0.0, deadline - time.monotonic()))
        except TimeoutError as error:
            raise DclpyTimeoutError("Async executor shutdown has not completed") from error
        finally:
            if not waiter.done():
                waiter.cancel()

    def shutdown(self, *, cancel_tasks: bool = True, timeout: float | None = None) -> bool:
        if self._in_owner_thread():
            raise InvalidStateError("Blocking shutdown cannot run on the bound event-loop thread")
        timeout = _timeout(timeout)
        self.request_shutdown(cancel_tasks=cancel_tasks)
        try:
            return self._shutdown_barrier._blocking_result(timeout)
        except TimeoutError as error:
            raise DclpyTimeoutError("Async executor shutdown has not completed") from error

    def spin(self) -> None:
        raise InvalidStateError("AsyncIOExecutor uses start on its bound loop")

    def spin_once(self, timeout_sec: float | None = None) -> None:
        raise InvalidStateError("AsyncIOExecutor uses start on its bound loop")
