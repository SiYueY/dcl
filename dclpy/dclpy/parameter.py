"""Checked Python values for the DMW parameter store."""
from __future__ import annotations
from dataclasses import dataclass
from typing import Any

from . import _dclpy as _native
from .exceptions import InvalidArgumentError

ParameterType = _native.ParameterType
ParameterDescriptor = _native.ParameterDescriptor
IntegerRange = _native.IntegerRange
FloatingPointRange = _native.FloatingPointRange
ParameterListResult = _native.ParameterListResult

_SCALARS = {
    ParameterType.BOOL: (bool, "bool"),
    ParameterType.INTEGER: (int, "integer"),
    ParameterType.DOUBLE: (float, "double"),
    ParameterType.STRING: (str, "string"),
}
_ARRAYS = {
    ParameterType.BOOL_ARRAY: (bool, "bool_array"),
    ParameterType.INTEGER_ARRAY: (int, "integer_array"),
    ParameterType.DOUBLE_ARRAY: (float, "double_array"),
    ParameterType.STRING_ARRAY: (str, "string_array"),
}


def _checked_integer(value: int) -> None:
    if not -(1 << 63) <= value < (1 << 63):
        raise InvalidArgumentError("Parameter integer is outside int64 range")


def _make_value(value: Any, kind: ParameterType | None) -> _native._ParameterValue:
    if kind is None:
        if value is None:
            kind = ParameterType.NOT_SET
        elif type(value) is bool:
            kind = ParameterType.BOOL
        elif type(value) is int:
            kind = ParameterType.INTEGER
        elif type(value) is float:
            kind = ParameterType.DOUBLE
        elif type(value) is str:
            kind = ParameterType.STRING
        elif isinstance(value, (bytes, bytearray)):
            kind = ParameterType.BYTE_ARRAY
        elif isinstance(value, (list, tuple)) and value:
            kind = next((candidate for candidate, (element, _) in _ARRAYS.items()
                         if type(value[0]) is element), None)
        else:
            raise InvalidArgumentError("Empty parameter arrays require an explicit type")
    if kind == ParameterType.NOT_SET:
        if value is not None:
            raise InvalidArgumentError("NotSet parameters require None")
        return _native._ParameterValue()
    if kind == ParameterType.BYTE_ARRAY:
        if not isinstance(value, (bytes, bytearray)):
            raise InvalidArgumentError("ByteArray parameters require bytes or bytearray")
        return _native._ParameterValue.byte_array(list(value))
    if kind in _SCALARS:
        expected, constructor = _SCALARS[kind]
        if type(value) is not expected:
            raise InvalidArgumentError("Parameter value does not match its declared type")
        if expected is int:
            _checked_integer(value)
        return getattr(_native._ParameterValue, constructor)(value)
    if kind in _ARRAYS:
        expected, constructor = _ARRAYS[kind]
        if not isinstance(value, (list, tuple)) or any(type(item) is not expected for item in value):
            raise InvalidArgumentError("Parameter arrays require homogeneous element types")
        if expected is int:
            for item in value:
                _checked_integer(item)
        return getattr(_native._ParameterValue, constructor)(value)
    raise InvalidArgumentError("Unsupported parameter type")


class Parameter:
    Type = ParameterType

    def __init__(self, name: str, value: Any = None, *, type_: ParameterType | None = None) -> None:
        if not isinstance(name, str):
            raise InvalidArgumentError("Parameter name must be a string")
        self._native = _native._Parameter()
        self._native.name = name
        self._native.value = _make_value(value, type_)

    @classmethod
    def _from_native(cls, value: _native._Parameter) -> Parameter:
        instance = cls.__new__(cls)
        instance._native = value
        return instance

    @property
    def name(self) -> str:
        return self._native.name

    @property
    def type_(self) -> ParameterType:
        return self._native.value.type

    @property
    def value(self) -> Any:
        return self._native.value.value


@dataclass(frozen=True)
class ParameterChangeSet:
    new_parameters: tuple[Parameter, ...]
    changed_parameters: tuple[Parameter, ...]
    deleted_parameters: tuple[Parameter, ...]

    @classmethod
    def _from_native(cls, change: _native._ParameterChangeSet) -> ParameterChangeSet:
        return cls(*(tuple(Parameter._from_native(value) for value in group)
                     for group in (change.new_parameters, change.changed_parameters,
                                   change.deleted_parameters)))
