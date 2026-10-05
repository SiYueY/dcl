"""Inbound service work retains its preallocated response/discard ticket."""
from __future__ import annotations

import inspect
from typing import Any

from . import _dclpy as _native
from ._entity import Entity
from .exceptions import InvalidStateError
from .qos import QoSProfile, _profile, qos_profile_services_default


class Service(Entity):
    _dispatch_kind = "service"

    def __init__(self, node: Any, service_type: type, service_name: str, callback: Any,
                 qos_profile: QoSProfile | int = qos_profile_services_default,
                 max_pending_requests: int = 1024) -> None:
        if not callable(callback):
            raise TypeError("Service callback must be callable")
        if type(max_pending_requests) is not int or max_pending_requests <= 0:
            raise ValueError("max_pending_requests must be a positive integer")
        node._check_open()
        self.service_type = service_type
        self.service_name = service_name
        self.callback = callback
        binding = node.context._bindings.service(service_type)
        native = _native._Service(node._native, binding, service_name,
                                  _profile(qos_profile)._to_native(), max_pending_requests)
        super().__init__(node, native)
        node._add_entity(self)

    def _snapshot_response(self, value: Any) -> Any:
        return self.context._bindings.snapshot(self.service_type.Response, value)

    def _invoke_sync(self, request: Any, response: Any, work: Any) -> None:
        try:
            result = self.callback(request, response)
            if inspect.isawaitable(result):
                if inspect.iscoroutine(result):
                    result.close()
                raise InvalidStateError("Coroutine service callbacks require AsyncIOExecutor")
            # Match the ROS service callback contract: a callback may either
            # return the response object or fill the supplied response and
            # return None. Snapshot the chosen object before native delivery.
            work.respond(self._snapshot_response(response if result is None else result))
        except BaseException:
            work.discard()
            raise
        finally:
            work.release()

    async def _invoke_async(self, request: Any, response: Any, work: Any) -> None:
        try:
            result = self.callback(request, response)
            if inspect.isawaitable(result):
                result = await result
            work.respond(self._snapshot_response(response if result is None else result))
        except BaseException:
            work.discard()
            raise
        # The Task-done observer also handles cancellation before this coroutine
        # starts, and transfers the lease before dropping its Task reference.
