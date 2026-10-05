"""Topic publication with a private snapshot taken before releasing the GIL."""
from __future__ import annotations

from typing import Any

from . import _dclpy as _native
from ._entity import Entity
from .qos import QoSProfile, _profile


class Publisher(Entity):
    _dispatch_kind = "publisher"
    def __init__(self, node: Any, message_type: type, topic: str, qos: QoSProfile | int) -> None:
        node._check_open()
        self.message_type = message_type
        self.topic_name = topic
        binding = node.context._bindings.message(message_type)
        native = _native._Publisher(node._native, binding, topic, _profile(qos)._to_native())
        super().__init__(node, native)
        node._add_entity(self)

    def publish(self, message: Any) -> None:
        self._check_open()
        snapshot = self.context._bindings.snapshot(self.message_type, message)
        self._native.write(snapshot)

    def publish_async(self, message: Any) -> Any:
        self._check_open()
        snapshot = self.context._bindings.snapshot(self.message_type, message)
        owner = self._owner
        if owner is None:
            from .exceptions import InvalidStateError
            raise InvalidStateError("publish_async requires an attached executor")
        return owner._submit_publish(self, snapshot)

    def get_actual_qos(self) -> QoSProfile:
        return QoSProfile._from_native(self._native.actual_qos())
