"""Fault-injection PID selection on direct and Darwin-supervised runtimes."""
from pathlib import Path
import unittest

from owned_engine_process import select_engine_process


class OwnedEngineProcessTest(unittest.TestCase):
    runtime = Path("/fixture/runtime with spaces")

    def target(self, rows, program="segment_node", supervised=False):
        return select_engine_process(rows, 100, 200, self.runtime, program, supervised)

    def test_direct_engine(self):
        self.assertEqual(self.target([(200, 100, 200, "segment_node")]), 200)

    def test_supervised_engine(self):
        rows = [(200, 100, 200, str(self.runtime / "lumabri")),
                (201, 200, 200, str(self.runtime / "segment_node"))]
        self.assertEqual(self.target(rows, supervised=True), 201)

    def test_supervised_host_not_supervisor(self):
        rows = [(200, 100, 200, str(self.runtime / "lumabri")),
                (201, 200, 200, str(self.runtime / "lumabri")),
                (202, 201, 200, str(self.runtime / "segment_chat"))]
        self.assertEqual(self.target(rows, program="lumabri", supervised=True), 201)

    def test_unrelated_engine_is_not_a_target(self):
        rows = [(200, 100, 200, "lumabri"), (201, 999, 200, "segment_node")]
        with self.assertRaises(AssertionError):
            self.target(rows, supervised=True)

    def test_wrong_owner_group_or_executable(self):
        for row in [(200, 999, 200, "segment_node"), (200, 100, 999, "segment_node"),
                    (200, 100, 200, "not_segment_node"),
                    (200, 100, 200, "/another/runtime/segment_node")]:
            with self.subTest(row=row), self.assertRaises(AssertionError):
                self.target([row])

    def test_wrong_supervisor_or_descendant(self):
        for rows in [
            [(200, 100, 200, "unexpected"), (201, 200, 200, "segment_node")],
            [(200, 100, 200, "lumabri"), (201, 200, 999, "segment_node")],
            [(200, 100, 200, "lumabri"), (201, 200, 200, "other")],
            [(200, 100, 200, "lumabri"), (201, 200, 200, "segment_node"),
             (202, 200, 200, "segment_node")],
        ]:
            with self.subTest(rows=rows), self.assertRaises(AssertionError):
                self.target(rows, supervised=True)


if __name__ == "__main__":
    unittest.main()
