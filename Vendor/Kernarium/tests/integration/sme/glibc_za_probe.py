#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The SME ABI of the image's glibc and libgcc.

With HWCAP2_SME, glibc 2.41 calls __libc_arm_za_disable() before fork,
vfork-style process creation and clone, and in longjmp and siglongjmp: when
TPIDR2_EL0 points at a TPIDR2 block with a save buffer, it stores ZA there
(the lazy save), clears TPIDR2 and turns ZA off. The probe loads ZA with a
pattern through a small code page, points TPIDR2 at a block, and after
os.fork(), os.posix_spawn(), a thread start, longjmp and siglongjmp checks
that the buffer holds the pattern, TPIDR2 reads 0 and SVCR.ZA is clear, in a
forked child too.

A signal handler installed through glibc's sigaction must start with SM and
ZA off and TPIDR2 0 when the signal interrupts a streaming region, and
returning through glibc's restorer must bring back streaming mode, ZA,
TPIDR2 and the whole of Z0; a handler may instead siglongjmp out of the
region. libgcc's SME support routines, which compiled SME code calls, must
report the state (__arm_sme_state), commit a lazy save with and without
turning ZA off (__arm_za_disable, __arm_tpidr2_save) and restore one
(__arm_tpidr2_restore).

It needs an engine that executes the SME profile. Its exit status is its
failure count.
"""

import ctypes
import os
import signal
import sys
import threading

libc = ctypes.CDLL(None, use_errno=True)
libc.mmap.restype = ctypes.c_void_p
libc.mmap.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, ctypes.c_long)
libc.mprotect.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int)
libgcc = ctypes.CDLL("libgcc_s.so.1")


class SigAction(ctypes.Structure):
    """glibc's struct sigaction on arm64."""
    _fields_ = (("handler", ctypes.c_void_p),
                ("mask", ctypes.c_uint64 * 16),
                ("flags", ctypes.c_int),
                ("restorer", ctypes.c_void_p))


libc.sigaction.argtypes = (ctypes.c_int, ctypes.POINTER(SigAction),
                           ctypes.POINTER(SigAction))

PROT_READ, PROT_WRITE, PROT_EXEC = 1, 2, 4
MAP_PRIVATE, MAP_ANONYMOUS = 0x02, 0x20
RET = 0xD65F03C0

# Each routine starts at a multiple of eight words in the code page. A name
# in a routine is a 64-bit literal, two words, that the probe fills in with
# that address before the page becomes executable.
ROUTINES = {
    "write_tpidr2": [0xD51BD0A0, RET],          # msr tpidr2_el0, x0
    "read_tpidr2": [0xD53BD0A0, RET],           # mrs x0, tpidr2_el0
    "read_svcr": [0xD53B4240, RET],             # mrs x0, svcr
    "rdsvl": [0x04BF5820, RET],                 # rdsvl x0, #1
    # smstart za, then ldr za[w12, 0], [x0] for each of x1 rows.
    "load_za": [0xD503457F, 0x5280000C, 0xE1000000, 0x8B010000,
                0x1100058C, 0xEB01019F, 0x54FFFF83, RET],
    "smstop_za": [0xD503447F, RET],             # smstop za
    "cntfrq": [0xD53BE000, RET],                # mrs x0, cntfrq_el0
    # ZA on: str za[w12, 0], [x0] for each of x1 rows.
    "store_za": [0x5280000C, 0xE1200000, 0x8B010000, 0x1100058C,
                 0xEB01019F, 0x54FFFF8B, RET],
    # x0(jmp_buf, setjmp, longjmp, savemask): setjmp(jmp_buf, savemask),
    # then longjmp(jmp_buf, 1); returns setjmp's second return value.
    "jump": [0xA9BE7BFD, 0x910003FD, 0xA90153F3, 0xAA0003F3,  # stp; mov x29
             0xAA0203F4, 0xAA0103E9, 0xAA0303E1, 0xD63F0120,  # blr x9
             0x350000A0, 0xAA1303E0, 0x52800021, 0xD63F0280,  # cbnz; blr x20
             0xD4200000, 0xA94153F3, 0xA8C27BFD, RET],        # brk #0
    # x0(function, out): calls it and stores its x0 and x1 at out.
    "call_pair": [0xA9BE7BFD, 0x910003FD, 0xF9000BF3, 0xAA0103F3,
                  0xD63F0000, 0xA9000660, 0xF9400BF3, 0xA8C27BFD, RET],
    # x0(pattern, out, tpidr2, ticks, count): smstart; ldr z0, [x0];
    # ldr za[w12, 0], [x0]; msr tpidr2_el0, x2; spin until *count moves or
    # ticks of CNTVCT pass; store SVCR and TPIDR2 to out, Z0 to out + 16 and
    # ZA row 0 to out + 16 + SVL; msr tpidr2_el0, xzr; smstop; return how
    # far *count moved while streaming.
    "stream_until_signal": [
        0xF9400085, 0xD503477F, 0x85804000, 0x5280000C,  # ldr x5; smstart
        0xE1000000, 0xD51BD0A2, 0xD53BE046, 0x8B0300C3,  # deadline in x3
        0xF9400086, 0xEB0500DF, 0x54000081, 0xD53BE046,  # count moved?
        0xEB0300DF, 0x54FFFF63, 0xD53B4246, 0xD53BD0A7,  # mrs svcr, tpidr2
        0xA8811C26, 0xE5804020, 0x04BF5826, 0x8B060021,  # str z0; rdsvl
        0xE1200020, 0xD51BD0BF, 0xD503467F, 0xF9400080,  # str za; smstop
        0xCB050000, RET],
    # The handler: stores SVCR and TPIDR2 to state[0] and state[1] and
    # counts its entry in state[2].
    "record": [0x58000109, 0xD53B424A, 0xD53BD0AB, 0xA9002D2A,
               0xF940092A, 0x9100054A, 0xF900092A, RET, "state"],
    # The handler that leaves: records as "record" does, then, once
    # state[3] arms it, siglongjmp(escape_buf, 1).
    "escape": [0x580001C9, 0xD53B424A, 0xD53BD0AB, 0xA9002D2A,
               0xF940092A, 0x9100054A, 0xF900092A, 0xF9400D2A,
               0xB40000AA, 0x580000E0, 0x52800021, 0x580000E9,
               0xD61F0120, RET, "state", "escape_buf", "siglongjmp"],
    # x0(sigjmp_buf, __sigsetjmp, pattern, tpidr2, ticks, armed):
    # __sigsetjmp(sigjmp_buf, 1); then *armed = 1; smstart;
    # ldr za[w12, 0], [pattern]; msr tpidr2_el0, tpidr2; spin for ticks;
    # msr tpidr2_el0, xzr; smstop. Returns 1 when a siglongjmp lands, 2 when
    # the time is up first, and disarms either way.
    "stream_escape": [
        0xA9BD7BFD, 0x910003FD, 0xA90153F3, 0xA9025BF5,
        0xAA0203F3, 0xAA0303F4, 0xAA0403F5, 0xAA0503F6,
        0xAA0103E9, 0x52800021, 0xD63F0120, 0x350001E0,  # __sigsetjmp
        0xD2800029, 0xF90002C9, 0xD503477F, 0x5280000C,  # arm; smstart
        0xE1000260, 0xD51BD0B4, 0xD53BE049, 0x8B150135,
        0xD53BE049, 0xEB15013F, 0x54FFFFC3, 0xD51BD0BF,  # spin
        0xD503467F, 0x52800040, 0xF90002DF, 0xA9425BF5,  # smstop; disarm
        0xA94153F3, 0xA8C37BFD, RET],
}
# SIGALRM's handler state: SVCR and TPIDR2 at entry, the entry count, and
# the escape handler's arming word.
state = (ctypes.c_uint64 * 4)()
# glibc's sigjmp_buf is 312 bytes on arm64.
escape_buf = (ctypes.c_uint64 * 64)()
jump_buf = (ctypes.c_uint64 * 64)()


def address_of(library: ctypes.CDLL, name: str) -> int:
    return ctypes.cast(getattr(library, name), ctypes.c_void_p).value


LITERALS = {
    "state": ctypes.addressof(state),
    "escape_buf": ctypes.addressof(escape_buf),
    "siglongjmp": address_of(libc, "siglongjmp"),
}


def build_code() -> tuple[dict, dict]:
    size = 4096
    page = libc.mmap(None, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
    if page in (None, ctypes.c_void_p(-1).value):
        raise OSError(ctypes.get_errno(), "mmap")
    words = (ctypes.c_uint32 * (size // 4)).from_address(page)
    addresses = {}
    index = 0
    for name, code in ROUTINES.items():
        addresses[name] = page + index * 4
        for word in code:
            if isinstance(word, str):
                assert index % 2 == 0
                words[index] = LITERALS[word] & 0xFFFFFFFF
                words[index + 1] = LITERALS[word] >> 32
                index += 2
            else:
                words[index] = word
                index += 1
        index = (index + 7) // 8 * 8
    assert index * 4 <= size
    if libc.mprotect(page, size, PROT_READ | PROT_EXEC) != 0:
        raise OSError(ctypes.get_errno(), "mprotect")
    function = ctypes.CFUNCTYPE(ctypes.c_uint64, *(ctypes.c_uint64,) * 6)

    def routine(address: int):
        call = function(address)
        return lambda *arguments: call(*arguments,
                                       *(0,) * (6 - len(arguments)))

    return ({name: routine(address) for name, address in addresses.items()},
            addresses)


code, addresses = build_code()
svl = code["rdsvl"](0, 0)
# Long enough for a timer signal to land on a loaded host, short enough to
# fail rather than hang.
spin_ticks = 10 * code["cntfrq"]()
rows = (ctypes.c_uint8 * (svl * svl))()
for index in range(svl * svl):
    rows[index] = (index * 7 + 3) & 0xFF
buffer = (ctypes.c_uint8 * (svl * svl))()
# The TPIDR2 block: save buffer, number of slices, then zero bytes.
block = (ctypes.c_uint64 * 2)()
failures = 0


def check(passed: bool, name: str) -> None:
    global failures
    if not passed:
        failures += 1
    print(f"{'PASS' if passed else 'FAIL'} glibc: {name}", flush=True)


def arm_lazy_save() -> None:
    ctypes.memset(buffer, 0, len(buffer))
    code["load_za"](ctypes.addressof(rows), svl)
    block[0] = ctypes.addressof(buffer)
    block[1] = svl
    code["write_tpidr2"](ctypes.addressof(block), 0)


def committed() -> bool:
    return (code["read_tpidr2"](0, 0) == 0 and
            (code["read_svcr"](0, 0) & 2) == 0 and
            bytes(buffer) == bytes(rows))


def main() -> int:
    print(f"glibc: streaming vector length {svl}", flush=True)

    arm_lazy_save()
    check(code["read_tpidr2"](0, 0) == ctypes.addressof(block)
          and code["read_svcr"](0, 0) & 2 == 2, "the lazy save is armed")
    pid = os.fork()
    if pid == 0:
        os._exit(0 if committed() else 1)
    _, status = os.waitpid(pid, 0)
    check(committed(), "fork commits the lazy save in the parent")
    check(os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0,
          "and the child starts with it committed")

    arm_lazy_save()
    pid = os.posix_spawn("/bin/true", ["true"], dict(os.environ))
    _, status = os.waitpid(pid, 0)
    check(committed() and os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0,
          "posix_spawn commits the lazy save")

    arm_lazy_save()
    seen = []
    thread = threading.Thread(target=lambda: seen.append(code["read_tpidr2"](0, 0)))
    thread.start()
    thread.join()
    check(committed(), "thread creation commits the lazy save")
    check(seen == [0], "the new thread starts with TPIDR2 0")

    for name, setjmp, longjmp, savemask in (
            ("longjmp", "_setjmp", "longjmp", 0),
            ("siglongjmp", "__sigsetjmp", "siglongjmp", 1)):
        arm_lazy_save()
        landed = code["jump"](ctypes.addressof(jump_buf),
                              address_of(libc, setjmp),
                              address_of(libc, longjmp), savemask)
        check(landed == 1 and committed(),
              f"{name} commits the lazy save")

    streaming_signal()
    escape_signal()
    libgcc_routines()
    code["smstop_za"](0, 0)
    return failures


def install(handler: str) -> SigAction:
    """Makes the named routine SIGALRM's handler, through glibc."""
    action, previous = SigAction(), SigAction()
    action.handler = addresses[handler]
    if libc.sigaction(signal.SIGALRM, ctypes.byref(action),
                      ctypes.byref(previous)) != 0:
        raise OSError(ctypes.get_errno(), "sigaction")
    return previous


def restore(previous: SigAction) -> None:
    signal.setitimer(signal.ITIMER_REAL, 0)
    if libc.sigaction(signal.SIGALRM, ctypes.byref(previous), None) != 0:
        raise OSError(ctypes.get_errno(), "sigaction")


def streaming_signal() -> None:
    pattern = bytes((index * 13 + 5) & 0xFF for index in range(svl))
    source = (ctypes.c_uint8 * svl).from_buffer_copy(pattern)
    out = (ctypes.c_uint8 * (16 + 2 * svl))()
    # A TPIDR2 block with no save buffer: ZA is live, nothing to save.
    block[0] = block[1] = 0
    ctypes.memset(state, 0, ctypes.sizeof(state))
    previous = install("record")
    signal.setitimer(signal.ITIMER_REAL, 0.2)
    count = code["stream_until_signal"](
        ctypes.addressof(source), ctypes.addressof(out),
        ctypes.addressof(block), spin_ticks, ctypes.addressof(state) + 16)
    restore(previous)
    after = (ctypes.c_uint64 * 2).from_buffer(out)
    check(count == 1 and state[2] == 1,
          "SIGALRM interrupts a streaming region")
    check(state[0] == 0 and state[1] == 0,
          "its handler starts with SM and ZA off and TPIDR2 0")
    check(after[0] == 3 and after[1] == ctypes.addressof(block),
          "returning brings back SM, ZA and TPIDR2")
    check(bytes(out[16:16 + svl]) == pattern,
          "and every byte of Z0")
    check(bytes(out[16 + svl:]) == pattern, "and ZA")


def escape_signal() -> None:
    pattern = bytes((index * 11 + 1) & 0xFF for index in range(svl))
    source = (ctypes.c_uint8 * svl).from_buffer_copy(pattern)
    block[0] = block[1] = 0
    ctypes.memset(state, 0, ctypes.sizeof(state))
    previous = install("escape")
    signal.setitimer(signal.ITIMER_REAL, 0.2)
    landed = code["stream_escape"](
        ctypes.addressof(escape_buf), address_of(libc, "__sigsetjmp"),
        ctypes.addressof(source), ctypes.addressof(block), spin_ticks,
        ctypes.addressof(state) + 24)
    restore(previous)
    check(landed == 1 and state[2] == 1 and state[0] == 0 and state[1] == 0,
          "a handler siglongjmps out of a streaming region")
    check(code["read_svcr"](0, 0) == 0 and code["read_tpidr2"](0, 0) == 0,
          "and lands with SM and ZA off and TPIDR2 0")
    check(signal.SIGALRM not in signal.pthread_sigmask(signal.SIG_BLOCK, ()),
          "and SIGALRM unblocked again")


def libgcc_routines() -> None:
    pair = (ctypes.c_uint64 * 2)()
    sme_state = address_of(libgcc, "__arm_sme_state")
    # Bits 63 and 62: SME and TPIDR2_EL0 are available; bit 1 PSTATE.ZA,
    # bit 0 PSTATE.SM.
    code["call_pair"](sme_state, ctypes.addressof(pair))
    check(pair[0] == 0xC000000000000000 and pair[1] == 0,
          "__arm_sme_state reports SME, ZA off and TPIDR2 0")
    arm_lazy_save()
    code["call_pair"](sme_state, ctypes.addressof(pair))
    check(pair[0] == 0xC000000000000002 and pair[1] == ctypes.addressof(block),
          "and ZA on with the TPIDR2 block")

    ctypes.CFUNCTYPE(None)(address_of(libgcc, "__arm_tpidr2_save"))()
    check(bytes(buffer) == bytes(rows) and
          code["read_tpidr2"](0, 0) == ctypes.addressof(block) and
          code["read_svcr"](0, 0) & 2 == 2,
          "__arm_tpidr2_save stores ZA and leaves TPIDR2 and ZA")
    arm_lazy_save()
    ctypes.CFUNCTYPE(None)(address_of(libgcc, "__arm_za_disable"))()
    check(committed(), "__arm_za_disable commits the lazy save")

    # ZA on and zeroed, TPIDR2 0, the save buffer holding the pattern.
    zeros = (ctypes.c_uint8 * (svl * svl))()
    code["load_za"](ctypes.addressof(zeros), svl)
    ctypes.memmove(buffer, rows, len(rows))
    block[0] = ctypes.addressof(buffer)
    block[1] = svl
    ctypes.CFUNCTYPE(None, ctypes.c_void_p)(
        address_of(libgcc, "__arm_tpidr2_restore"))(ctypes.addressof(block))
    restored = (ctypes.c_uint8 * (svl * svl))()
    code["store_za"](ctypes.addressof(restored), svl)
    check(bytes(restored) == bytes(rows) and code["read_tpidr2"](0, 0) == 0,
          "__arm_tpidr2_restore loads ZA from the save buffer")


if __name__ == "__main__":
    sys.exit(main())
