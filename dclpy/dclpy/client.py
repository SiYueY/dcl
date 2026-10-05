"""Service correlation before RequestId registration and bounded staging."""
from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass
import logging
import time
from typing import Any

from . import _dclpy as _native
from ._entity import Entity
from .exceptions import (ContextShutdownError, EntityClosedError, InterruptedError,
                         InvalidStateError, ResourceExhaustedError)
from .qos import QoSProfile, _profile, qos_profile_services_default

_LOGGER = logging.getLogger(__name__)


@dataclass
class _EarlyResponse:
    response: Any
    candidates: frozenset[int]


class Client(Entity):
    _dispatch_kind = "client"

    def __init__(self, node: Any, service_type: type, service_name: str,
                 qos_profile: QoSProfile | int = qos_profile_services_default) -> None:
        node._check_open()
        self.service_type = service_type
        self.service_name = service_name
        binding = node.context._bindings.service(service_type)
        native = _native._Client(node._native, binding, service_name, _profile(qos_profile)._to_native())
        super().__init__(node, native)
        self._pending: dict[Any, int] = {}
        self._early: dict[Any, _EarlyResponse] = {}
        self._tombstones: OrderedDict[Any, float] = OrderedDict()
        self._unreconciled: set[int] = set()
        node._add_entity(self)

    def call_async(self, request: Any) -> Any:
        self._check_open()
        snapshot = self.context._bindings.snapshot(self.service_type.Request, request)
        owner = self._owner
        if owner is None:
            raise InvalidStateError("call_async requires an attached executor")
        return owner._submit_request(self, snapshot)

    def service_is_ready(self) -> bool:
        self._check_open()
        return self._native.service_is_ready()

    def wait_for_service(self, timeout_sec: float | None = None) -> bool:
        self._check_open()
        try:
            return self._native.wait_for_service(timeout_sec)
        except InterruptedError as error:
            cause = self._cause
            if cause is not None:
                raise cause from error
            if not self.context.ok():
                raise ContextShutdownError("Context shut down during availability wait") from error
            raise EntityClosedError("Client closed during availability wait") from error

    def _tombstone(self, request_id: Any) -> None:
        now = time.monotonic()
        while self._tombstones and next(iter(self._tombstones.values())) <= now:
            self._tombstones.popitem(last=False)
        if request_id in self._tombstones:
            return
        if len(self._tombstones) == 4096:
            self._tombstones.popitem(last=False)
        self._tombstones[request_id] = now + 60

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

    def _send_committed(self, operation_id: int, operation: Any, completion: Any) -> bool:
        request_id = completion.request_id
        try:
            completion.check()
        except BaseException as error:
            operation.future._try_set_exception(error)
            self._retire_candidate(operation_id)
            return False
        if completion.skipped or request_id is None:
            self._retire_candidate(operation_id)
            return False
        if self._closing:
            operation.future._try_set_exception(self._cause)
        operation.request_id = request_id
        early = self._early.pop(request_id, None)
        if operation.future.done() or self._closing:
            self._tombstone(request_id)
            keep = False
        elif early is not None:
            self._tombstone(request_id)
            operation.future._try_set_result(early.response)
            keep = False
        else:
            self._pending[request_id] = operation_id
            keep = True
        self._retire_candidate(operation_id)
        return keep

    def _take_response(self, pin: Any) -> None:
        received = self._native.receive(pin)
        if received is None:
            return
        request_id, response = received
        now = time.monotonic()
        expiry = self._tombstones.get(request_id)
        if expiry is not None:
            if expiry > now:
                return
            del self._tombstones[request_id]
        operation_id = self._pending.pop(request_id, None)
        if operation_id is not None:
            self._tombstone(request_id)
            self._owner._complete_request(operation_id, response)
            return
        candidates = frozenset(operation_id for operation_id in self._unreconciled
                               if self._owner._operations[operation_id].native_work.send_started)
        if not candidates:
            _LOGGER.debug("Dropping unknown service response with no unreconciled native send")
            return
        if request_id in self._early:
            return
        if len(self._early) == 4096:
            cause = ResourceExhaustedError("Early response staging capacity exhausted")
            self._request_close(cause)
            self._owner._cancel_entity_operations(self, cause)
            self._early.clear()
            return
        self._early[request_id] = _EarlyResponse(response, candidates)

    def _cancel_operation(self, operation_id: int, operation: Any) -> None:
        operation.native_work.cancel()
        request_id = operation.request_id
        if request_id is not None:
            self._pending.pop(request_id, None)
            self._early.pop(request_id, None)
            self._tombstone(request_id)
        self._retire_candidate(operation_id)

    def _retire_bindings(self) -> None:
        self._pending.clear()
        self._early.clear()
        self._tombstones.clear()
        self._unreconciled.clear()
        super()._retire_bindings()
