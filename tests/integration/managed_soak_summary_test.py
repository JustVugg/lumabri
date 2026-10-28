"""Report semantics and process ownership, not simulated inference evidence."""
import subprocess
import unittest
from unittest.mock import Mock, patch

from managed_soak_test import hosts_quiescent, process_sample, processes_quiescent, summaries


class SoakReportTest(unittest.TestCase):
    def test_remote_cleanup_is_not_gateway_quiescence(self):
        def state(allocation, connections=0, requests=0):
            return 200, {"allocation": allocation, "state": "accepting",
                         "connections": connections, "admitted_requests": requests}
        api = Mock()
        api.http.side_effect = [state("a", 1, 0), state("b"),
                                state("a"), state("b", 1, 1),
                                state("a"), state("b")]
        self.assertFalse(hosts_quiescent(api.http, ("a", "b")))
        self.assertFalse(hosts_quiescent(api.http, ("a", "b")))
        self.assertTrue(hosts_quiescent(api.http, ("a", "b")))
        self.assertEqual([call.args for call in api.http.call_args_list],
                         [(f"/api/v1/allocations/{name}/control",) for name in ("a", "b") * 3])

    def test_unknown_or_inconsistent_host_is_not_idle(self):
        valid = {"allocation": "a", "state": "accepting", "connections": 0, "admitted_requests": 0}
        for code, state in ((503, {}), (401, {}), (200, {}),
                            (200, {**valid, "allocation": "b"}),
                            (200, {**valid, "state": "drained"}),
                            (200, {**valid, "connections": -1}),
                            (200, {**valid, "connections": False}),
                            (200, {**valid, "admitted_requests": 1})):
            with self.subTest(code=code, state=state):
                api = Mock()
                api.http.return_value = code, state
                with self.assertRaises((AssertionError, KeyError)):
                    hosts_quiescent(api.http, ("a",))

    def test_rejections_and_stops_are_not_success_percentiles(self):
        rows = [{"phase": "burst", "model": "a", "outcome": "completed",
                 "ttft_seconds": .1, "elapsed_seconds": .2,
                 "timing": {"token_notification_gaps": {"p95_seconds": .01}}},
                {"phase": "burst", "model": "a", "outcome": "rejected"},
                {"phase": "burst", "model": "a", "outcome": "client_disconnected"},
                {"phase": "burst", "model": "a", "outcome": "failed"},
                {"phase": "burst", "model": "b", "outcome": "rejected"}]
        first, second = summaries(rows)
        self.assertEqual([first[k] for k in ("requests", "completed", "rejected", "client_disconnected", "failed")],
                         [4, 1, 1, 1, 1])
        self.assertEqual(first["client_ttft_p95_seconds"], .1)
        self.assertEqual(first["percentiles_scope"], "completed_requests_only")
        self.assertIsNone(second["client_ttft_p95_seconds"])
        self.assertEqual(second["requests"], 1)

    def test_only_owned_descendants_and_unknown_descriptors(self):
        result = subprocess.CompletedProcess([], 0, stdout=(
            "100 1 8 S lumabri\n101 100 20 Sl /tmp/runtime/segment_node\n"
            "102 101 4 Sl segment_chat\n999 1 999999 R unrelated\n"))
        with patch("managed_soak_test.subprocess.run", return_value=result), \
             patch("managed_soak_test.Path.is_dir", return_value=True), \
             patch("managed_soak_test.Path.iterdir", side_effect=PermissionError):
            sample = process_sample({100})
        self.assertEqual(set(sample["processes"]), {"100", "101", "102"})
        self.assertEqual(sample["rss_bytes"], 32 * 1024)
        self.assertTrue(all(p["descriptors"] is None for p in sample["processes"].values()))
        self.assertEqual(sample["processes"]["101"]["program"], "segment_node")

    def test_missing_owner_is_not_a_zero_resource_sample(self):
        result = subprocess.CompletedProcess([], 0, stdout="999 1 1024 R unrelated\n")
        with patch("managed_soak_test.subprocess.run", return_value=result):
            with self.assertRaises(AssertionError):
                process_sample({100})

    def test_gateway_worker_cannot_enter_stable_baseline(self):
        # Reproduce the CI failure: a zero-RSS HTTP zombie can be present in
        # both samples before the parent's next poll/reap. It must disappear,
        # not become a permanent PID expected throughout the workload.
        permanent = {"100": {"parent": 1, "state": "S"},
                     "200": {"parent": 1, "state": "S"},
                     "201": {"parent": 200, "state": "Sl"}}
        self.assertTrue(processes_quiescent({"processes": permanent}, 100))
        for state in ("S", "R", "Z", "Z+"):
            observed = {"processes": {**permanent, "101": {"parent": 100, "state": state}}}
            self.assertFalse(processes_quiescent(observed, 100))
            self.assertFalse(processes_quiescent(observed, 100))
        lost_engine = {**permanent, "201": {"parent": 200, "state": "Z"}}
        self.assertFalse(processes_quiescent({"processes": lost_engine}, 100))


if __name__ == "__main__":
    unittest.main()
