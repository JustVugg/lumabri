#!/usr/bin/env python3
"""Compatibility hooks against the pinned, unmodified Colibri sources."""
import argparse
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--engine", type=Path, required=True)
args, unittest_args = parser.parse_known_args()
ENGINE = args.engine.resolve()
PATCHES = runpy.run_path(str(ROOT / "engine_patches/make_patches.py"))
ROUTING_CHECK = (
    '    if (!result && selected != topk)\n'
    '        result = moe_fail("layer %d: routing selected %d experts, wanted %d",\n'
    '                          weights->plan.layer, selected, topk);\n'
)
LEGACY_CHECK = '    if (!result && selected != topk) result = -1;\n'


class EnginePatchTests(unittest.TestCase):
    def test_qwen_preserves_optimized_paths_and_shared_tail(self):
        source = (ENGINE / "qwen36.c").read_text()
        patched = PATCHES["apply_hooks"](source, PATCHES["QWEN36"], "qwen36.c")
        self.assertIn('!use_xf && !use_qt && lumi_layer_on(layer)', patched)
        self.assertIn('} else\n#endif\n        if (use_xf)', patched)
        self.assertIn('} else if (use_qt)', patched)
        for line in (
            '    int use_xf = !use_qt && xf_mode(m);',
            '    if (use_xf) { moe_xf_run(m, layer, x, S, out, xidx, xval); free(xidx); free(xval); }',
            '    if (!use_qt) qwen_shared_experts_cpu(m,l,x,S,out,sh,shu,shd);',
        ):
            self.assertEqual(source.count(line), 1)
            self.assertEqual(patched.count(line), 1)
        self.assertEqual((ENGINE / "qwen36.c").read_text(), source)

    def test_qwen_comment_is_not_an_abi(self):
        source = (ENGINE / "qwen36.c").read_text().replace(
            '/* optional CUDA VRAM expert tier */', '/* changed documentation */')
        patched = PATCHES["apply_hooks"](source, PATCHES["QWEN36"], "qwen36.c")
        self.assertIn('/* changed documentation */', patched)
        with self.assertRaises(SystemExit):
            PATCHES["apply_hooks"](source.replace('        if (use_xf) {',
                '        if (new_runner) {'), PATCHES["QWEN36"], "qwen36.c")

    def patch_deepseek(self, source, succeeds=True):
        with tempfile.TemporaryDirectory(prefix="lmb-v4-hooks-") as directory:
            src, dst = Path(directory) / "source.c", Path(directory) / "patched.c"
            src.write_text(source)
            result = subprocess.run([sys.executable,
                str(ROOT / "engine_patches/deepseek_v4_p2p.py"), str(src), str(dst)],
                capture_output=True, text=True, timeout=10)
            self.assertEqual(src.read_text(), source)
            if succeeds:
                self.assertEqual(result.returncode, 0, result.stderr)
                return dst.read_text()
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(dst.exists(), "failed hooks must not publish a partial source")

    def test_deepseek_hook_precedes_loaders_and_preserves_diagnostic(self):
        source = (ENGINE / "deepseek_v4.c").read_text()
        self.assertEqual(source.count(ROUTING_CHECK), 1)
        patched = self.patch_deepseek(source)
        start = patched.index(ROUTING_CHECK)
        hook = patched.index('float *lumi_partial = NULL;', start)
        preload = patched.index('if (!result && expert_prefetch_enabled()', start)
        loader = patched.index('profiled_expert_load_start(', start)
        self.assertLess(hook, preload)
        self.assertLess(hook, loader)
        self.assertIn('malloc((size_t)(selected > 0 ? selected : 1) * sizeof(*views))', patched)
        # The previously reviewed routing layout is still supported.
        legacy = self.patch_deepseek(source.replace(ROUTING_CHECK, LEGACY_CHECK))
        self.assertIn(LEGACY_CHECK + '#ifdef LUMABRI_P2P', legacy)

    def test_deepseek_unknown_or_ambiguous_routing_fails_closed(self):
        source = (ENGINE / "deepseek_v4.c").read_text()
        for replacement in ('', ROUTING_CHECK * 2, ROUTING_CHECK + LEGACY_CHECK):
            with self.subTest(replacement=replacement):
                self.patch_deepseek(source.replace(ROUTING_CHECK, replacement), succeeds=False)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *unittest_args])
