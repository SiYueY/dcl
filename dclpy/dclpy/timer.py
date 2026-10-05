"""Executor-dispatched, Clock-bound timers."""
from __future__ import annotations

from typing import Any

from . import _dclpy as _native
from ._entity import Entity
from .clock import Clock
from .exceptions import InvalidArgumentError


class Timer(Entity):
    _dispatch_kind = "timer"

    def __init__(self, node: Any, period_sec: float, callback: Any, *, clock: Clock,
                 autostart: bool = True) -> None:
        if not callable(callback):
            raise TypeError("Timer callback must be callable")
        if not isinstance(clock, Clock) or clock.context is not node.context:
            raise InvalidArgumentError("Timer clock must belong to the Node Context")
        if type(autostart) is not bool:
            raise TypeError("autostart must be bool")
        try:
            period_ns = int(float(period_sec) * 1_000_000_000)
        except (TypeError, ValueError, OverflowError) as error:
            raise InvalidArgumentError("Timer period must be a finite positive duration") from error
        if period_ns <= 0:
            raise InvalidArgumentError("Timer period must be positive")
        node._check_open()
        self.callback = callback
        self.clock = clock
        native = _native._Timer(clock._native, period_ns, autostart)
        super().__init__(node, native)
        node._add_entity(self)

    def _take(self, pin: Any) -> Any | None:
        return self._native.consume(pin)

    @property
    def timer_period_ns(self) -> int:
        return self._native.period_ns

    def exchange_period(self, period_sec: float) -> int:
        try:
            period_ns = int(float(period_sec) * 1_000_000_000)
        except (TypeError, ValueError, OverflowError) as error:
            raise InvalidArgumentError("Timer period must be a finite positive duration") from error
        if period_ns <= 0:
            raise InvalidArgumentError("Timer period must be positive")
        return self._native.exchange_period(period_ns)

    def cancel(self) -> None:
        self._check_open()
        self._native.cancel()

    def reset(self) -> None:
        self._check_open()
        self._native.reset()

    def is_canceled(self) -> bool:
        self._check_open()
        return self._native.is_canceled()

    def is_ready(self) -> bool:
        self._check_open()
        return self._native.is_ready()

    def time_until_next_call_ns(self) -> int:
        self._check_open()
        return self._native.time_until_next_call()
