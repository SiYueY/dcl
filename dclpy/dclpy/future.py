"""Thread-safe terminal arbitration with independent asyncio await bridges."""
from __future__ import annotations

import asyncio
from collections.abc import Callable, Generator
from enum import Enum, auto
import logging
import threading
from typing import Generic, TypeVar, Any, cast
import weakref

from .exceptions import InvalidStateError, ExecutorStoppedError

_T = TypeVar("_T")
_LOGGER = logging.getLogger(__name__)


class _State(Enum):
    PENDING = auto()
    FINISHED = auto()
    CANCELED = auto()


class Future(Generic[_T]):
    def __init__(self, *, executor: Any = None) -> None:
        self._condition = threading.Condition()
        self._state = _State.PENDING
        self._result: _T | None = None
        self._exception: BaseException | None = None
        self._cancel_hook: Callable[[], Any] | None = None
        self._callbacks: list[Callable[[Future[_T]], Any]] = []
        self._bridges: list[Callable[[Future[_T]], Any]] = []
        self._executor = weakref.ref(executor) if executor is not None else None
        if executor is not None:
            executor._register_future(self)

    def _set_cancel_hook(self, hook: Callable[[], Any]) -> None:
        with self._condition:
            if self._state is not _State.PENDING:
                raise InvalidStateError("Cannot attach native cancellation to a terminal Future")
            self._cancel_hook = hook

    def done(self) -> bool:
        with self._condition:
            return self._state is not _State.PENDING

    def cancelled(self) -> bool:
        with self._condition:
            return self._state is _State.CANCELED

    def cancel(self) -> bool:
        return self._finish(_State.CANCELED)

    def _try_set_result(self, value: _T) -> bool:
        return self._finish(_State.FINISHED, result=value)

    def _try_set_exception(self, error: BaseException) -> bool:
        if not isinstance(error, BaseException):
            raise TypeError("Future exception must be a BaseException")
        return self._finish(_State.FINISHED, exception=error)

    def set_result(self, value: _T) -> None:
        if not self._try_set_result(value):
            raise InvalidStateError("Future already reached a terminal state")

    def set_exception(self, error: BaseException) -> None:
        if not self._try_set_exception(error):
            raise InvalidStateError("Future already reached a terminal state")

    def _finish(self, state: _State, *, result: _T | None = None,
                exception: BaseException | None = None) -> bool:
        with self._condition:
            if self._state is not _State.PENDING:
                return False
        owner = self._executor() if self._executor is not None else None
        if owner is not None:
            owner._begin_future_notification()
        try:
            with self._condition:
                if self._state is not _State.PENDING:
                    return False
                if state is _State.CANCELED and self._cancel_hook is not None:
                    # Only the private native queue cancellation hook runs
                    # here; application callbacks always run after unlocking.
                    self._cancel_hook()
                self._cancel_hook = None
                self._state = state
                self._result = result
                self._exception = exception
                callbacks, self._callbacks = self._callbacks, []
                bridges, self._bridges = self._bridges, []
                self._condition.notify_all()
            # This producer reservation spans the terminal commit and every
            # owner enqueue. STOPPED cannot overtake the publication gap.
            for callback in callbacks:
                self._schedule(callback)
            for bridge in bridges:
                self._invoke(bridge)
            return True
        finally:
            if owner is not None:
                owner._end_future_notification()

    def result(self) -> _T:
        with self._condition:
            if self._state is _State.PENDING:
                raise InvalidStateError("Future is pending")
            if self._state is _State.CANCELED:
                raise asyncio.CancelledError()
            if self._exception is not None:
                raise self._exception
            return cast(_T, self._result)

    def exception(self) -> BaseException | None:
        with self._condition:
            if self._state is _State.PENDING:
                raise InvalidStateError("Future is pending")
            if self._state is _State.CANCELED:
                raise asyncio.CancelledError()
            return self._exception

    def _blocking_result(self, timeout: float | None) -> _T:
        with self._condition:
            if not self._condition.wait_for(lambda: self._state is not _State.PENDING, timeout):
                raise TimeoutError("Future is still pending")
        return self.result()

    def add_done_callback(self, callback: Callable[[Future[_T]], Any]) -> None:
        if not callable(callback):
            raise TypeError("Done callback must be callable")
        owner = self._executor() if self._executor is not None else None
        if owner is not None:
            owner._begin_future_notification()
        try:
            with self._condition:
                if self._state is _State.PENDING:
                    self._callbacks.append(callback)
                    return
            self._schedule(callback)
        finally:
            if owner is not None:
                owner._end_future_notification()

    def remove_done_callback(self, callback: Callable[[Future[_T]], Any]) -> int:
        with self._condition:
            original = len(self._callbacks)
            self._callbacks = [entry for entry in self._callbacks if entry is not callback]
            return original - len(self._callbacks)

    def _schedule(self, callback: Callable[[Future[_T]], Any]) -> None:
        if self._executor is not None:
            executor = self._executor()
            if executor is None:
                _LOGGER.error("Future callback owner was destroyed before notification")
                return
            try:
                executor._schedule_done_callback(callback, self)
            except ExecutorStoppedError:
                _LOGGER.error("Future callback owner has already stopped")
            return
        self._invoke(callback)

    def _invoke(self, callback: Callable[[Future[_T]], Any]) -> None:
        try:
            callback(self)
        except BaseException:
            _LOGGER.exception("Future done callback failed")

    async def _wait_async(self) -> _T:
        loop = asyncio.get_running_loop()
        bridge = loop.create_future()

        def transfer() -> None:
            if bridge.done():
                return
            if self.cancelled():
                bridge.cancel()
            else:
                error = self.exception()
                if error is not None:
                    bridge.set_exception(error)
                else:
                    bridge.set_result(self.result())

        def completed(_future: Future[_T]) -> None:
            try:
                loop.call_soon_threadsafe(transfer)
            except RuntimeError:
                # A closed loop invalidates this bridge, never the underlying
                # operation or the bridges belonging to other event loops.
                self._remove_bridge(completed)

        with self._condition:
            pending = self._state is _State.PENDING
            if pending:
                self._bridges.append(completed)
        if not pending:
            completed(self)
        try:
            return await bridge
        finally:
            self._remove_bridge(completed)

    def _remove_bridge(self, bridge: Callable[[Future[_T]], Any]) -> None:
        with self._condition:
            self._bridges = [entry for entry in self._bridges if entry is not bridge]

    def __await__(self) -> Generator[Any, None, _T]:
        return self._wait_async().__await__()
