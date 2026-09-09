"""Native loader proof: the tested helper execs a real separately built binary."""
import os
from pathlib import Path
import platform
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = ROOT / "tests/c/preload_path_fixture.c"
cc = shlex.split(os.environ.get("CC", "cc"))
with tempfile.TemporaryDirectory(prefix="lumabri-preload-test-") as temporary:
    root = Path(temporary)
    folder = root / ("install with spaces:and colon" if platform.system() == "Linux" else "install with spaces")
    folder.mkdir()
    library = folder / ("libtest.dylib" if platform.system() == "Darwin" else "libtest.so")
    launcher, probe = root / "launcher", folder / "probe"
    common = cc + ["-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT), str(source)]
    subprocess.run(common + ["-DTEST_PRELOAD_LIBRARY", "-dynamiclib" if platform.system() == "Darwin" else "-shared",
                             "-fPIC", "-o", str(library)], check=True)
    subprocess.run(common + ["-DTEST_PRELOAD_LAUNCHER", "-o", str(launcher)], check=True)
    subprocess.run(common + ["-o", str(probe)], check=True)
    environment = {k: v for k, v in os.environ.items()
                   if k not in ("LD_PRELOAD", "DYLD_INSERT_LIBRARIES", "LMB_PRELOAD_PATH_TEST")}
    assert subprocess.run([str(probe)], env=environment).returncode != 0, "probe must require the actual constructor"
    if platform.system() == "Linux":
        old = subprocess.run([str(probe)], env={**environment, "LD_PRELOAD": str(library)},
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert old.returncode != 0, "regression fixture must expose the old unescaped preload failure"
    subprocess.run([str(launcher), str(library), str(probe)], check=True, env=environment)
    assert subprocess.run([str(launcher), str(folder / "missing library"), str(probe)], env=environment).returncode != 0
print("PRELOAD PATH: PASS (native constructor loaded through a spaced install path)")
