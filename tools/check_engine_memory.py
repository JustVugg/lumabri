#!/usr/bin/env python3
"""Reject unwrapped native allocation imports in the owned engine archive.

Compile-time namespacing, not libc interposition: only engine_memory.o may
import these entry points. This is a build boundary check, not a claim about
allocations inside external libraries, stacks, GPU drivers or kernel code.
"""
import argparse
import re
import subprocess

NATIVE = set("malloc calloc realloc reallocarray free cfree posix_memalign aligned_alloc "
             "memalign valloc pvalloc strdup strndup mmap mmap64 munmap mremap "
             "mlock munlock mlockall munlockall madvise posix_madvise "
             "vm_allocate vm_deallocate mach_vm_allocate mach_vm_deallocate".split())


def violations(output):
    violations = []
    members = set()
    for line in output.splitlines():
        # GNU nm: lib.a:object.o: U malloc; Apple nm: lib.a(object.o): _malloc.
        match = re.match(r".*[\[(:]([^\[\]():]+\.o)[\]):]:?\s+(?:U\s+)?(\S+)\s*$", line)
        if not match:
            symbol = line.split()[-1] if line.split() else ""
            if symbol.lstrip("_").split("@", 1)[0].split("$", 1)[0] in NATIVE:
                violations.append(f"unrecognized nm ownership: {line}")
            continue
        member, symbol = match.groups()
        members.add(member)
        symbol = symbol.lstrip("_").split("@", 1)[0].split("$", 1)[0]
        if member != "engine_memory.o" and symbol in NATIVE:
            violations.append(f"{member}: unwrapped {symbol}")
    if "engine_memory.o" not in members or len(members) < 2:
        violations.append("missing allocator or engine objects in archive inspection")
    return violations


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive")
    args = parser.parse_args()
    result = subprocess.run(["nm", "-A", "-u", args.archive], text=True, capture_output=True, check=True)
    problems = violations(result.stdout)
    if problems:
        parser.exit(1, "ENGINE MEMORY BOUNDARY: FAIL\n" + "\n".join(problems) + "\n")
    print("ENGINE MEMORY BOUNDARY: PASS (native allocator imports confined to engine_memory.o)")


if __name__ == "__main__":
    main()
