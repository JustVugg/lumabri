"""The ownership audit must reject both GNU and Apple unwrapped imports."""
import importlib.util
from pathlib import Path

path = Path(__file__).resolve().parents[2] / "tools/check_engine_memory.py"
spec = importlib.util.spec_from_file_location("boundary", path)
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
for owned, runtime in (
    ("lib.a:engine_memory.o:                 U malloc\n", "lib.a:olmoe.o:                 U lmb_engine_malloc\n"),
    ("lib.a(engine_memory.o): _malloc\n", "lib.a(olmoe.o): _lmb_engine_malloc\n"),
    ("lib.a[engine_memory.o]: U _malloc\n", "lib.a[olmoe.o]: U _lmb_engine_malloc\n"),
):
    assert not audit.violations(owned + runtime)
    for native in audit.NATIVE:
        bad = runtime.replace("lmb_engine_malloc", native)
        assert audit.violations(owned + bad), native
        assert audit.violations(owned + bad.replace(native, native + "$UNIX2003")), native
    assert audit.violations(owned)
assert audit.violations("")
print("ENGINE MEMORY AUDIT: PASS (GNU/Apple imports, missing ownership, native bypass)")
