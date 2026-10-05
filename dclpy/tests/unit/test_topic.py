import time

import pytest

from dclpy import Context, Node
from dclpy import _dclpy as native
from dclpy.exceptions import AlreadyRegisteredError, EntityClosedError, InvalidStateError
from dclpy.qos import QoSProfile, HistoryPolicy, QosDuration, InvalidArgumentError
from std_msgs_dclpy.msg import String


def test_qos_values_and_invalid_duration_access():
    qos = QoSProfile(depth=3, deadline=QosDuration.finite(4000))
    converted = qos._to_native()
    assert converted.depth == 3
    assert converted.deadline.nanoseconds == 4000
    with pytest.raises(InvalidArgumentError):
        QoSProfile(depth=0)
    with pytest.raises(InvalidStateError):
        QosDuration.infinite().nanoseconds
    assert QoSProfile(depth=0, history=HistoryPolicy.KEEP_ALL)._to_native().depth == 0


def test_real_topic_receive_private_instance_and_dispatch_close_barrier():
    context = Context(domain_id=202)
    node = Node("topic_test", context=context)
    subscriber = node.create_subscription(String, "/dclpy_topic_test", lambda msg: None, 10)
    publisher = node.create_publisher(String, "/dclpy_topic_test", 10)
    wait_set = native._WaitSet(context._native)
    token = wait_set.add(subscriber._native, 1)
    batch = None
    try:
        with pytest.raises(AlreadyRegisteredError):
            wait_set.add(subscriber._native, 2)
        deadline = time.monotonic() + 5
        message = None
        while time.monotonic() < deadline and message is None:
            publisher.publish(String(data="snapshot"))
            batch = wait_set.wait(0.1)
            for entry in batch.entries:
                message = subscriber._take(entry.pin)
            if message is None:
                batch.release()
                batch = None
        assert message.data == "snapshot"
        # Existing batch pins hold native destruction through CLOSING.
        subscriber.close()
        wait_set.remove(token)
        assert not subscriber.wait_closed(0)
        batch.release()
        batch = None
        assert subscriber.wait_closed(1)
        with pytest.raises(EntityClosedError):
            publisher.close()
            publisher.publish(String())
    finally:
        if batch is not None:
            batch.release()
        wait_set.close()
        context.shutdown(timeout=5)
