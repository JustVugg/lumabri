#!/usr/bin/env python3
import unittest
from production_check import check


def result(rate, peers=3, calls=None):
    return {
        "tokens_match_oracle": True,
        "tok_s_single": rate,
        "tok_s_aggregate": {"1": rate, "2": rate * 1.8,
                            "4": rate * 3.0, "8": rate * 4.0},
        "peers": peers,
        "expert_calls_per_peer": calls or {"donor-a": 10},
    }


class ProductionCheckTest(unittest.TestCase):
    def test_accepts_two_x_and_active_donor(self):
        got = check(result(1), result(1.2), result(2.5))
        self.assertGreater(got["speedup"], 2)

    def test_rejects_speed_or_oracle_or_idle_donors(self):
        with self.assertRaisesRegex(ValueError, "ratio"):
            check(result(1), result(1), result(1.9))
        broken = result(3)
        broken["tokens_match_oracle"] = False
        with self.assertRaisesRegex(ValueError, "oracle"):
            check(result(1), result(1), broken)
        with self.assertRaisesRegex(ValueError, "no active"):
            check(result(1), result(1), result(3, calls={"donor": 0}))


if __name__ == "__main__":
    unittest.main()
