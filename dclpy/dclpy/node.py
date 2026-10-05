"""Logical ROS/DDS node identity and its parameter store."""
from __future__ import annotations

import threading
from typing import Any

from . import _dclpy as _native
from .context import Context
from .exceptions import EntityClosedError
from .parameter import Parameter, ParameterDescriptor, ParameterChangeSet


class Node:
    def __init__(self, node_name: str, *, context: Context, namespace: str = "/",
                 cli_args: list[str] | None = None, use_global_arguments: bool = True,
                 allow_undeclared_parameters: bool = False) -> None:
        context._check_open()
        options = _native._NodeOptions()
        options.node_name = node_name
        options.namespace = namespace
        options.arguments = _native._parse_arguments(cli_args or [])
        options.use_global_arguments = use_global_arguments
        options.allow_undeclared_parameters = allow_undeclared_parameters
        self.context = context
        self._native = _native._Node(context._native, options)
        self._lock = threading.RLock()
        self._entities: set[Any] = set()
        self._owner = None
        self._attachment_generation = 0
        self._closing = False
        try:
            context._register_node(self)
        except BaseException:
            self._native.close()
            raise

    @property
    def executor(self) -> Any:
        with self._lock:
            return self._owner

    def get_name(self) -> str:
        return self._native.name

    def get_namespace(self) -> str:
        return self._native.namespace

    def get_fully_qualified_name(self) -> str:
        return self._native.fully_qualified_name

    def _check_open(self) -> None:
        self.context._check_open()
        with self._lock:
            if self._closing:
                raise EntityClosedError("Node is closing")

    def _add_entity(self, entity: Any) -> None:
        with self._lock:
            self._check_open()
            self._entities.add(entity)
            if self._owner is not None:
                try:
                    self._owner._add_entity(entity)
                except BaseException:
                    self._entities.discard(entity)
                    entity.close()
                    raise

    def _request_close(self, cause: BaseException | None) -> None:
        with self._lock:
            self._closing = True
            entities = tuple(self._entities)
        for entity in entities:
            entity._request_close(cause)
        self._native.close()

    def close(self) -> None:
        self._request_close(EntityClosedError("Node was closed"))

    destroy_node = close

    def wait_closed(self, timeout: float | None = None) -> bool:
        # Node teardown includes its children, using one caller deadline.
        from .context import _timeout
        import time
        timeout = _timeout(timeout)
        deadline = None if timeout is None else time.monotonic() + timeout
        with self._lock:
            entities = tuple(self._entities)
        for entity in entities:
            remaining = None if deadline is None else max(0.0, deadline - time.monotonic())
            if not entity.wait_closed(remaining):
                return False
        remaining = None if deadline is None else max(0.0, deadline - time.monotonic())
        return self._native.wait_closed(remaining)

    def _retire_bindings(self) -> None:
        with self._lock:
            entities = tuple(self._entities)
        for entity in entities:
            entity._retire_bindings()

    def create_service(self, service_type: type, service_name: str, callback: Any, *,
                       qos_profile: Any = None, max_pending_requests: int = 1024) -> Any:
        from .service import Service
        from .qos import qos_profile_services_default
        return Service(self, service_type, service_name, callback,
                       qos_profile_services_default if qos_profile is None else qos_profile,
                       max_pending_requests)

    def create_client(self, service_type: type, service_name: str, *, qos_profile: Any = None) -> Any:
        from .client import Client
        from .qos import qos_profile_services_default
        return Client(self, service_type, service_name,
                      qos_profile_services_default if qos_profile is None else qos_profile)

    def create_publisher(self, message_type: type, topic: str, qos_profile: Any) -> Any:
        from .publisher import Publisher
        return Publisher(self, message_type, topic, qos_profile)

    def create_subscription(self, message_type: type, topic: str, callback: Any,
                            qos_profile: Any) -> Any:
        from .subscription import Subscription
        return Subscription(self, message_type, topic, callback, qos_profile)

    def create_timer(self, period_sec: float, callback: Any, *, clock: Any = None,
                     autostart: bool = True) -> Any:
        from .clock import Clock, ClockType
        from .timer import Timer
        timer_clock = clock if clock is not None else Clock(self.context, ClockType.ROS)
        return Timer(self, period_sec, callback, clock=timer_clock, autostart=autostart)

    def create_graph_event(self, callback: Any) -> Any:
        from .graph import GraphEvent
        return GraphEvent(self.context, node=self, callback=callback)

    def declare_parameter(self, name: str, value: Any = None,
                          descriptor: ParameterDescriptor | None = None, *,
                          ignore_override: bool = False) -> Parameter:
        parameter = Parameter(name, value)
        result = self._native.declare_parameter(name, parameter._native.value,
                                               descriptor or ParameterDescriptor(), ignore_override)
        return Parameter._from_native(result)

    def undeclare_parameter(self, name: str) -> None:
        self._native.undeclare_parameter(name)

    def has_parameter(self, name: str) -> bool:
        return self._native.has_parameter(name)

    def get_parameter(self, name: str) -> Parameter:
        return Parameter._from_native(self._native.get_parameter(name))

    def get_parameters(self, names: list[str]) -> list[Parameter]:
        return [Parameter._from_native(value) for value in self._native.get_parameters(names)]

    def describe_parameter(self, name: str) -> ParameterDescriptor:
        return self._native.describe_parameter(name)

    def list_parameters(self, prefixes: list[str] | None = None, depth: int = 0) -> Any:
        return self._native.list_parameters(prefixes or [], depth)

    def validate_parameters(self, parameters: list[Parameter]) -> None:
        self._native.validate_parameters([parameter._native for parameter in parameters])

    def set_parameters_atomically(self, parameters: list[Parameter]) -> ParameterChangeSet:
        return ParameterChangeSet._from_native(
            self._native.set_parameters_atomically([parameter._native for parameter in parameters]))

    def take_parameter_changes(self) -> ParameterChangeSet:
        return ParameterChangeSet._from_native(self._native.take_parameter_changes())
