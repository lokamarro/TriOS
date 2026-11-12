#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guest probe: a signal taken between two instructions keeps x0.

Only a system call's own return may be restarted or turned into EINTR. The
probe spins in machine code with x0 holding each -ERESTART* code an
interrupted system call returns (-512, -513, -514 and -516) while a 1 ms
interval timer's SIGALRM interrupts it, with SA_RESTART and without, and
checks that x0 comes back unchanged. Prints one line per check and exits with
the number of failures. The contract is kernel/common/README.md.
"""

import ctypes
import mmap
import os
import signal
import sys
import tempfile

# nop; subs x1, x1, #1; b.ne .-4; ret: spins x1 times and returns x0
# untouched. A wrongly restarted system call moves the pc back one
# instruction, which the leading nop keeps inside the mapping.
CODE = bytes.fromhex("1f2003d5" "210400f1" "e1ffff54" "c0035fd6")
LOOPS = 1 << 22
ROUNDS = 2000
WANTED_SIGNALS = 3
CODES = (512, 513, 514, 516)
MASK = (1 << 64) - 1

libc = ctypes.CDLL(None, use_errno=True)
libc.mmap.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, ctypes.c_long)
libc.mmap.restype = ctypes.c_void_p


def spin_function():
    """Maps CODE executable from a file, so the kernel keeps the instruction
    cache coherent, and returns it as (value, loops) -> x0."""
    with tempfile.TemporaryFile(dir="/tmp") as code_file:
        code_file.write(CODE)
        code_file.flush()
        address = libc.mmap(None, mmap.PAGESIZE, mmap.PROT_READ | mmap.PROT_EXEC,
                            mmap.MAP_PRIVATE, code_file.fileno(), 0)
    if address in (None, ctypes.c_void_p(-1).value):
        raise OSError(ctypes.get_errno(), "mmap of the spin code failed")
    prototype = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64)
    return prototype(address)


def main():
    if os.uname().machine != "aarch64":
        print("SKIP signal-restart: not an arm64 guest")
        return 0
    spin = spin_function()
    handled = 0

    def on_alarm(_signal, _frame):
        nonlocal handled
        handled += 1

    signal.signal(signal.SIGALRM, on_alarm)
    failures = 0
    for restart in (False, True):
        # siginterrupt(False) sets SA_RESTART.
        signal.siginterrupt(signal.SIGALRM, not restart)
        for code in CODES:
            value = -code & MASK
            held = value
            handled = 0
            signal.setitimer(signal.ITIMER_REAL, 0.001, 0.001)
            for _ in range(ROUNDS):
                held = spin(value, LOOPS)
                if held != value or handled >= WANTED_SIGNALS:
                    break
            signal.setitimer(signal.ITIMER_REAL, 0, 0)
            name = f"{'SA_RESTART' if restart else 'no SA_RESTART'}, x0 -{code}"
            passed = held == value and handled >= WANTED_SIGNALS
            detail = f"x0 {held:#x}, {handled} rounds with SIGALRM"
            print(f"{'PASS' if passed else 'FAIL'} {name}: {detail}", flush=True)
            failures += 0 if passed else 1
    return failures


if __name__ == "__main__":
    sys.exit(main())
