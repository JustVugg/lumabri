"""Driver contracts, not substituted inference or update evidence."""
from pathlib import Path
import os
import unittest
from unittest.mock import Mock, patch

from installed_fleet_update_test import packages, Terminal


class FleetContractTest(unittest.TestCase):
    def test_both_complete_packages_and_version_pair_are_checked(self):
        paths = [Path("/previous"), Path("/candidate")]
        with patch("installed_fleet_update_test.checked_package") as complete, \
                patch("installed_fleet_update_test.candidate_pair", return_value=["verified"]) as pair:
            self.assertEqual(packages(paths), ["verified"])
            self.assertEqual([call.args[0] for call in complete.call_args_list], paths)
            pair.assert_called_once_with(paths)
        with patch("installed_fleet_update_test.checked_package", side_effect=ValueError("incomplete")), \
                patch("installed_fleet_update_test.candidate_pair") as pair:
            with self.assertRaisesRegex(ValueError, "incomplete"):
                packages(paths)
            pair.assert_not_called()

    def test_terminal_detach_drains_and_close_is_idempotent(self):
        terminal = Terminal.__new__(Terminal)
        terminal.closed = False
        terminal.master, other = os.pipe()
        terminal.log = Mock()
        terminal.process = Mock()
        terminal.process.poll.side_effect = [None, None, 0]
        terminal.drain = Mock()
        try:
            terminal.close()
            self.assertTrue(terminal.closed)
            terminal.process.terminate.assert_called_once()
            terminal.process.kill.assert_not_called()
            self.assertGreaterEqual(terminal.drain.call_count, 2)
            terminal.close()
            terminal.log.close.assert_called_once()
        finally:
            os.close(other)
            if not terminal.closed:
                os.close(terminal.master)


if __name__ == "__main__":
    unittest.main()
