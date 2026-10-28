"""The ownership audit must reject ELF, Mach-O and PE native bypasses."""
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
owned = "lib.a:engine_memory.o: U __imp_VirtualLock\n"
runtime = "lib.a:olmoe.o: U lmb_engine_malloc\n"
assert not audit.violations(owned + runtime)
for native in audit.NATIVE:
    for prefix in ("__imp_", "__imp__"):
        assert audit.violations(owned + runtime.replace("lmb_engine_malloc", prefix + native))
assert audit.violations("unrecognized: __imp_VirtualAlloc")
input_boundary = "lib.a:engine_io_windows.o: U __imp_ReadFile\n"
assert not audit.violations(owned+runtime+input_boundary,windows_input=True)
assert audit.violations(owned+runtime,windows_input=True)
for native in audit.INPUT:
    assert audit.violations(owned+input_boundary+runtime.replace(
        "lmb_engine_malloc","__imp_"+native),windows_input=True),native
assert audit.violations(owned+runtime+input_boundary+"unknown: __imp_ReadFile",windows_input=True)
print("ENGINE MEMORY AUDIT: PASS (ELF/Mach-O/PE imports, missing ownership, native bypass)")
