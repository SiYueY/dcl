"""Contract checks against generated providers, including real robot Actions."""
import ctypes
import gc

import pytest

from dclpy import _dclpy as native
from dclpy.exceptions import BusyError, TypeMismatchError
from builtin_interfaces_dclpy.msg import Time
from geometry_msgs_dclpy.msg import PoseArray, Pose
from mfr3duo_msgs_dclpy.action import Move, Grasp
from nav_msgs_dclpy.msg import Odometry
from std_msgs_dclpy.msg import String, Header, UInt8
from std_srvs_dclpy.srv import SetBool


def test_action_and_service_metadata_match_native_constituents():
    registry = native._BindingRegistry()
    registry.validate_action(Move)
    registry.validate_action(Grasp)
    registry.validate_service(SetBool)
    request = Move.Impl.SendGoalService.Request(goal=Move.Goal(width=0.04, speed=0.05))
    assert request.goal.width == 0.04
    assert type(Move.Impl.GoalStatusMessage()) is type(Grasp.Impl.GoalStatusMessage())
    assert Move.Impl.CancelGoalService is Grasp.Impl.CancelGoalService
    registry.clear()


def test_outbound_snapshot_and_clone_do_not_share_mutable_storage():
    registry = native._BindingRegistry()
    message = String(data="original")
    snapshot = registry.snapshot(String, message)
    message.data = "changed"
    clone = snapshot.clone()
    assert snapshot.materialize().data == clone.materialize().data == "original"
    with pytest.raises(BusyError):
        registry.clear()
    del snapshot, clone
    gc.collect()
    registry.clear()
    with pytest.raises(TypeMismatchError):
        registry.snapshot(String, Header())


def test_nested_views_keep_parent_alive_and_survive_inline_assignment():
    parent = Odometry()
    child = parent.header.stamp
    parent.header = Header(stamp=Time(sec=8))
    assert child.sec == 8
    child.sec = 12
    assert parent.header.stamp.sec == 12
    del parent
    gc.collect()
    child.sec = 15
    assert child.sec == 15


def test_message_sequences_return_detached_elements():
    message = PoseArray(poses=[Pose()])
    copied = message.poses
    copied[0].position.x = 7
    assert message.poses[0].position.x == 0
    message.poses = copied
    copied[0].position.x = 9
    assert message.poses[0].position.x == 7
    with pytest.raises(TypeError):
        message.poses = [Pose(), Header()]
    assert len(message.poses) == 1
    assert message.poses[0].position.x == 7


def test_scalar_range_and_setter_failure_preserve_old_values():
    value = UInt8(data=255)
    for invalid in (-1, 256, 1 << 80):
        with pytest.raises(OverflowError):
            value.data = invalid
        assert value.data == 255
    text = String(data="kept")
    with pytest.raises(TypeError):
        text.data = 12
    assert text.data == "kept"
    with pytest.raises(ValueError):
        Move.Impl.SendGoalService.Request().goal_id.uuid = [1] * 15


def test_capsule_validation_fails_before_accessing_missing_adapters():
    class HeaderV1(ctypes.Structure):
        _fields_ = [("abi_version", ctypes.c_uint32), ("struct_size", ctypes.c_uint32),
                    ("compatibility_id", ctypes.c_char_p)]

    header = HeaderV1(99, ctypes.sizeof(HeaderV1), b"incompatible")
    create = ctypes.pythonapi.PyCapsule_New
    create.restype = ctypes.py_object
    create.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]

    class Incompatible:
        __dclpy_message_binding__ = create(ctypes.addressof(header), b"dclpy.MessageBindingV1", None)

    with pytest.raises(ImportError):
        native._BindingRegistry().message(Incompatible)

    class WrongClass:
        __dclpy_message_binding__ = String.__dclpy_message_binding__

    with pytest.raises(ImportError):
        native._BindingRegistry().message(WrongClass)
