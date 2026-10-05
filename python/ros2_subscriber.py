"""Print the values published by ROS2PublisherSink, decoded with data_tamer_parser.

Handles both modes of the sink: one data_tamer_msgs/Snapshot per sample on
`<prefix>/data`, or data_tamer_msgs/SnapshotBatch on `<prefix>/data_batch`
(ROS2PublisherOptions::aggregate), with schemas in either rendering. Needs a
sourced ROS 2 workspace that contains data_tamer_msgs:

    python3 python/ros2_subscriber.py --prefix /test
    ros2 run data_tamer_cpp ros2_publisher --aggregate --yaml   # in another shell
"""
import argparse

import rclpy
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, HistoryPolicy, QoSProfile, ReliabilityPolicy

from data_tamer_msgs.msg import Schemas, Snapshot, SnapshotBatch

import data_tamer_parser as dt


class DataTamerSubscriber(Node):
    def __init__(self, prefix: str, max_values: int):
        super().__init__("data_tamer_subscriber")
        self.registry = dt.SchemaRegistry()
        self.max_values = max_values
        # the publisher latches the latest complete schema catalog: subscribe
        # transient-local to get it late
        latched = QoSProfile(history=HistoryPolicy.KEEP_LAST, depth=1,
                             reliability=ReliabilityPolicy.RELIABLE,
                             durability=DurabilityPolicy.TRANSIENT_LOCAL)
        # best effort matches both a reliable publisher (the default data_qos) and
        # a best-effort one, whereas a reliable subscriber would receive nothing
        # from a best-effort publisher. The price is that samples may be lost.
        data = QoSProfile(history=HistoryPolicy.KEEP_LAST, depth=100,
                          reliability=ReliabilityPolicy.BEST_EFFORT)
        self.create_subscription(Schemas, prefix + "/schemas", self.registry.add_schemas, latched)
        self.create_subscription(Snapshot, prefix + "/data", self.on_snapshot, data)
        self.create_subscription(SnapshotBatch, prefix + "/data_batch", self.on_batch, data)

    def on_snapshot(self, msg: Snapshot):
        schema = self.registry.find(msg.schema_hash)
        if schema is None:
            self.get_logger().warn("snapshot with an unknown schema; waiting for /schemas")
            return
        self.show(schema, msg.timestamp_nsec, dt.parse_snapshot_msg(schema, msg))

    def on_batch(self, msg: SnapshotBatch):
        decoded = list(dt.iter_snapshot_batch(self.registry, msg))
        self.get_logger().info(
            f"batch: {len(msg.snapshots)} snapshots, {len(msg.schemas)} embedded schemas, "
            f"{len(decoded)} decoded")
        if decoded:
            self.show(*decoded[-1])

    def show(self, schema: dt.Schema, timestamp_nsec: int, values: dict):
        shown = list(values.items())[:self.max_values]
        text = ", ".join(f"{k}={v:.3g}" if isinstance(v, float) else f"{k}={v}" for k, v in shown)
        self.get_logger().info(f"[{schema.channel_name}] t={timestamp_nsec}: {text}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--prefix", default="/test", help="topic prefix of the sink")
    parser.add_argument("--max-values", type=int, default=4, help="values printed per sample")
    args, ros_args = parser.parse_known_args()
    rclpy.init(args=ros_args)
    node = DataTamerSubscriber(args.prefix, args.max_values)
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.try_shutdown()


if __name__ == "__main__":
    main()
