"""Immutable QoS policy values, copied at each native entity creation."""
from __future__ import annotations

from dataclasses import dataclass, field

from . import _dclpy as _native
from .exceptions import InvalidArgumentError

HistoryPolicy = _native.HistoryPolicy
ReliabilityPolicy = _native.ReliabilityPolicy
DurabilityPolicy = _native.DurabilityPolicy
LivelinessPolicy = _native.LivelinessPolicy
QosDuration = _native.QosDuration


@dataclass(frozen=True)
class QoSProfile:
    depth: int = 10
    history: HistoryPolicy = HistoryPolicy.KEEP_LAST
    reliability: ReliabilityPolicy = ReliabilityPolicy.RELIABLE
    durability: DurabilityPolicy = DurabilityPolicy.VOLATILE
    liveliness: LivelinessPolicy = LivelinessPolicy.SYSTEM_DEFAULT
    deadline: QosDuration = field(default_factory=QosDuration.system_default)
    lifespan: QosDuration = field(default_factory=QosDuration.system_default)
    liveliness_lease_duration: QosDuration = field(default_factory=QosDuration.system_default)

    def __post_init__(self) -> None:
        if type(self.depth) is not int or self.depth < 0:
            raise InvalidArgumentError("QoS depth must be a nonnegative integer")
        if self.history == HistoryPolicy.KEEP_LAST and self.depth == 0:
            raise InvalidArgumentError("KEEP_LAST requires a positive depth")
        for value, kind in ((self.history, HistoryPolicy), (self.reliability, ReliabilityPolicy),
                            (self.durability, DurabilityPolicy), (self.liveliness, LivelinessPolicy),
                            (self.deadline, QosDuration), (self.lifespan, QosDuration),
                            (self.liveliness_lease_duration, QosDuration)):
            if not isinstance(value, kind):
                raise TypeError(f"Invalid QoS policy: expected {kind.__name__}")

    def _to_native(self) -> _native._Qos:
        qos = _native._Qos()
        if self.history == HistoryPolicy.KEEP_LAST:
            qos.keep_last(self.depth)
        elif self.history == HistoryPolicy.KEEP_ALL:
            qos.keep_all()
        else:
            qos.history_system_default()
        if self.reliability == ReliabilityPolicy.RELIABLE:
            qos.reliable()
        elif self.reliability == ReliabilityPolicy.BEST_EFFORT:
            qos.best_effort()
        else:
            qos.reliability_system_default()
        if self.durability == DurabilityPolicy.TRANSIENT_LOCAL:
            qos.transient_local()
        elif self.durability == DurabilityPolicy.VOLATILE:
            qos.volatile()
        else:
            qos.durability_system_default()
        qos.set_liveliness(self.liveliness)
        qos.set_deadline(self.deadline)
        qos.set_lifespan(self.lifespan)
        qos.set_liveliness_lease_duration(self.liveliness_lease_duration)
        return qos

    @classmethod
    def _from_native(cls, qos: _native._Qos) -> QoSProfile:
        return cls(depth=qos.depth, history=qos.history, reliability=qos.reliability,
                   durability=qos.durability, liveliness=qos.liveliness, deadline=qos.deadline,
                   lifespan=qos.lifespan, liveliness_lease_duration=qos.liveliness_lease_duration)


def _profile(value: QoSProfile | int) -> QoSProfile:
    if type(value) is int:
        return QoSProfile(depth=value)
    if not isinstance(value, QoSProfile):
        raise TypeError("QoS must be a QoSProfile or integer depth")
    return value


qos_profile_default = QoSProfile()
qos_profile_services_default = QoSProfile()
qos_profile_sensor_data = QoSProfile(depth=5, reliability=ReliabilityPolicy.BEST_EFFORT)
qos_profile_parameters = QoSProfile(depth=1000)
qos_profile_parameter_events = QoSProfile(depth=1000)
qos_profile_action_status_default = QoSProfile(depth=1, durability=DurabilityPolicy.TRANSIENT_LOCAL)
qos_profile_system_default = QoSProfile(depth=0, history=HistoryPolicy.SYSTEM_DEFAULT,
                                       reliability=ReliabilityPolicy.SYSTEM_DEFAULT,
                                       durability=DurabilityPolicy.SYSTEM_DEFAULT)
