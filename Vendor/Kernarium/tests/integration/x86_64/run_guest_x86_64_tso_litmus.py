#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that x86-64 and i386 guests keep x86's memory order across vCPUs.

Cross-builds the freestanding tso_litmus_probe.cpp for x86-64 and i386, boots
an x86-64 image with four vCPUs on an emulator built with the macos-x86_64
preset, and runs message passing (also with the variables in a thread's own
stack frame), store buffering with and without MFENCE, and IRIW, between the
threads of one process and between processes that share SysV shm, a memfd
mapped MAP_SHARED or inherited shared anonymous memory. No test may see an
outcome x86 forbids.

The cycles run every test in two shared phases with unshared ones between
and after them: tasks on the probe's address space, created and joined
for each test (thread-cycle), and SysV shm attached and detached
(shm-cycle) or shared anonymous memory mapped and unmapped (anon-cycle)
around the processes of a phase. The host's UML_X86_MM_SHARED_LOG must
show the probe's address space marked shared from the system call that
starts each shared interval to the one that ends it, and unshared outside
them, every agent process marked shared, and on the JIT the same address
spaces running ordered code exactly while marked and unordered code
otherwise.

The negative control, which shows that message passing catches a space
that keeps no order, is the unit test x86-64-jit-store-order: it drives the
JIT engine directly, so the product carries no setting that relaxes a
shared address space.
"""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import sys
import tempfile


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
# python3 -I leaves the script's own directory off sys.path.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from guest import Guest, GuestFile  # noqa: E402
from mm_shared_log import (  # noqa: E402
    MARK, ORDERING, UNORDERED_SHARED, Entry, changes, history, logged)
from probe_runner import (  # noqa: E402
    GUEST_DIRECTORY, I386, X86_64, add_arguments, build_probes, check_files)


PROBE = pathlib.Path(__file__).with_name("tso_litmus_probe.cpp")
PROBES = {
    "tso-litmus-probe": ("x86-64", X86_64),
    "tso-litmus-probe32": ("i386", I386),
}
THREAD_TESTS = ("mp", "mp-stack-load", "mp-stack-store", "sb", "sb-mfence", "iriw")
PROCESS_TESTS = ("mp", "sb", "sb-mfence", "iriw")
TESTS = {
    "thread": THREAD_TESTS,
    "shm": PROCESS_TESTS,
    "memfd": PROCESS_TESTS,
    "anon": PROCESS_TESTS,
    "thread-cycle": THREAD_TESTS,
    "shm-cycle": PROCESS_TESTS,
    "anon-cycle": PROCESS_TESTS,
}
CASES = tuple(TESTS)
# The shared phases of a cycle, and for each test the agents besides the
# leader.
PHASES = 2
OTHER_AGENTS = {"iriw": 3}
RESULT = re.compile(
    r"tso-litmus: abi=(\S+) case=(\S+)(?: phase=(\d+))? test=(\S+) "
    r"rounds=(\d+) (forbidden|allowed)=(\d+)"
)
RANGE = re.compile(
    r"tso-litmus: abi=(\S+) range=(0x[0-9a-f]+)-(0x[0-9a-f]+) "
    r"clone=(0x[0-9a-f]+) share=(0x[0-9a-f]+) unshare=(0x[0-9a-f]+)"
)
CPUS = 4


def check_cycle(case: str, hits: list[Entry], engine: str, start: int,
                unshare: int) -> list[str]:
    """The problems of one cycle's log, in the probe's code: its address space
    must be marked shared exactly from each system call that starts a shared
    interval (returning to `start`) to the one that ends it (returning to
    `unshare`), and each agent process's from its first dispatch to its
    exit; on the JIT, the same spaces must run ordered code exactly then."""
    tests = TESTS[case]
    threads = case == "thread-cycle"
    intervals = PHASES * (len(tests) if threads else 1)
    expected = [(1, start), (0, unshare)] * intervals
    problems = []
    kinds = [(MARK, "marked shared")]
    if engine == "jit":
        kinds.append((ORDERING, "ran ordered code"))
    marks = changes(hits, MARK)
    leaders = [generation for generation, changed in marks.items()
               if any(entry.rip == start for entry in changed)]
    if len(leaders) != 1:
        return [f"{len(leaders)} address spaces changed their mark at {start:#x}"]
    leader = leaders[0]
    for event, name in kinds:
        by_generation = changes(hits, event)
        changed = by_generation.get(leader, [])
        print(f"  {event} generation={leader}: {history(changed)}")
        states = [(entry.state, entry.rip) for entry in changed]
        if event == ORDERING:
            # The JIT also logs a space's first dispatch, which runs
            # unordered code before the first interval starts.
            if not states or states[0][0] != 0:
                problems.append("the probe's first dispatch did not run unordered code")
            states = states[1:]
        if states != expected:
            problems.append(f"the probe's address space {name} other than from "
                            f"each {start:#x} to the next {unshare:#x}")
        if threads:
            continue
        agents = [generation for generation in by_generation if generation != leader]
        count = PHASES * sum(OTHER_AGENTS.get(test, 1) for test in tests)
        if len(agents) != count:
            problems.append(f"{len(agents)} agent processes logged {event}, expected {count}")
        for generation in agents:
            if [entry.state for entry in by_generation[generation]] != [1]:
                problems.append(f"agent generation {generation} {name} only in part: "
                                f"{history(by_generation[generation])}")
    if threads and len(marks) != 1:
        problems.append(f"{len(marks) - 1} other address spaces changed their mark")
    return problems


def run_cases(
    arguments: argparse.Namespace,
    payloads: dict[str, GuestFile],
    cases: tuple[str, ...],
    rounds: int,
    log: pathlib.Path,
) -> tuple[dict[tuple[str, str, int, str], int], list[str]]:
    """Runs every case under both ABIs in one boot; returns the count each
    (abi, case, phase, test) printed, after checking that each printed once,
    and the problems of the cycles' logs. A case without phases has phase 0."""
    counts: dict[tuple[str, str, int, str], int] = {}
    problems = []
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "1G",
        "tmpfs",
        (f"ncpus={CPUS}",),
        init_shell=True,
        engine=arguments.engine,
        payloads=payloads,
    ) as guest:
        guest.boot_to_shell(arguments.timeout)
        for name, (abi, _) in PROBES.items():
            for case in cases:
                before = len(logged(log))
                result = guest.run_command(
                    f"{GUEST_DIRECTORY}/{name} {case} {rounds}",
                    arguments.probe_timeout,
                )
                output = result.output.strip()
                print(output, flush=True)
                header = RANGE.search(output)
                if result.return_code not in (0, 1) or header is None:
                    raise RuntimeError(f"{abi} {case}: the probe failed: {output!r}")
                for match in RESULT.finditer(output):
                    key = (match.group(1), match.group(2), int(match.group(3) or 0),
                           match.group(4))
                    if key in counts or int(match.group(5)) != rounds:
                        raise RuntimeError(f"unexpected result line: {match.group(0)}")
                    counts[key] = int(match.group(7))
                phases = range(1, PHASES + 1) if case.endswith("-cycle") else (0,)
                expected = {(abi, case, phase, test)
                            for phase in phases for test in TESTS[case]}
                missing = expected - counts.keys()
                if missing:
                    raise RuntimeError(f"{abi} {case}: no result for {sorted(missing)}")
                if not case.endswith("-cycle"):
                    continue
                low, high, clone, share, unshare = (int(header.group(index), 16)
                                                    for index in range(2, 7))
                hits = [entry for entry in logged(log)[before:] if low <= entry.rip < high]
                start = clone if case == "thread-cycle" else share
                for problem in check_cycle(case, hits, arguments.engine, start, unshare):
                    problems.append(f"{abi} {case}: {problem}")
    problems += [f"generation {entry.generation} ran unordered code at {entry.rip:#x} "
                 "while marked shared"
                 for entry in logged(log) if entry.event == UNORDERED_SHARED]
    return counts, problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_arguments(parser)
    parser.add_argument("--rounds", type=int, default=1_000_000,
                        help="rounds of each test in each case and ABI")
    parser.add_argument("--timeout", type=int, default=180, help="boot timeout")
    parser.add_argument("--probe-timeout", type=int, default=900,
                        help="timeout of one case under one ABI")
    arguments = parser.parse_args()

    check_files(parser, arguments)
    if min(arguments.rounds, arguments.timeout, arguments.probe_timeout) <= 0:
        parser.error("rounds and timeouts must be positive")

    payloads = build_probes(arguments.compiler, PROBE, PROBES)
    failures = 0
    with tempfile.TemporaryDirectory(prefix="uml-tso-litmus-log-") as directory:
        log = pathlib.Path(directory) / "mm-shared.log"
        os.environ["UML_X86_MM_SHARED_LOG"] = str(log)
        counts, problems = run_cases(arguments, payloads, CASES, arguments.rounds, log)
    for (abi, case, phase, test), count in sorted(counts.items()):
        if test == "sb":
            continue
        passed = count == 0
        failures += not passed
        where = f" phase {phase}" if phase else ""
        print(f"{'PASS' if passed else 'FAIL'} {abi} {case}{where} {test}: "
              f"{count} forbidden outcome(s) in {arguments.rounds} rounds")
    for problem in problems:
        print(f"FAIL {problem}")
    failures += len(problems)

    if failures:
        raise RuntimeError(f"{failures} check(s) failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
