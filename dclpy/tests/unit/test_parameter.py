import pytest

from dclpy import Parameter, ParameterType
from dclpy import Context
from dclpy.exceptions import InvalidArgumentError
from dclpy.node import Node


@pytest.mark.parametrize("value,kind", [
    (None, ParameterType.NOT_SET), (True, ParameterType.BOOL),
    (5, ParameterType.INTEGER), (0.5, ParameterType.DOUBLE),
    ("robot", ParameterType.STRING), (b"\x00\xff", ParameterType.BYTE_ARRAY),
    ([True, False], ParameterType.BOOL_ARRAY), ([1, 2], ParameterType.INTEGER_ARRAY),
    ([1.0, 2.0], ParameterType.DOUBLE_ARRAY), (["a", "b"], ParameterType.STRING_ARRAY),
])
def test_parameter_mapping(value, kind):
    parameter = Parameter("test", value)
    assert parameter.type_ == kind
    assert parameter.value == value


@pytest.mark.parametrize("value", [1 << 63, -(1 << 63) - 1, [1, True], [1, 1.0], [], [None]])
def test_invalid_parameter_values_are_rejected_before_native_access(value):
    with pytest.raises(InvalidArgumentError):
        Parameter("test", value)


def test_explicit_empty_array_and_snapshot():
    original = [1, 2]
    parameter = Parameter("test", original)
    original.append(3)
    assert parameter.value == [1, 2]
    result = parameter.value
    result.append(4)
    assert parameter.value == [1, 2]
    assert Parameter("empty", [], type_=ParameterType.INTEGER_ARRAY).value == []
    with pytest.raises(InvalidArgumentError):
        Parameter("wrong", True, type_=ParameterType.INTEGER)


def test_node_atomic_parameter_change_set_and_validation():
    context = Context(participant_name="dclpy-parameter-test")
    try:
        node = Node("parameter_node", context=context)
        assert node.declare_parameter("enabled", False).value is False
        change = node.set_parameters_atomically([Parameter("enabled", True)])
        assert [value.name for value in change.changed_parameters] == ["enabled"]
        assert node.get_parameter("enabled").value is True
        pending = node.take_parameter_changes()
        assert [value.name for value in pending.new_parameters] == ["enabled"]
        assert [value.name for value in pending.changed_parameters] == ["enabled"]
        node.undeclare_parameter("enabled")
        assert [value.name for value in node.take_parameter_changes().deleted_parameters] == ["enabled"]
    finally:
        context.shutdown()
