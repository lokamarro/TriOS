#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the "mm is shared" dispatch flag of the x86_64-userspace slice.

The kernel marks an entry of a task shared while another agent may observe
its address space: while another task may run on it (a vfork child does not
count its waiting parent), while it maps memory another address space may
map too (MAP_SHARED of any kind, SysV shm), or while io_uring keeps a ring in
its memory. The host reads the mark from each dispatch
(DARWIN_UML_DISPATCH_MM_SHARED), and the x86-64 JIT keeps x86's store order
for a marked dispatch only. Cross-builds the freestanding mm_shared_probe.cpp,
boots an x86-64 image on an emulator built with the macos-x86_64 preset with
UML_X86_MM_SHARED_LOG set, and runs the probe in each mode. The host logs
each change of an address space's mark with the RIP of the dispatch that saw
it; in the probe's code each mode must mark the address spaces it shares and
no other, and a mode that ends its sharing must see the mark go again. Where
the probe starts or ends its sharing with one system call, the change must
come at that call's return address.

On the JIT, the log also records each change of an address space's
ordering: an unshared probe must run only unordered code, a shared one must
run ordered code and, once its sharing ends, unordered code again, and no
dispatch the kernel marked shared may run unordered code.
"""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import os
import pathlib
import re
import sys
import tempfile
import typing


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
# python3 -I leaves the script's own directory off sys.path.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from guest import Guest  # noqa: E402
from mm_shared_log import (  # noqa: E402
    MARK, ORDERING, UNORDERED_SHARED, Entry, changes, history, logged)
from probe_runner import (  # noqa: E402
    GUEST_DIRECTORY, X86_64, build_probes, parse_arguments)


PROBE = pathlib.Path(__file__).with_name("mm_shared_probe.cpp")
# Unlike the other probes, this one is built without -mno-red-zone.
PROBES = {"mm-shared-probe": ("x86-64", X86_64[:1])}
GUEST_PROBE = f"{GUEST_DIRECTORY}/mm-shared-probe"
# Each mode: the number of address spaces it marks shared (the probe's own,
# and for fork-shared its child's as well); whether the probe's sharing must
# end again; and whether its start and its end come at the return of the
# probe's sys_share() and sys_unshare() calls.
class Expect(typing.NamedTuple):
    shared: int
    ends: bool = False
    exact_share: bool = False
    exact_unshare: bool = False


MODES = {
    "single": Expect(0),
    "fork": Expect(0),
    "vfork": Expect(0),
    "private-file": Expect(0),
    "thread": Expect(1),
    "shared-anon": Expect(1),
    "shared-ro": Expect(1),
    "shm": Expect(1),
    "uring-nommap": Expect(1, exact_share=True),
    "fork-shared": Expect(2),
    "thread-fork": Expect(1),
    "thread-join": Expect(1, ends=True, exact_unshare=True),
    "unmap": Expect(1, True, True, True),
    "shmdt": Expect(1, True, True, True),
    "split": Expect(1, True, True, True),
    "mremap": Expect(1, True, True, True),
    "remap-private": Expect(1, True, True, True),
    "fork-dontcopy": Expect(1, True, True, True),
    # The rings are unpinned from a work item after close().
    "uring-close": Expect(1, ends=True, exact_share=True),
}
RANGE = re.compile(
    r"mm-shared-probe: mode=([\w-]+) range=(0x[0-9a-f]+)-(0x[0-9a-f]+) "
    r"share=(0x[0-9a-f]+) unshare=(0x[0-9a-f]+)"
)
PROBE_TIMEOUT_SECONDS = 60


def check_ending(expect: Expect, changed: list[Entry], share: int,
                 unshare: int, name: str) -> str | None:
    """Whether a shared generation's changes of `name` (1 shared or
    ordered, 0 not) start and, where they must, end its sharing as the mode
    says."""
    states = [entry.state for entry in changed]
    if 1 not in states:
        return f"the probe's address space never {name}"
    first = states.index(1)
    if expect.exact_share and changed[first].rip != share:
        return (f"the address space {name} first at {changed[first].rip:#x}, "
                f"not at the share call's return {share:#x}")
    if not expect.ends:
        return None
    if 0 not in states[first:]:
        return f"the address space {name} until the probe ended"
    if expect.exact_unshare:
        if states[first:] != [1, 0] or changed[first + 1].rip != unshare:
            return (f"the address space did not stop being {name} exactly at "
                    f"the unshare call's return {unshare:#x}: {history(changed)}")
    return None


def check_mode(expect: Expect, hits: list[Entry], engine: str, share: int,
               unshare: int) -> str | None:
    """One mode's verdict, or None when it passes."""
    marks = changes(hits, MARK)
    marked = [generation for generation, history in marks.items()
              if any(entry.state == 1 for entry in history)]
    if len(marked) != expect.shared:
        return (f"{len(marked)} shared address space(s) in the probe, "
                f"expected {expect.shared}")
    if expect.shared == 1:
        problem = check_ending(expect, marks[marked[0]], share, unshare,
                               "was marked shared")
        if problem is not None:
            return problem
    if engine != "jit":
        return None
    orders = changes(hits, ORDERING)
    if not orders:
        return "the JIT logged no dispatch in the probe"
    ordered = [generation for generation, history in orders.items()
               if any(entry.state == 1 for entry in history)]
    if sorted(ordered) != sorted(marked):
        return (f"address spaces {sorted(ordered)} ran ordered code, but "
                f"{sorted(marked)} were marked shared")
    if expect.shared == 1:
        return check_ending(expect, orders[marked[0]], share, unshare,
                            "ran ordered code")
    return None


def main() -> int:
    arguments = parse_arguments(
        __doc__, compiler_help="clang driver with an x86-64 Linux target and ld.lld")

    payloads = build_probes(arguments.compiler, PROBE, PROBES)
    failures = 0
    with tempfile.TemporaryDirectory(prefix="uml-mm-shared-") as directory:
        log = pathlib.Path(directory) / "mm-shared.log"
        os.environ["UML_X86_MM_SHARED_LOG"] = str(log)
        with Guest(
            arguments.emulator,
            arguments.rootfs,
            "512M",
            "tmpfs",
            kernel_arguments=("ncpus=10",),
            init_shell=True,
            engine=arguments.engine,
            payloads=payloads,
        ) as guest:
            guest.boot_to_shell(arguments.timeout)
            for mode, expect in MODES.items():
                before = len(logged(log))
                result = guest.run_command(f"{GUEST_PROBE} {mode}",
                                           PROBE_TIMEOUT_SECONDS)
                output = result.output.strip()
                print(output)
                match = RANGE.search(output)
                if result.return_code != 0 or match is None or match.group(1) != mode:
                    raise RuntimeError(f"{mode}: the probe failed: {output!r}")
                start, end, share, unshare = (int(match.group(index), 16)
                                              for index in range(2, 6))
                hits = [entry for entry in logged(log)[before:]
                        if start <= entry.rip < end]
                problem = check_mode(expect, hits, arguments.engine, share, unshare)
                failures += problem is not None
                ending = ", ended again" if expect.ends else ""
                for event, by_generation in ((MARK, changes(hits, MARK)),
                                             (ORDERING, changes(hits, ORDERING))):
                    for generation, changed in by_generation.items():
                        print(f"  {event} generation={generation}: {history(changed)}")
                print(f"FAIL {mode}: {problem}" if problem else
                      f"PASS {mode}: {expect.shared} shared address space(s){ending}")
        unordered = [entry for entry in logged(log)
                     if entry.event == UNORDERED_SHARED]
        for entry in unordered:
            print(f"FAIL generation {entry.generation} ran unordered code at "
                  f"{entry.rip:#x} while marked shared")
        failures += len(unordered)
    if failures:
        raise RuntimeError(f"{failures} mode(s) failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
