"""Native Segment/Edge must explain insufficient locking BEFORE tensor loads.

Use the already-generated OLMoE fixture. No download, listener or model-sized
lock is needed; only the isolated test child gets a zero memlock allowance.
"""
import os
from pathlib import Path
import resource
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    if sys.platform != "linux":
        print("MEMLOCK PREFLIGHT: Linux admission not claimed on this kernel")
        return
    caps = next(line.split()[1] for line in Path("/proc/self/status").read_text().splitlines()
                if line.startswith("CapEff:"))
    if int(caps, 16) & (1 << 14):
        raise RuntimeError("Run the refusal test without CAP_IPC_LOCK")
    model = Path(sys.argv[1]).resolve()
    allowance = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    if allowance < 0:
        raise ValueError("negative memlock allowance")
    if not (model / "config.json").is_file():
        raise RuntimeError("A real reduced OLMoE checkpoint is required")
    with tempfile.TemporaryDirectory(prefix="lmb-memlock-") as tmp:
        env = {**os.environ, "HOME": tmp, "LUMABRI_PEER_KEY": tmp + "/peer.key",
               "LUMABRI_KNOWN_HOSTS": tmp + "/known.hosts", "LUMABRI_ENCRYPT": "1",
               "LUMABRI_RESIDENCY_POLICY": "engine-locked", "LUMABRI_RESIDENT_REQUIRED": "1",
               "LUMABRI_ENGINE_BACKEND": "cpu", "OMP_NUM_THREADS": "1",
               "LUMABRI_SEGMENT_RAM_RESERVE_MB": "256"}
        for key in ("LUMABRI_READY_FD", "LUMABRI_NODE_CONTROL_FD",
                    "LUMABRI_SEGMENT_CLIENT_PK", "LUMABRI_COMPUTE_SOCKET"):
            env.pop(key, None)
        common = ["--engine", "olmoe", "--model-dir", str(model), "--model", "memlock-only",
                  "--tracker", "127.0.0.1:1", "--model-root", "01" * 32,
                  "--tokenizer-root", "02" * 32, "--context", "64", "--threads", "1"]
        for program, extra in (
            ("segment_node", ["--range", "0:1", "--port", "47302", "--advertise",
                              "127.0.0.1:47302", "--name", "memlock-only", "--memory-limit-mb",
                              "2048" if allowance else "512"]),
            ("segment_chat", ["--tokens", "1", "--prompt-ids", "1"]),
        ):
            reader, writer = os.pipe()
            try:
                args = [str(ROOT / program), *common, *extra]
                if program == "segment_node":
                    args += ["--preflight-fd", str(writer)]
                result = subprocess.run(args, env=env, text=True, capture_output=True,
                                        timeout=15, pass_fds=(writer,),
                                        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_MEMLOCK, (allowance, allowance)))
                os.close(writer)
                writer = -1
                signal = os.read(reader, 16)
            finally:
                os.close(reader)
                if writer >= 0:
                    os.close(writer)
            assert result.returncode == 78, (program, result.returncode, result.stderr)
            assert f"RLIMIT_MEMLOCK) is {allowance} bytes" in result.stderr, result.stderr
            assert "Refusing before loading weights" in result.stderr, result.stderr
            if allowance:
                assert "CPU weight payload" in result.stderr, result.stderr
            assert "[resident] layer" not in result.stderr, result.stderr
            assert "malloc " not in result.stderr, result.stderr
            if program == "segment_node":
                assert signal == b"F", signal
        print("MEMLOCK PREFLIGHT: PASS (native Segment + Edge, explicit denial before tensor loading)")


if __name__ == "__main__":
    main()
