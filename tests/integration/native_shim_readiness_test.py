"""Origin readiness must include tracker publication, not only a TCP listener."""
import json
import subprocess
import unittest
from unittest.mock import Mock, patch

from native_shim_test import wait_registered


class OriginReadinessTest(unittest.TestCase):
    def test_waits_for_the_exact_advertised_source(self):
        def reply(peers):
            return subprocess.CompletedProcess([], 0, json.dumps({"schema": 1, "peers": peers}), "")
        target = {"name": "origin", "model": "model", "storage": {"files": 4}}
        child = Mock(); child.poll.return_value = None
        with patch("native_shim_test.subprocess.run", side_effect=[
                reply([]), reply([{**target, "model": "unrelated"}]),
                reply([{**target, "name": "another"}]),
                reply([{**target, "storage": {"files": 3}}]), reply([target])]) as probe, \
                patch("native_shim_test.time.sleep") as sleep:
            wait_registered(child, "127.0.0.1:1234", "model", "origin", 4, {})
        self.assertEqual(probe.call_count, 5)
        self.assertEqual(sleep.call_count, 4)
        self.assertEqual(probe.call_args.args[0][1:], ["--tracker", "127.0.0.1:1234", "--model", "model"])

    def test_dead_origin_is_not_ready(self):
        child = Mock(); child.poll.return_value = 1
        with patch("native_shim_test.subprocess.run") as probe:
            with self.assertRaisesRegex(AssertionError, "origin exited"):
                wait_registered(child, "tracker", "model", "origin", 1, {})
            probe.assert_not_called()

    def test_unavailable_tracker_is_not_readiness(self):
        child = Mock(); child.poll.return_value = None
        with patch("native_shim_test.subprocess.run", return_value=subprocess.CompletedProcess([], 1, "", "unavailable")):
            with self.assertRaisesRegex(AssertionError, "unavailable"):
                wait_registered(child, "tracker", "model", "origin", 1, {})

    def test_unpublished_source_times_out(self):
        child = Mock(); child.poll.return_value = None
        with patch("native_shim_test.time.monotonic", side_effect=[0, 16]), \
                patch("native_shim_test.subprocess.run") as probe:
            with self.assertRaisesRegex(AssertionError, "deadline"):
                wait_registered(child, "tracker", "model", "origin", 1, {})
            probe.assert_not_called()


if __name__ == "__main__":
    unittest.main()
