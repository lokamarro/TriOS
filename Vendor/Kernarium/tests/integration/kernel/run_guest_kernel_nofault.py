#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that a faulting nofault kernel read recovers instead of panicking.

copy_from_kernel_nofault() and the other nofault accessors resume a faulting
access at a recovery point that only the fault handler reaches. sysrq t dumps
every task, and its workqueue report for each idle kernel worker reads through
that worker's NULL pool-workqueue pointer with copy_from_kernel_nofault(). The
read must fail quietly: the guest keeps running and the host reports no fault
for it, since a recovered nofault read is expected kernel behaviour. A lost
recovery point panics the guest instead. The kernel has no BPF tracing, so
bpf_probe_read_kernel() cannot reach this path.

The same dump checks the stack walker: a task blocked in the scheduler is
walked from the context it saved, so the dump's Call Traces list entries. A
walk bounded by the wrong stack prints every Call Trace empty. The kernel
follows frame records, so no entry may be marked unreliable ("?"); a kernel
without frame pointers would guess every one. The log buffer is enlarged so
the dump cannot overwrite its own header. The contract is
kernel/common/README.md.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import re
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()

    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1G",
        "tmpfs",
        ("ncpus=2", "log_buf_len=1M"),
        init_shell=True,
        engine=arguments.engine,
    ) as guest:
        guest.boot_to_shell(180)
        # The dump goes to the kernel log only; the console would take minutes.
        dumped = guest.run_command(
            "echo 1 > /proc/sys/kernel/printk && echo t > /proc/sysrq-trigger && "
            "echo dumps=$(dmesg | grep -c 'sysrq: Show State') "
            "workers=$(dmesg | grep -c ' task:kworker/') "
            "traces=$(dmesg | grep -c 'Call Trace:') "
            "entries=$(dmesg | grep -c -E '\\] +\\[<[0-9a-f]+>\\] ') "
            "unreliable=$(dmesg | grep -c -E '\\] +\\[<[0-9a-f]+>\\] \\? ')",
            120,
        )
        found = re.search(
            r"dumps=(\d+) workers=(\d+) traces=(\d+) entries=(\d+) "
            r"unreliable=(\d+)", dumped.output)
        dumps, workers, traces, entries, unreliable = (
            tuple(int(found[index]) for index in range(1, 6))
            if found else (0, 0, 0, 0, 0))
        # The emulator's stderr shares the console, so a host fault report
        # made during the dump lands inside the command's output.
        reports = len(re.findall(r"^Darwin POSIX ", dumped.output, re.MULTILINE))
        print(f"sysrq t ran {dumps} time(s), listed {workers} kernel workers "
              f"with {entries} entries ({unreliable} unreliable) in {traces} "
              f"Call Traces "
              f"and printed {reports} host fault report line(s)")
        if dumped.return_code != 0 or dumps != 1 or workers < 1:
            raise RuntimeError("sysrq t did not dump the kernel workers:\n"
                               + dumped.output)
        if traces < 1 or entries < 1:
            raise RuntimeError("sysrq t printed only empty Call Traces:\n"
                               + dumped.output)
        if unreliable:
            raise RuntimeError("sysrq t marked Call Trace entries unreliable:\n"
                               + dumped.output)
        if reports:
            raise RuntimeError("the host reported recovered nofault reads:\n"
                               + dumped.output)
        guest.check_command("echo alive", 30, expected="alive")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
