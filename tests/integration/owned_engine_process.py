"""Identify a fault-injection target inside this fixture's owned process tree.

home_spawn returns the engine PID on Linux, but the dedicated supervisor PID
on Darwin. Never mistake that supervisor for the engine, or signal a process
merely because its name looks familiar.
"""
from pathlib import Path
import subprocess
import sys


def select_engine_process(rows, owner, tracked, runtime, program, supervised):
    assert owner > 1 and tracked > 1 and owner != tracked
    by_pid = {pid: (parent, group, command) for pid, parent, group, command in rows}

    def matches(command, expected):
        # Linux ps normally reports the short comm; Darwin reports its path.
        return command == expected.name or Path(command).resolve() == expected.resolve()

    parent, group, command = by_pid[tracked]
    assert parent == owner and group == tracked, (tracked, parent, group, owner)
    if not supervised:
        assert matches(command, runtime / program), (tracked, command, program)
        return tracked
    assert matches(command, runtime / "lumabri"), (tracked, command)
    children = [(pid, group, command) for pid, parent, group, command in rows if parent == tracked]
    assert len(children) == 1, (tracked, children)
    pid, group, command = children[0]
    assert pid > 1 and group == tracked and matches(command, runtime / program), (pid, group, command)
    return pid


def owned_engine_pid(snapshot, field, runtime, program):
    output = subprocess.run(["ps", "-axo", "pid=,ppid=,pgid=,comm="],
                            text=True, capture_output=True, check=True).stdout
    rows = []
    for line in output.splitlines():
        pid, parent, group, command = line.split(None, 3)
        rows.append((int(pid), int(parent), int(group), command))
    return select_engine_process(rows, snapshot["pid"], snapshot[field], runtime, program,
                                 supervised=sys.platform == "darwin")
