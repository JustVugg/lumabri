"""Report semantics and process ownership, not simulated inference evidence."""
import subprocess
import unittest
from unittest.mock import patch

from managed_soak_test import process_sample, summaries


class SoakReportTest(unittest.TestCase):
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
            "100 1 8 lumabri\n101 100 20 /tmp/runtime/segment_node\n"
            "102 101 4 segment_chat\n999 1 999999 unrelated\n"))
        with patch("managed_soak_test.subprocess.run", return_value=result), \
             patch("managed_soak_test.Path.is_dir", return_value=True), \
             patch("managed_soak_test.Path.iterdir", side_effect=PermissionError):
            sample = process_sample({100})
        self.assertEqual(set(sample["processes"]), {"100", "101", "102"})
        self.assertEqual(sample["rss_bytes"], 32 * 1024)
        self.assertTrue(all(p["descriptors"] is None for p in sample["processes"].values()))
        self.assertEqual(sample["processes"]["101"]["program"], "segment_node")

    def test_missing_owner_is_not_a_zero_resource_sample(self):
        result = subprocess.CompletedProcess([], 0, stdout="999 1 1024 unrelated\n")
        with patch("managed_soak_test.subprocess.run", return_value=result):
            with self.assertRaises(AssertionError):
                process_sample({100})


if __name__ == "__main__":
    unittest.main()
