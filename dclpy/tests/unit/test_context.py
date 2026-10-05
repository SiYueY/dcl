import asyncio

import pytest

from dclpy import Context, Node, Parameter, ParameterDescriptor
from dclpy.exceptions import ContextShutdownError, EntityClosedError, InvalidNameError, InvalidStateError


def test_context_closes_all_children_and_rejects_new_work():
    context = Context(domain_id=213)
    first = Node("first", context=context)
    second = Node("second", context=context)
    first.declare_parameter("width", 0.04)
    assert first.get_parameter("width").value == 0.04
    assert context.shutdown(timeout=5)
    assert first.wait_closed(0)
    assert second.wait_closed(0)
    with pytest.raises(ContextShutdownError):
        first.get_parameter("width")
    with pytest.raises(ContextShutdownError):
        Node("late", context=context)
    assert context.shutdown(timeout=0)


def test_node_close_rejects_operations_without_shutting_other_nodes():
    with Context(domain_id=213) as context:
        first = Node("closed", context=context)
        second = Node("active", context=context)
        first.close()
        assert first.wait_closed(1)
        with pytest.raises(EntityClosedError):
            first.get_name()
        second.declare_parameter("active", True)
        assert second.get_parameter("active").value is True


def test_parameter_batch_constraints_are_atomic():
    with Context(domain_id=213) as context:
        node = Node("parameters", context=context)
        node.declare_parameter("width", 0.04)
        read_only = ParameterDescriptor()
        read_only.read_only = True
        node.declare_parameter("identity", "robot", read_only)
        with pytest.raises(InvalidStateError):
            node.set_parameters_atomically([Parameter("width", 0.08), Parameter("identity", "other")])
        assert node.get_parameter("width").value == 0.04
        changes = node.set_parameters_atomically([Parameter("width", 0.06)])
        assert changes.changed_parameters[0].name == "width"
        assert changes.changed_parameters[0].value == 0.06


def test_native_errors_preserve_expected_exception_class():
    with Context(domain_id=213) as context:
        with pytest.raises(InvalidNameError):
            Node("invalid/name", context=context)


def test_repeated_async_shutdown_joins_one_coordinator():
    async def run():
        context = Context(domain_id=213)
        node = Node("async_close", context=context)
        assert all(await asyncio.gather(*(context.shutdown_async(timeout=5) for _ in range(8))))
        assert node.wait_closed(0)
        assert context.shutdown(timeout=0)
    asyncio.run(run())
