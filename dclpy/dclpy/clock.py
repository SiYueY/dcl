"""Clock wrappers over the Context-owned DMW clock primitive."""
from __future__ import annotations

from enum import Enum
from typing import Any

from . import _dclpy as _native
from .exceptions import EntityClosedError


class ClockType(Enum):
    SYSTEM = _native.ClockType.SYSTEM_TIME
    STEADY = _native.ClockType.STEADY_TIME
    ROS = _native.ClockType.ROS_TIME


class Time:
    def __init__(self, nanoseconds: int = 0, *, clock_type: ClockType = ClockType.SYSTEM) -> None:
        if type(nanoseconds) is not int:
            raise TypeError("nanoseconds must be an int")
        self.nanoseconds = nanoseconds
        self.clock_type = clock_type

    @classmethod
    def _from_native(cls, value: Any) -> "Time":
        return cls(value.nanoseconds, clock_type=ClockType(value.clock_type))

    def _to_native(self) -> Any:
        value = _native.Time()
        value.nanoseconds = self.nanoseconds
        value.clock_type = self.clock_type.value
        return value


class Clock:
    def __init__(self, context: Any, clock_type: ClockType = ClockType.SYSTEM) -> None:
        context._check_open()
        if not isinstance(clock_type, ClockType):
            raise TypeError("clock_type must be a ClockType")
        self.context = context
        self.clock_type = clock_type
        self._native = _native._Clock(context._native, clock_type.value)
        self._closing = False

    def _check_open(self) -> None:
        self.context._check_open()
        if self._closing:
            raise EntityClosedError("Clock is closing")

    def now(self) -> Time:
        self._check_open()
        return Time._from_native(self._native.now())

    def enable_ros_time_override(self, enabled: bool) -> None:
        self._check_open()
        if type(enabled) is not bool:
            raise TypeError("enabled must be bool")
        self._native.enable_ros_time_override(enabled)

    def set_ros_time(self, value: Time) -> None:
        self._check_open()
        if not isinstance(value, Time):
            raise TypeError("value must be a Time")
        if value.clock_type is not ClockType.ROS:
            raise TypeError("ROS time override requires a ROS clock Time")
        self._native.set_ros_time(value._to_native())

    @property
    def ros_time_override_enabled(self) -> bool:
        self._check_open()
        return self._native.ros_time_override_enabled()

    def close(self) -> None:
        self._closing = True
        self._native.close()

    def wait_closed(self, timeout: float | None = None) -> bool:
        return self._native.wait_closed(timeout)
