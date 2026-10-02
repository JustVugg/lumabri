"""Only fixture IDs may differ from the unmodified upstream Edge oracle."""
import json
from pathlib import Path
import tempfile
import unittest

from prepare_qwen_edge_oracle import CASES, fixture_oracle, fixture_tokenizer


class QwenFixtureTest(unittest.TestCase):
    def test_token_ids_fit_model_and_live_only_in_added_tokens(self):
        original = {"model": {"vocab": {f"<|edge_fixture_{i}|>": i
                                        for i in range(320)}, "merges": []},
                    "added_tokens": []}
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "tokenizer.json"
            path.write_text(json.dumps(original))
            prepared = json.loads(fixture_tokenizer(path))
            self.assertEqual(json.loads(path.read_text()), original)
            self.assertEqual(len(prepared["model"]["vocab"]), 314)
            self.assertEqual([v["id"] for v in prepared["added_tokens"]], list(range(256, 262)))
            self.assertEqual([v["content"] for v in prepared["added_tokens"]], [v[0] for v in CASES])
            self.assertTrue(all(not v["special"] for v in prepared["added_tokens"]))
            path.write_text(json.dumps(prepared))
            with self.assertRaises(ValueError):
                fixture_tokenizer(path)

    def test_oracle_assertions_are_not_removed(self):
        source = '\n'.join('{"%s", %d}' % case for case in CASES)
        source += '\nREQUIRE(encoded_count == 1);\nREQUIRE(greedy == expected);\n'
        result = fixture_oracle(source)
        restored = result
        for index, (piece, production_id) in enumerate(CASES, 256):
            restored = restored.replace('{"%s", %d}' % (piece, index),
                                        '{"%s", %d}' % (piece, production_id))
        self.assertEqual(restored, source)
        with self.assertRaises(ValueError):
            fixture_oracle(source + '\n' + source)
        with self.assertRaises(ValueError):
            fixture_oracle(source.replace('<think>', '<unknown>'))


if __name__ == "__main__":
    unittest.main()
