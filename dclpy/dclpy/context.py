"""Context admission and the single, shielded shutdown coordinator."""
from __future__ import annotations

import asyncio
from enum import Enum, auto
import math
import os
import threading
import time
from typing import Any
import weakref

from . import _dclpy as _native
from .future import Future
from .exceptions import ContextShutdownError, DclpyTimeoutError, InvalidArgumentError, InvalidStateError


_FAULTED_CONTEXTS: set[Any] = set()


class _State(Enum):
    OPEN = auto()
    CLOSING = auto()
    CLOSED = auto()


def _timeout(value: float | None) -> float | None:
    if value is None:
        return None
    value = float(value)
    if not math.isfinite(value) or value < 0:
        raise InvalidArgumentError("Timeout must be finite and nonnegative")
    return value


class Context:
    def __init__(self, *, domain_id: int | None = None, participant_name: str = "dclpy",
                 args: list[str] | None = None, runtime_mode: Any = _native.RuntimeMode.ROS2) -> None:
        options = _native._ContextOptions()
        if domain_id is None:
            try:
                domain_id = int(os.environ.get("ROS_DOMAIN_ID", "0"))
            except ValueError as error:
                raise InvalidArgumentError("ROS_DOMAIN_ID must be an integer") from error
        if type(domain_id) is not int or not 0 <= domain_id <= 232:
            raise InvalidArgumentError("DDS domain_id must be between 0 and 232")
        options.domain_id = domain_id
        options.participant_name = participant_name
        options.runtime_mode = runtime_mode
        options.arguments = _native._parse_arguments(args or [])
        self._native = _native._Context(options)
        self._bindings = _native._BindingRegistry()
        self._lock = threading.RLock()
        self._state = _State.OPEN
        self._owners = weakref.WeakSet()
        self._nodes = weakref.WeakSet()
        self._cancel_tasks = False
        self._shutdown_cause: ContextShutdownError | None = None
        self._shutdown_result: Future[bool] | None = None
        self._coordinator_thread: threading.Thread | None = None
        self._dispatcher = None
        self._shutdown_owners: tuple[Any, ...] = ()
        self._shutdown_start_error: BaseException | None = None

    def _get_dispatcher(self) -> Any:
        with self._lock:
            self._check_open()
            if self._dispatcher is None:
                self._dispatcher = _native._NativeIoDispatcher(self._native)
            return self._dispatcher

    def create_clock(self, clock_type: Any) -> Any:
        from .clock import Clock, ClockType
        if not isinstance(clock_type, ClockType):
            raise TypeError("clock_type must be a ClockType")
        return Clock(self, clock_type)

    def create_graph_event(self) -> Any:
        from .graph import GraphEvent
        return GraphEvent(self)

    def get_graph_revision(self) -> int:
        self._check_open()
        return self._native.graph_revision()

    def get_graph_snapshot(self) -> Any:
        self._check_open()
        return self._native.graph_snapshot()

    def ok(self) -> bool:
        with self._lock:
            return self._state is _State.OPEN

    def _check_open(self) -> None:
        with self._lock:
            if self._state is not _State.OPEN:
                raise ContextShutdownError("Context no longer accepts user work")

    def _register_node(self, node: Any) -> None:
        with self._lock:
            self._check_open()
            self._nodes.add(node)

    def _register_owner(self, owner: Any) -> None:
        with self._lock:
            self._check_open()
            self._owners.add(owner)

    def _owners_snapshot(self) -> tuple[Any, ...]:
        with self._lock:
            return tuple(self._owners)

    def _reject_managed_wait(self) -> None:
        for owner in self._owners_snapshot():
            if owner._is_managed_current_task():
                raise InvalidStateError("Managed callbacks must request shutdown without awaiting their own barrier")

    def _reject_blocking_owner_wait(self) -> None:
        for owner in self._owners_snapshot():
            if owner._in_owner_thread():
                raise InvalidStateError("Blocking shutdown cannot run on this Context's executor or event-loop thread")

    def request_shutdown(self, *, cancel_tasks: bool = True) -> None:
        with self._lock:
            if self._state is _State.CLOSED:
                return
            self._cancel_tasks = self._cancel_tasks or bool(cancel_tasks)
            first = self._state is _State.OPEN
            if first:
                self._state = _State.CLOSING
                self._shutdown_cause = ContextShutdownError("Context shutdown was requested")
                self._native.stop_admission()
                self._shutdown_result = Future()
            if first:
                self._shutdown_owners = tuple(self._owners)
            owners, nodes = self._shutdown_owners, tuple(self._nodes)
            cause, cancellation = self._shutdown_cause, self._cancel_tasks
        # Publish the Context cause before any owner enters STOPPING. Calls
        # below only enqueue owner control; no callback registry migrates here.
        errors = []
        for owner in owners:
            try:
                owner._request_shutdown(cause=cause, cancel_tasks=cancellation)
            except BaseException as error:
                errors.append(error)
        for node in nodes:
            try:
                node._request_close(cause)
            except BaseException as error:
                errors.append(error)
        try:
            self._native.close_children()
        except BaseException as error:
            errors.append(error)
        if errors:
            with self._lock:
                if self._shutdown_start_error is None:
                    self._shutdown_start_error = errors[0]
                _FAULTED_CONTEXTS.add(self)
        if first:
            thread = threading.Thread(target=self._run_coordinator,
                                      name="dclpy-context-shutdown", daemon=True)
            with self._lock:
                self._coordinator_thread = thread
            thread.start()

    def _run_coordinator(self) -> None:
        try:
            asyncio.run(self._coordinate_shutdown())
        except BaseException as error:
            with self._lock:
                _FAULTED_CONTEXTS.add(self)
            self._shutdown_result._try_set_exception(error)
        else:
            with self._lock:
                self._state = _State.CLOSED
            self._shutdown_result._try_set_result(True)
            with self._lock:
                self._shutdown_owners = ()

    async def _coordinate_shutdown(self) -> None:
        with self._lock:
            if self._shutdown_start_error is not None:
                raise self._shutdown_start_error
            owners = self._shutdown_owners
        # Owners produce these barriers on their own spin/control thread or
        # loop. Each DCLPY await creates a bridge on this coordinator's loop,
        # so no foreign-loop Task/Future is directly awaited.
        for owner in owners:
            await owner._shutdown_join()
        with self._lock:
            dispatcher = self._dispatcher
        if dispatcher is not None:
            await asyncio.to_thread(dispatcher.shutdown)
        await asyncio.to_thread(self._native.finish_shutdown)
        # This thread holds GIL here, after all worker-owned samples and native
        # payload caches have retired. The registry refuses premature unload.
        for node in tuple(self._nodes):
            node._retire_bindings()
        self._bindings.clear()

    def shutdown(self, *, cancel_tasks: bool = True, timeout: float | None = None) -> bool:
        timeout = _timeout(timeout)
        deadline = None if timeout is None else time.monotonic() + timeout
        self._reject_blocking_owner_wait()
        self.request_shutdown(cancel_tasks=cancel_tasks)
        with self._lock:
            result = self._shutdown_result
        if result is None:
            return True
        try:
            remaining = None if deadline is None else max(0.0, deadline - time.monotonic())
            return result._blocking_result(remaining)
        except TimeoutError as error:
            raise DclpyTimeoutError("Context shutdown has not completed") from error

    async def shutdown_async(self, *, cancel_tasks: bool = True,
                             timeout: float | None = None) -> bool:
        timeout = _timeout(timeout)
        deadline = None if timeout is None else time.monotonic() + timeout
        self._reject_managed_wait()
        self.request_shutdown(cancel_tasks=cancel_tasks)
        with self._lock:
            result = self._shutdown_result
        if result is None:
            return True
        waiter = asyncio.create_task(result._wait_async())
        try:
            if timeout is None:
                return await asyncio.shield(waiter)
            remaining = max(0.0, deadline - time.monotonic())
            return await asyncio.wait_for(asyncio.shield(waiter), remaining)
        except TimeoutError as error:
            raise DclpyTimeoutError("Context shutdown has not completed") from error
        finally:
            if not waiter.done():
                waiter.cancel()  # Detaches this bridge; the coordinator continues.

    def __enter__(self) -> Context:
        self._check_open()
        return self

    def __exit__(self, *_exception: Any) -> None:
        self.shutdown()
