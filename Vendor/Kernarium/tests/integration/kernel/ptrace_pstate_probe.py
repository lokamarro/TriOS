#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guest probe: a tracer's PSTATE writes are checked like native arm64's.

It stops a child, writes PSTATE through PTRACE_SETREGSET and PTRACE_POKEUSER,
and reads it back. PSTATE.SS and the RES0 bits must be cleared, the TCO, DIT
and SSBS bits kept, and a mode or DAIF bit refused with EINVAL. A continued
child whose SS write was accepted must run to its exit, not stop on a
single-step trap after each instruction. Prints one line per check and exits
with the number of failures.
"""

import ctypes
import errno
import os
import signal
import sys

PTRACE_TRACEME = 0
PTRACE_POKEUSER = 6
PTRACE_CONT = 7
PTRACE_GETREGSET = 0x4204
PTRACE_SETREGSET = 0x4205
NT_PRSTATUS = 1
REGISTERS = 34  # x0-x30, sp, pc, pstate
PSTATE = 33

SS = 1 << 21
PAN = 1 << 22
IL = 1 << 20
TCO = 1 << 25
DIT = 1 << 24
SSBS = 1 << 12
MODE_EL1H = 0x5
IRQ_MASK = 1 << 7

libc = ctypes.CDLL(None, use_errno=True)
libc.ptrace.argtypes = (ctypes.c_long, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)
libc.ptrace.restype = ctypes.c_long


class IoVec(ctypes.Structure):
    _fields_ = (("base", ctypes.c_void_p), ("length", ctypes.c_size_t))


def ptrace(request, pid, address, data):
    ctypes.set_errno(0)
    result = libc.ptrace(request, pid, address, data)
    return 0 if result >= 0 else ctypes.get_errno()


def regset(request, pid, registers):
    vector = IoVec(ctypes.cast(registers, ctypes.c_void_p), ctypes.sizeof(registers))
    return ptrace(request, pid, ctypes.c_void_p(NT_PRSTATUS), ctypes.byref(vector))


def main():
    if os.uname().machine != "aarch64":
        print("SKIP ptrace-pstate: not an arm64 guest")
        return 0
    child = os.fork()
    if child == 0:
        ptrace(PTRACE_TRACEME, 0, None, None)
        os.kill(os.getpid(), signal.SIGSTOP)
        os._exit(7)
    failures = 0

    def check(name, passed, detail=""):
        nonlocal failures
        print(f"{'PASS' if passed else 'FAIL'} {name}{': ' + detail if detail else ''}", flush=True)
        failures += 0 if passed else 1

    _, status = os.waitpid(child, 0)
    check("child stopped", os.WIFSTOPPED(status))
    registers = (ctypes.c_uint64 * REGISTERS)()
    check("getregset", regset(PTRACE_GETREGSET, child, registers) == 0)
    original = registers[PSTATE]

    registers[PSTATE] = original | SS | PAN | IL | TCO | DIT | SSBS
    result = regset(PTRACE_SETREGSET, child, registers)
    check("setregset keeps a valid pstate", result == 0, errno.errorcode.get(result, str(result)))
    regset(PTRACE_GETREGSET, child, registers)
    check("SS and RES0 cleared", registers[PSTATE] & (SS | PAN | IL) == 0, hex(registers[PSTATE]))
    check("TCO, DIT and SSBS kept",
          registers[PSTATE] & (TCO | DIT | SSBS) == TCO | DIT | SSBS, hex(registers[PSTATE]))

    for name, bit in (("mode", MODE_EL1H), ("DAIF", IRQ_MASK)):
        registers[PSTATE] = original | bit
        result = regset(PTRACE_SETREGSET, child, registers)
        check(f"setregset refuses a {name} bit", result == errno.EINVAL,
              errno.errorcode.get(result, str(result)))
        result = ptrace(PTRACE_POKEUSER, child, ctypes.c_void_p(PSTATE * 8),
                        ctypes.c_void_p(original | bit))
        check(f"pokeuser refuses a {name} bit", result == errno.EINVAL,
              errno.errorcode.get(result, str(result)))
    regset(PTRACE_GETREGSET, child, registers)
    check("refused writes left pstate alone",
          registers[PSTATE] & (MODE_EL1H | IRQ_MASK) == 0, hex(registers[PSTATE]))

    registers[PSTATE] = original | SS
    regset(PTRACE_SETREGSET, child, registers)
    ptrace(PTRACE_CONT, child, None, None)
    _, status = os.waitpid(child, 0)
    stepped = os.WIFSTOPPED(status) and os.WSTOPSIG(status) == signal.SIGTRAP
    check("continued child runs to its exit",
          os.WIFEXITED(status) and os.WEXITSTATUS(status) == 7,
          "stopped by a single-step trap" if stepped else f"status {status:#x}")
    if not os.WIFEXITED(status):
        os.kill(child, signal.SIGKILL)
        os.waitpid(child, 0)
    return failures


if __name__ == "__main__":
    sys.exit(main())
