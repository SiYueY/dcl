"""A topic reader dispatched exclusively by its attached executor."""
from __future__ import annotations

from collections.abc import Callable
from typing import Any

from . import _dclpy as _native
from ._entity import Entity
from .qos import QoSProfile, _profile


class Subscription(Entity):
    _dispatch_kind = "subscription"
    def __init__(self, node: Any, message_type: type, topic: str,
                 callback: Callable[[Any], Any], qos: QoSProfile | int) -> None:
        if not callable(callback):
            raise TypeError("Subscription callback must be callable")
        node._check_open()
        self.message_type = message_type
        self.topic_name = topic
        self.callback = callback
        binding = node.context._bindings.message(message_type)
        native = _native._Subscription(node._native, binding, topic, _profile(qos)._to_native())
        super().__init__(node, native)
        node._add_entity(self)

    def _take(self, pin: Any) -> Any | None:
        received = self._native.receive(pin)
        return None if received is None else received[0]

    def get_actual_qos(self) -> QoSProfile:
        return QoSProfile._from_native(self._native.actual_qos())
