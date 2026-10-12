"""Deterministic fragmented-output regressions; no processes or models needed."""
import unittest

from home_flow_test import TerminalText, hosted_turn_complete, approval_frame_ready


class HouseholdOutputTest(unittest.TestCase):
    def test_approval_waits_for_complete_current_offer(self):
        frame = ("\x1b[H\x1b[1;1HWaiting for your approval"
                 "\x1b[11;1HModel: model-b (olmoe)"
                 "\x1b[24;1HAccept this request\x1b[27;1HDecline"
                 "\x1b[30;1HAllow repeat loads for 1 hour"
                 "\x1b[39;1H↑ ↓ choose   Enter confirm\x1b[0m").encode()
        old_frame = frame.replace(b"model-b", b"model-a")
        for split in range(len(frame)):
            with self.subTest(split=split):
                out = TerminalText()
                out.feed(old_frame)
                self.assertFalse(approval_frame_ready(out, "model-b", repeat=True))
                out.feed(frame[:split])
                self.assertFalse(approval_frame_ready(out, "model-b", repeat=True))
                out.feed(frame[split:])
                self.assertTrue(approval_frame_ready(out, "model-b", repeat=True))
                self.assertFalse(approval_frame_ready(out, "model-a", repeat=True))

    def test_repeat_permission_must_be_visible(self):
        out = TerminalText()
        out.feed(b"\x1b[HWaiting for your approval Model: model-a (olmoe) "
                 b"Accept this request Decline Enter confirm\x1b[0m")
        self.assertTrue(approval_frame_ready(out, "model-a"))
        self.assertFalse(approval_frame_ready(out, "model-a", repeat=True))

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

    def test_darwin_translated_line_endings(self):
        prefix = "host prefill 0.3s · 8 generated tokens · decode 389.52 tok/s · first text 1.1s · hosted stream · no local checkpoint\x1b[0m"
        for ending in ("\n", "\r\n", "\r\r\n"):
            out = TerminalText()
            out.feed(prefix.encode())
            for byte in ending.encode()[:-1]:
                out.feed(bytes([byte]))
                self.assertFalse(hosted_turn_complete(out.text))
            out.feed(b"\n")
            self.assertTrue(hosted_turn_complete(out.text))


if __name__ == "__main__":
    unittest.main()
