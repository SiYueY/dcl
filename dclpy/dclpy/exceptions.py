"""Expected DCLPY failures, shared with the native exception translator."""
from ._dclpy import (
    DclpyError, InvalidArgumentError, InvalidStateError, InvalidNameError,
    TypeMismatchError, AlreadyExistsError, NotFoundError, AlreadyRegisteredError,
    NotRegisteredError, BusyError, DclpyTimeoutError, UnsupportedError,
    IncompatibleQosError, ParentDestroyedError, ResourceExhaustedError,
    MiddlewareError, ContextShutdownError, EntityClosedError, ExecutorStoppedError,
    InterruptedError, ProtocolFaultError,
)

__all__ = [
    "DclpyError", "InvalidArgumentError", "InvalidStateError", "InvalidNameError",
    "TypeMismatchError", "AlreadyExistsError", "NotFoundError", "AlreadyRegisteredError",
    "NotRegisteredError", "BusyError", "DclpyTimeoutError", "UnsupportedError",
    "IncompatibleQosError", "ParentDestroyedError", "ResourceExhaustedError",
    "MiddlewareError", "ContextShutdownError", "EntityClosedError", "ExecutorStoppedError",
    "InterruptedError", "ProtocolFaultError",
]
