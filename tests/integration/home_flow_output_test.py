"""Deterministic fragmented-output regressions; no processes or models needed."""
import unittest

from home_flow_test import TerminalText, hosted_turn_complete


class HouseholdOutputTest(unittest.TestCase):
    def test_metrics_prefix_does_not_finish_turn(self):
        out = TerminalText()
        out.feed(b"\x1b[2m  host prefill 0.1s \xc2\xb7 8 generated tokens")
        # This prefix satisfied the old wait and raced its following assert.
        self.assertIn("generated tokens", out.text)
        self.assertFalse(hosted_turn_complete(out.text))
        out.feed(" · decode 120.00 tok/s".encode())
        self.assertFalse(hosted_turn_complete(out.text))
        out.feed(" · hosted stream · no local checkpoint".encode())
        self.assertFalse(hosted_turn_complete(out.text))
        out.feed(b"\x1b[0m\r")
        self.assertFalse(hosted_turn_complete(out.text))
        out.feed(b"\n")
        self.assertTrue(hosted_turn_complete(out.text))

    def test_every_byte_boundary_including_utf8_and_ansi(self):
        line = "\x1b[2m  host prefill 0.1s · 8 generated tokens · decode 120.00 tok/s · hosted stream · no local checkpoint\x1b[0m\r\n"
        wire = line.encode()
        for split in range(len(wire)):
            with self.subTest(split=split):
                out = TerminalText()
                out.feed(wire[:split])
                self.assertFalse(hosted_turn_complete(out.text))
                out.feed(wire[split:])
                self.assertEqual(out.text, line)
                self.assertTrue(hosted_turn_complete(out.text))
        out = TerminalText()
        for i, byte in enumerate(wire):
            out.feed(bytes([byte]))
            self.assertEqual(hosted_turn_complete(out.text), i == len(wire) - 1)
        self.assertEqual(out.text, line)

    def test_missing_footer_stays_incomplete(self):
        for text in ("Speed: 12 tok/s\n", "8 generated tokens\n",
                     "Segment generation failed\n", "hosted stream\n",
                     " · hosted stream · no local checkpoint"):
            self.assertFalse(hosted_turn_complete(text))


if __name__ == "__main__":
    unittest.main()
