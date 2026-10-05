"""Graph snapshots and manually-taken or executor-dispatched graph events."""
from __future__ import annotations

import threading
from typing import Any

from . import _dclpy as _native
from .exceptions import EntityClosedError, InvalidStateError


class GraphEvent:
    _dispatch_kind = "graph"

    def __init__(self, context: Any, *, node: Any = None, callback: Any = None) -> None:
        if (node is None) != (callback is None):
            raise ValueError("GraphEvent callback mode requires both node and callback")
        if callback is not None and not callable(callback):
            raise TypeError("GraphEvent callback must be callable")
        context._check_open()
        self.context = context
        self.node = node
        self.callback = callback
        self._native = _native._GraphEvent(context._native)
        self._lock = threading.RLock()
        self._closing = False
        self._cause: BaseException | None = None
        self._owner = None
        if node is not None:
            node._add_entity(self)

    def _check_open(self) -> None:
        self.context._check_open()
        with self._lock:
            if self._closing:
                raise EntityClosedError("Graph event is closing")

    def take(self) -> Any | None:
        self._check_open()
        if self.node is not None:
            raise InvalidStateError("Executor-owned GraphEvent cannot be manually taken")
        return self._native.take()

    def _take(self, pin: Any) -> Any | None:
        return self._native.take_with_pin(pin)

    def _request_close(self, cause: BaseException | None) -> None:
        with self._lock:
            if not self._closing:
                self._closing = True
                self._cause = cause or EntityClosedError("Graph event was closed")
            owner = self._owner
        self._native.close()
        if owner is not None:
            owner._entity_closing(self)
        elif self._native.state == _native._EntityState.CLOSED:
            self._retire_bindings()

    def close(self) -> None:
        self._request_close(EntityClosedError("Graph event was closed"))

    destroy = close

    def wait_closed(self, timeout: float | None = None) -> bool:
        closed = self._native.wait_closed(timeout)
        if closed:
            self._retire_bindings()
        return closed

    def _retire_bindings(self) -> None:
        self._native.retire_binding()
