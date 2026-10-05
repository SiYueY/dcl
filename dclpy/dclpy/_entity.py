"""Python ownership metadata over native, lease-protected resource backings."""
from __future__ import annotations

import threading
from typing import Any

from . import _dclpy as _native
from .exceptions import EntityClosedError


class Entity:
    def __init__(self, node: Any, native: Any) -> None:
        self.node = node
        self.context = node.context
        self._native = native
        self._lock = threading.RLock()
        self._closing = False
        self._cause: BaseException | None = None
        self._owner = None

    def _check_open(self) -> None:
        self.context._check_open()
        with self._lock:
            if self._closing:
                raise EntityClosedError("Entity is closing")

    def _request_close(self, cause: BaseException | None) -> None:
        with self._lock:
            if not self._closing:
                self._closing = True
                self._cause = cause or EntityClosedError("Entity was closed")
            owner = self._owner
        self._native.close()
        if owner is not None:
            owner._entity_closing(self)
        elif self._native.state == _native._EntityState.CLOSED:
            self._retire_bindings()

    def close(self) -> None:
        self._request_close(EntityClosedError("Entity was closed"))

    destroy = close

    def wait_closed(self, timeout: float | None = None) -> bool:
        closed = self._native.wait_closed(timeout)
        if closed:
            self._retire_bindings()
        return closed

    def _retire_bindings(self) -> None:
        self._native.retire_binding()
        with self.node._lock:
            self.node._entities.discard(self)
