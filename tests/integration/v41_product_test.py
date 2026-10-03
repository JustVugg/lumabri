"""Real V4.1 household binaries: sequential oracle, TCP split and approvals.

Uses private test households only. Requires the upstream-format tiny fixture;
does not fetch a checkpoint or contact any user's donor.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    repo = Path(__file__).resolve().parents[2]
    fixture = repo / "build/v41-tiny"
    for name in ("tracker", "segment_node", "segment_chat", "lumabri", "build/test_v41_range"):
        if not os.access(repo / name, os.X_OK):
            raise RuntimeError(f"missing executable: {name}")
    if not (fixture / "ref.json").is_file():
        raise RuntimeError("build the V4.1 tiny fixture first")
    env = dict(os.environ, OMP_NUM_THREADS="1")
    oracle = subprocess.run([str(repo / "build/test_v41_range"), str(fixture), "--tokens-json"],
                            env=env, check=True, capture_output=True, text=True, timeout=120)
    ids = json.loads(oracle.stdout)["token_ids"]
    assert len(ids) == 8 and all(type(i) is int and i >= 0 for i in ids)
    env.update(SPLIT_ENGINE="deepseek_v41", SPLIT_MODEL_DIR=str(fixture),
               SPLIT_MODEL="v41-tiny", SPLIT_CONTEXT="128", SPLIT_MAX_ROWS="1",
               SPLIT_ROUNDS="1", SPLIT_THREADS="2", SPLIT_TOKENS="8",
               SPLIT_EXPECT_IDS=",".join(map(str, ids)), PORT="18290")
    subprocess.run(["bash", "tests/integration/segment_split_test.sh"], cwd=repo,
                   env=env, check=True, timeout=240)
    with tempfile.TemporaryDirectory(prefix="lmb-v41-catalogue-") as tmp:
        (Path(tmp) / "v41-tiny").symlink_to(fixture, target_is_directory=True)
        subprocess.run([sys.executable, "tests/integration/home_flow_test.py", "--models-dir", tmp,
                        "--resident-default", "--expect-metrics", "--context", "128"],
                       cwd=repo, check=True, timeout=300)
    print("V41 PRODUCT PASS: sequential oracle, real TCP split, approved resident household chat")


if __name__ == "__main__":
    main()
