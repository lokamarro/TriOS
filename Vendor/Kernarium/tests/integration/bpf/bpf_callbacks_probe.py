#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise BPF callbacks on an interpreter-only guest kernel.

This runs inside the guest as root. It assembles programs by hand, describes
their functions and map values with hand-built BTF, and loads them through the
raw bpf() system call, so it needs nothing beyond Python's standard library.
It runs bpf_loop(), inlined and through the helper, bpf_for_each_map_elem(), a
self-rearming BPF timer, nested callbacks and tail calls into callback
programs; checks that the verifier's frame, stack and tail-call limits still
reject; and exhausts and refills the interpreter's callback entry-point pool.
Each check prints one PASS or FAIL line; the exit status is the failure count.
"""

from __future__ import annotations

import ctypes
import errno
import os
import platform
import resource
import struct
import sys
import time


NR_BPF = {"aarch64": 280, "x86_64": 321}[platform.machine()]

BPF_MAP_CREATE = 0
BPF_MAP_LOOKUP_ELEM = 1
BPF_MAP_UPDATE_ELEM = 2
BPF_PROG_LOAD = 5
BPF_PROG_TEST_RUN = 10
BPF_OBJ_GET_INFO_BY_FD = 15
BPF_BTF_LOAD = 18

BPF_MAP_TYPE_HASH = 1
BPF_MAP_TYPE_ARRAY = 2
BPF_MAP_TYPE_PROG_ARRAY = 3
BPF_PROG_TYPE_SOCKET_FILTER = 1

HELPER_MAP_LOOKUP_ELEM = 1
HELPER_TAIL_CALL = 12
HELPER_FOR_EACH_MAP_ELEM = 164
HELPER_TIMER_INIT = 169
HELPER_TIMER_SET_CALLBACK = 170
HELPER_TIMER_START = 171
HELPER_LOOP = 181

BPF_MAX_LOOPS = 1 << 23
# struct __sk_buff: mark follows len and pkt_type. A test run leaves it zero,
# but the verifier cannot know that, which keeps bpf_loop() from inlining.
SKB_MARK = 8
CLOCK_MONOTONIC = 1

R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10 = range(11)

libc = ctypes.CDLL(None, use_errno=True)
libc.syscall.restype = ctypes.c_long


class BpfError(OSError):
    def __init__(self, error: int, log: str = "") -> None:
        super().__init__(error, os.strerror(error))
        self.log = log


def bpf(command: int, attr: ctypes.Array) -> int:
    result = libc.syscall(NR_BPF, command, ctypes.byref(attr), len(attr))
    if result < 0:
        raise BpfError(ctypes.get_errno())
    return result


def attr_buffer(size: int = 160) -> ctypes.Array:
    return ctypes.create_string_buffer(size)


def put(attr: ctypes.Array, offset: int, fmt: str, *values) -> None:
    struct.pack_into("<" + fmt, attr, offset, *values)


def address(buffer) -> int:
    return ctypes.addressof(buffer)


# --- BTF -------------------------------------------------------------------


class Btf:
    """A small BTF blob builder; type IDs start at 1."""

    def __init__(self) -> None:
        self.types: list[bytes] = []
        self.strings = bytearray(b"\0")

    def name(self, text: str) -> int:
        if not text:
            return 0
        offset = len(self.strings)
        self.strings += text.encode() + b"\0"
        return offset

    def add(self, data: bytes) -> int:
        self.types.append(data)
        return len(self.types)

    def integer(self, name: str, size: int, signed: bool) -> int:
        encoding = 1 if signed else 0
        return self.add(struct.pack("<IIII", self.name(name), 1 << 24, size,
                                    encoding << 24 | size * 8))

    def array(self, element: int, index: int, count: int) -> int:
        return self.add(struct.pack("<IIIIII", 0, 3 << 24, 0, element, index,
                                    count))

    def structure(self, name: str, size: int,
                  members: list[tuple[str, int, int]]) -> int:
        data = struct.pack("<III", self.name(name), 4 << 24 | len(members), size)
        for member, type_id, bit_offset in members:
            data += struct.pack("<III", self.name(member), type_id, bit_offset)
        return self.add(data)

    def prototype(self, returns: int, parameters: list[tuple[str, int]]) -> int:
        data = struct.pack("<III", 0, 13 << 24 | len(parameters), returns)
        for parameter, type_id in parameters:
            data += struct.pack("<II", self.name(parameter), type_id)
        return self.add(data)

    def function(self, name: str, prototype: int, is_global: bool) -> int:
        return self.add(struct.pack("<III", self.name(name),
                                    12 << 24 | (1 if is_global else 0),
                                    prototype))

    def load(self) -> int:
        types = b"".join(self.types)
        header = struct.pack("<HBBIIIII", 0xEB9F, 1, 0, 24, 0, len(types),
                             len(types), len(self.strings))
        blob = ctypes.create_string_buffer(header + types + bytes(self.strings))
        log = ctypes.create_string_buffer(65536)
        attr = attr_buffer()
        put(attr, 0, "QQIII", address(blob), address(log), len(blob) - 1,
            len(log), 1)
        try:
            return bpf(BPF_BTF_LOAD, attr)
        except BpfError as error:
            raise BpfError(error.errno, log.value.decode(errors="replace"))


class Types:
    """The BTF shared by every program and map in this probe."""

    def __init__(self) -> None:
        btf = Btf()
        self.int = btf.integer("int", 4, True)
        u64 = btf.integer("unsigned long long", 8, False)
        main = btf.prototype(self.int, [("ctx", u64)])
        callback = btf.prototype(self.int, [(f"a{i}", u64) for i in range(5)])
        self.main = btf.function("main", main, True)
        self.callbacks = [btf.function(f"callback{i}", callback, False)
                          for i in range(16)]
        opaque = btf.array(u64, self.int, 2)
        timer = btf.structure("bpf_timer", 16, [("__opaque", opaque, 0)])
        self.timer_value = btf.structure(
            "timer_value", 24, [("timer", timer, 0), ("count", u64, 128)])
        self.fd = btf.load()


# --- Assembler -------------------------------------------------------------


class Asm:
    """Assemble eBPF with labels; functions become BTF subprograms."""

    def __init__(self) -> None:
        self.slots: list[list] = []
        self.labels: dict[str, int] = {}
        self.functions: list[tuple[int, str]] = []

    def here(self) -> int:
        return sum(2 if slot[0] == "ld64" else 1 for slot in self.slots)

    def label(self, name: str) -> None:
        self.labels[name] = self.here()

    def function(self, name: str) -> None:
        self.label(name)
        self.functions.append((self.here(), name))

    def raw(self, code, dst=0, src=0, off=0, imm=0) -> None:
        self.slots.append(["insn", code, dst, src, off, imm])

    def mov(self, dst, imm) -> None:
        self.raw(0xB7, dst, 0, 0, imm)

    def movr(self, dst, src) -> None:
        self.raw(0xBF, dst, src)

    def add(self, dst, imm) -> None:
        self.raw(0x07, dst, 0, 0, imm)

    def addr(self, dst, src) -> None:
        self.raw(0x0F, dst, src)

    def mul(self, dst, imm) -> None:
        self.raw(0x27, dst, 0, 0, imm)

    def ldx(self, dst, src, off, size=0x18) -> None:
        self.raw(0x61 | size, dst, src, off)

    def stx(self, dst, off, src, size=0x18) -> None:
        self.raw(0x63 | size, dst, src, off)

    def st(self, dst, off, imm, size=0x18) -> None:
        self.raw(0x62 | size, dst, 0, off, imm)

    def jump(self, op, dst, imm, target: str) -> None:
        self.slots.append(["jmp", 0x05 | op, dst, 0, target, imm])

    def call(self, helper: int) -> None:
        self.raw(0x85, 0, 0, 0, helper)

    def exit(self) -> None:
        self.raw(0x95)

    def ld64(self, dst, value, src=0) -> None:
        self.slots.append(["ld64", dst, src, value])

    def ld_map(self, dst, fd: int) -> None:
        self.ld64(dst, fd, 1)

    def ld_function(self, dst, target: str) -> None:
        self.ld64(dst, target, 4)

    def build(self) -> tuple[bytes, list[tuple[int, int]]]:
        code = bytearray()
        position = 0
        for slot in self.slots:
            kind = slot[0]
            if kind == "insn":
                _, op, dst, src, off, imm = slot
                code += struct.pack("<BBhi", op, src << 4 | dst, off, imm)
                position += 1
            elif kind == "jmp":
                _, op, dst, src, target, imm = slot
                off = self.labels[target] - position - 1
                code += struct.pack("<BBhi", op, src << 4 | dst, off, imm)
                position += 1
            else:
                _, dst, src, value = slot
                if src == 4:
                    value = self.labels[value] - position - 1
                    low, high = value, 0
                else:
                    low, high = value & 0xFFFFFFFF, value >> 32
                code += struct.pack("<BBhi", 0x18, src << 4 | dst, 0,
                                    ctypes.c_int32(low).value)
                code += struct.pack("<BBhi", 0, 0, 0, ctypes.c_int32(high).value)
                position += 2
        return bytes(code), [(start, index) for index, (start, _) in
                             enumerate(self.functions)]


# --- Kernel objects --------------------------------------------------------


def create_map(map_type: int, key_size: int, value_size: int, entries: int,
               types: Types | None = None, value_type: int = 0) -> int:
    attr = attr_buffer()
    put(attr, 0, "IIII", map_type, key_size, value_size, entries)
    if types is not None:
        put(attr, 48, "III", types.fd, types.int, value_type)
    return bpf(BPF_MAP_CREATE, attr)


def lookup(map_fd: int, key: bytes, size: int) -> bytes:
    key_buffer = ctypes.create_string_buffer(key)
    value_buffer = ctypes.create_string_buffer(size)
    attr = attr_buffer()
    put(attr, 0, "IIQQQ", map_fd, 0, address(key_buffer),
        address(value_buffer), 0)
    bpf(BPF_MAP_LOOKUP_ELEM, attr)
    return value_buffer.raw[:size]


def update(map_fd: int, key: bytes, value: bytes) -> None:
    key_buffer = ctypes.create_string_buffer(key)
    value_buffer = ctypes.create_string_buffer(value)
    attr = attr_buffer()
    put(attr, 0, "IIQQQ", map_fd, 0, address(key_buffer),
        address(value_buffer), 0)
    bpf(BPF_MAP_UPDATE_ELEM, attr)


def load(types: Types, asm: Asm) -> int:
    code, functions = asm.build()
    insns = ctypes.create_string_buffer(code)
    license_text = ctypes.create_string_buffer(b"GPL")
    records = b"".join(
        struct.pack("<II", start,
                    types.main if index == 0 else types.callbacks[index - 1])
        for start, index in functions)
    func_info = ctypes.create_string_buffer(records)
    log = ctypes.create_string_buffer(1 << 20)
    attr = attr_buffer()
    put(attr, 0, "IIQQIIQ", BPF_PROG_TYPE_SOCKET_FILTER, len(code) // 8,
        address(insns), address(license_text), 1, len(log), address(log))
    put(attr, 72, "IIQI", types.fd, 8, address(func_info), len(functions))
    try:
        return bpf(BPF_PROG_LOAD, attr)
    except BpfError as error:
        raise BpfError(error.errno, log.value.decode(errors="replace"))


def run(prog_fd: int) -> int:
    packet = ctypes.create_string_buffer(64)
    attr = attr_buffer()
    put(attr, 0, "IIIIQQI", prog_fd, 0, len(packet), 0, address(packet), 0, 1)
    bpf(BPF_PROG_TEST_RUN, attr)
    return struct.unpack_from("<I", attr, 4)[0]


def prog_info(prog_fd: int, insns=None) -> ctypes.Array:
    """Return struct bpf_prog_info, optionally with the translated program."""
    info = ctypes.create_string_buffer(80)
    if insns is not None:
        struct.pack_into("<IQ", info, 20, len(insns), 0)
        struct.pack_into("<Q", info, 32, address(insns))
    attr = attr_buffer()
    put(attr, 0, "IIQ", prog_fd, len(info), address(info))
    bpf(BPF_OBJ_GET_INFO_BY_FD, attr)
    return info


def jited_length(prog_fd: int) -> int:
    return struct.unpack_from("<I", prog_info(prog_fd), 16)[0]


def xlated(prog_fd: int) -> bytes:
    length = struct.unpack_from("<I", prog_info(prog_fd), 20)[0]
    insns = ctypes.create_string_buffer(length)
    prog_info(prog_fd, insns)
    return insns.raw[:length]


# --- Programs --------------------------------------------------------------


def loop_program(count: int, stop_at: int | None, inline: bool) -> Asm:
    """Return the callback's sum of indices times 1000 plus bpf_loop()."""
    asm = Asm()
    asm.function("main")
    asm.st(R10, -8, 0)
    if inline:
        asm.mov(R4, 0)
    else:
        asm.ldx(R4, R1, SKB_MARK, 0x00)
    asm.mov(R1, count)
    asm.ld_function(R2, "callback")
    asm.movr(R3, R10)
    asm.add(R3, -8)
    asm.call(HELPER_LOOP)
    asm.movr(R6, R0)
    asm.ldx(R0, R10, -8)
    asm.mul(R0, 1000)
    asm.addr(R0, R6)
    asm.exit()
    asm.function("callback")
    asm.ldx(R3, R2, 0)
    asm.addr(R3, R1)
    asm.stx(R2, 0, R3)
    asm.mov(R0, 0)
    if stop_at is not None:
        asm.jump(0x50, R1, stop_at, "keep_going")
        asm.mov(R0, 1)
        asm.label("keep_going")
    asm.exit()
    return asm


def for_each_program(map_fd: int) -> Asm:
    """Return the sum of the map's values times 1000 plus the elements seen."""
    asm = Asm()
    asm.function("main")
    asm.st(R10, -8, 0)
    asm.ld_map(R1, map_fd)
    asm.ld_function(R2, "callback")
    asm.movr(R3, R10)
    asm.add(R3, -8)
    asm.mov(R4, 0)
    asm.call(HELPER_FOR_EACH_MAP_ELEM)
    asm.movr(R6, R0)
    asm.ldx(R0, R10, -8)
    asm.mul(R0, 1000)
    asm.addr(R0, R6)
    asm.exit()
    asm.function("callback")
    asm.ldx(R5, R3, 0)
    asm.ldx(R6, R4, 0)
    asm.addr(R6, R5)
    asm.stx(R4, 0, R6)
    asm.mov(R0, 0)
    asm.exit()
    return asm


def nested_program(depth: int, iterations: int) -> Asm:
    """Nest bpf_loop() depth levels deep; the innermost callback counts."""
    asm = Asm()
    asm.function("main")
    asm.st(R10, -8, 0)
    asm.ldx(R6, R1, SKB_MARK, 0x00)
    asm.stx(R10, -16, R6)
    asm.mov(R1, iterations)
    asm.ld_function(R2, "level1")
    asm.movr(R3, R10)
    asm.add(R3, -16)
    asm.movr(R4, R6)
    asm.call(HELPER_LOOP)
    asm.ldx(R0, R10, -8)
    asm.exit()
    for level in range(1, depth + 1):
        asm.function(f"level{level}")
        if level == depth:
            # ctx points at the saved flags; the counter sits just above it.
            asm.ldx(R3, R2, 8)
            asm.add(R3, 1)
            asm.stx(R2, 8, R3)
        else:
            asm.ldx(R4, R2, 0)
            asm.movr(R3, R2)
            asm.mov(R1, iterations)
            asm.ld_function(R2, f"level{level + 1}")
            asm.call(HELPER_LOOP)
        asm.mov(R0, 0)
        asm.exit()
    return asm


def stack_program(main_bytes: int, callback_bytes: int) -> Asm:
    """Fill both frames, then check the caller's frame survived the callback."""
    asm = Asm()
    asm.function("main")
    for offset in range(8, main_bytes + 1, 8):
        asm.st(R10, -offset, offset)
    asm.mov(R1, 1)
    asm.ld_function(R2, "callback")
    asm.mov(R3, 0)
    asm.mov(R4, 0)
    asm.call(HELPER_LOOP)
    asm.mov(R0, 0)
    for offset in range(8, main_bytes + 1, 8):
        asm.ldx(R1, R10, -offset)
        asm.addr(R0, R1)
    asm.exit()
    asm.function("callback")
    for offset in range(8, callback_bytes + 1, 8):
        asm.st(R10, -offset, 1000000 + offset)
    asm.mov(R0, 0)
    asm.exit()
    return asm


def tail_caller_program(prog_array_fd: int) -> Asm:
    asm = Asm()
    asm.function("main")
    asm.ld_map(R2, prog_array_fd)
    asm.mov(R3, 0)
    asm.call(HELPER_TAIL_CALL)
    asm.mov(R0, 1)
    asm.exit()
    return asm


def tail_and_callback_program(prog_array_fd: int) -> Asm:
    asm = Asm()
    asm.function("main")
    asm.movr(R6, R1)
    asm.st(R10, -8, 0)
    asm.mov(R1, 4)
    asm.ld_function(R2, "callback")
    asm.movr(R3, R10)
    asm.add(R3, -8)
    asm.mov(R4, 0)
    asm.call(HELPER_LOOP)
    asm.movr(R1, R6)
    asm.ld_map(R2, prog_array_fd)
    asm.mov(R3, 0)
    asm.call(HELPER_TAIL_CALL)
    asm.mov(R0, 1)
    asm.exit()
    asm.function("callback")
    asm.mov(R0, 0)
    asm.exit()
    return asm


def timer_program(map_fd: int, rounds: int) -> Asm:
    """Arm a timer whose callback counts and re-arms itself rounds times."""
    asm = Asm()
    asm.function("main")
    asm.st(R10, -4, 0, 0x00)
    asm.ld_map(R1, map_fd)
    asm.movr(R2, R10)
    asm.add(R2, -4)
    asm.call(HELPER_MAP_LOOKUP_ELEM)
    asm.jump(0x50, R0, 0, "found")
    asm.mov(R0, 1)
    asm.exit()
    asm.label("found")
    asm.movr(R6, R0)
    asm.movr(R1, R6)
    asm.ld_map(R2, map_fd)
    asm.mov(R3, CLOCK_MONOTONIC)
    asm.call(HELPER_TIMER_INIT)
    asm.mov(R7, 1000)
    asm.jump(0x50, R0, 0, "failed")
    asm.movr(R1, R6)
    asm.ld_function(R2, "callback")
    asm.call(HELPER_TIMER_SET_CALLBACK)
    asm.mov(R7, 2000)
    asm.jump(0x50, R0, 0, "failed")
    asm.movr(R1, R6)
    asm.mov(R2, 1000000)
    asm.mov(R3, 0)
    asm.call(HELPER_TIMER_START)
    asm.mov(R7, 3000)
    asm.jump(0x50, R0, 0, "failed")
    asm.exit()
    # Report which helper failed and its errno.
    asm.label("failed")
    asm.mul(R0, -1)
    asm.addr(R0, R7)
    asm.exit()
    asm.function("callback")
    asm.ldx(R6, R3, 16)
    asm.add(R6, 1)
    asm.stx(R3, 16, R6)
    asm.jump(0xA0, R6, rounds, "rearm")
    asm.mov(R0, 0)
    asm.exit()
    asm.label("rearm")
    asm.movr(R1, R3)
    asm.mov(R2, 1000000)
    asm.mov(R3, 0)
    asm.call(HELPER_TIMER_START)
    asm.mov(R0, 0)
    asm.exit()
    return asm


def slot_program(callbacks: int, references: int = 1) -> Asm:
    """Load callbacks distinct callbacks, each referenced references times."""
    asm = Asm()
    asm.function("main")
    asm.mov(R6, 0)
    for index in range(callbacks):
        for _ in range(references):
            asm.mov(R1, 1)
            asm.ld_function(R2, f"callback{index}")
            asm.mov(R3, 0)
            asm.mov(R4, 0)
            asm.call(HELPER_LOOP)
            asm.addr(R6, R0)
    asm.movr(R0, R6)
    asm.exit()
    for index in range(callbacks):
        asm.function(f"callback{index}")
        asm.mov(R0, 0)
        asm.exit()
    return asm


# --- Checks ----------------------------------------------------------------


failures = 0


def report(name: str, ok: bool, detail: str) -> None:
    global failures
    if not ok:
        failures += 1
    print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}", flush=True)


def expect_value(name: str, types: Types, asm: Asm, expected: int) -> int | None:
    try:
        fd = load(types, asm)
    except BpfError as error:
        report(name, False, f"load failed: {error.strerror}\n{error.log[-1500:]}")
        return None
    value = run(fd)
    report(name, value == expected, f"returned {value}, expected {expected}")
    return fd


def expect_rejected(name: str, types: Types, asm: Asm, error_number: int,
                    message: str) -> None:
    try:
        fd = load(types, asm)
    except BpfError as error:
        ok = error.errno == error_number and message in error.log
        tail = error.log.strip().splitlines()[-1:] or [""]
        report(name, ok, f"{errno.errorcode.get(error.errno)}: {tail[0]}")
        return
    os.close(fd)
    report(name, False, "the verifier accepted it")


def pseudo_calls(code: bytes) -> int:
    return sum(1 for i in range(0, len(code), 8)
               if code[i] == 0x85 and code[i + 1] >> 4 == 1)


def check_dump(name: str, fd: int) -> None:
    code = xlated(fd)
    count = len(code) // 8
    loads = []
    for index in range(count):
        op, regs, _, imm = struct.unpack_from("<BBhi", code, index * 8)
        if op == 0x18 and regs >> 4 == 4:
            high = struct.unpack_from("<i", code, index * 8 + 12)[0]
            loads.append((index, imm, high))
    ok = bool(loads) and all(0 < index + imm + 1 < count and high >= 1
                             for index, imm, high in loads)
    report(name, ok, f"callback loads (insn, offset, subprog) {loads}")


def fill_slots(types: Types, limit: int) -> tuple[list[int], BpfError | None]:
    fds: list[int] = []
    while len(fds) < limit:
        try:
            fds.append(load(types, slot_program(1)))
        except BpfError as error:
            return fds, error
    return fds, None


def wait_for_capacity(types: Types, expected: int) -> int:
    """Released slots return after an RCU grace period; poll for them."""
    deadline = time.monotonic() + 20
    while True:
        fds, _ = fill_slots(types, expected + 1)
        count = len(fds)
        for fd in fds:
            os.close(fd)
        if count >= expected or time.monotonic() >= deadline:
            return count
        time.sleep(0.5)


def load_when_free(types: Types, asm_factory, seconds: float = 20) -> int | None:
    """Retry a load until a released slot comes back after its grace period."""
    deadline = time.monotonic() + seconds
    while True:
        try:
            return load(types, asm_factory())
        except BpfError as error:
            if error.errno != errno.ENOSPC or time.monotonic() >= deadline:
                return None
        time.sleep(0.2)


def check_one_free_slot(types: Types, fds: list[int]) -> None:
    """With the pool full, free exactly one slot and use it three ways."""
    os.close(fds.pop())
    shared = load_when_free(types, lambda: slot_program(1, references=3))
    report("slots-shared-by-references", shared is not None,
           "three loads of one callback fit the one free slot")
    if shared is None:
        return
    os.close(shared)
    # Two distinct callbacks never fit one slot. The refused load binds its
    # first callback before failing, and must give that slot back too.
    refused = accepted = None
    deadline = time.monotonic() + 20
    while accepted is None and time.monotonic() < deadline:
        try:
            os.close(load(types, slot_program(2)))
            refused = False
            break
        except BpfError as error:
            if error.errno != errno.ENOSPC:
                break
            refused = True
        accepted = load_when_free(types, lambda: slot_program(1), 1)
    report("slots-two-callbacks-need-two", refused is True,
           "a program with two callbacks was refused the one free slot")
    report("slots-partial-bind-released", accepted is not None,
           "the refused load released the slot it had bound")
    if accepted is not None:
        fds.append(accepted)


def main() -> int:
    resource.setrlimit(resource.RLIMIT_NOFILE, (8192, 8192))
    jit = os.path.exists("/proc/sys/net/core/bpf_jit_enable")
    report("no-jit", not jit, "the kernel has no BPF JIT" if not jit
           else "bpf_jit_enable exists, so callbacks may not use the pool")
    types = Types()

    for inline in (True, False):
        path = "inline" if inline else "helper"
        fd = expect_value(f"bpf_loop-{path}", types,
                          loop_program(100, None, inline), 4950 * 1000 + 100)
        if fd is not None:
            report(f"bpf_loop-{path}-interpreted", jited_length(fd) == 0,
                   f"jited length {jited_length(fd)}")
            calls = pseudo_calls(xlated(fd))
            report(f"bpf_loop-{path}-path", (calls == 1) == inline,
                   f"{calls} bpf-to-bpf call(s) in the translated program")
            check_dump(f"bpf_loop-{path}-dump", fd)
            os.close(fd)
        fd = expect_value(f"bpf_loop-{path}-stop", types,
                          loop_program(100, 41, inline), 861 * 1000 + 42)
        if fd is not None:
            os.close(fd)
        fd = expect_value(f"bpf_loop-{path}-too-many", types,
                          loop_program(BPF_MAX_LOOPS + 1, None, inline),
                          (-errno.E2BIG) & 0xFFFFFFFF)
        if fd is not None:
            os.close(fd)

    array = create_map(BPF_MAP_TYPE_ARRAY, 4, 8, 8)
    for index in range(8):
        update(array, struct.pack("<I", index), struct.pack("<Q", index + 1))
    fd = expect_value("for_each-array", types, for_each_program(array),
                      36 * 1000 + 8)
    if fd is not None:
        os.close(fd)
    os.close(array)
    table = create_map(BPF_MAP_TYPE_HASH, 4, 8, 16)
    for index in range(5):
        update(table, struct.pack("<I", 10 + index),
               struct.pack("<Q", 100 + index))
    fd = expect_value("for_each-hash", types, for_each_program(table),
                      510 * 1000 + 5)
    if fd is not None:
        os.close(fd)
    os.close(table)

    # A timer callback runs from hrtimer expiry, outside any program run.
    timers = create_map(BPF_MAP_TYPE_ARRAY, 4, 24, 1, types, types.timer_value)
    fd = expect_value("timer-arm", types, timer_program(timers, 5), 0)
    if fd is not None:
        count = 0
        deadline = time.monotonic() + 10
        while count < 5 and time.monotonic() < deadline:
            time.sleep(0.05)
            count = struct.unpack_from(
                "<Q", lookup(timers, struct.pack("<I", 0), 24), 16)[0]
        # The fifth run does not re-arm, so the count must stay there.
        time.sleep(0.2)
        count = struct.unpack_from(
            "<Q", lookup(timers, struct.pack("<I", 0), 24), 16)[0]
        report("timer-callback", count == 5, f"callback ran {count} times")
        os.close(fd)
    os.close(timers)

    for depth in (2, 7):
        fd = expect_value(f"nested-{depth}", types, nested_program(depth, 2),
                          2 ** depth)
        if fd is not None:
            os.close(fd)
    expect_rejected("nested-8", types, nested_program(8, 2), errno.E2BIG,
                    "the call stack of 9 frames is too deep")

    fd = expect_value("stack-256+256", types, stack_program(256, 256),
                      sum(range(8, 257, 8)))
    if fd is not None:
        os.close(fd)
    expect_rejected("stack-256+288", types, stack_program(256, 288),
                    errno.EACCES, "combined stack size of 2 calls")

    programs = create_map(BPF_MAP_TYPE_PROG_ARRAY, 4, 4, 1)
    target = expect_value("tail-target", types, loop_program(10, None, False),
                          45 * 1000 + 10)
    if target is not None:
        update(programs, struct.pack("<I", 0), struct.pack("<I", target))
        try:
            caller = load(types, tail_caller_program(programs))
            value = run(caller)
            report("tail-call-into-callbacks", value == 45 * 1000 + 10,
                   f"returned {value}")
            os.close(caller)
        except BpfError as error:
            report("tail-call-into-callbacks", False,
                   f"{error.strerror}\n{error.log[-1500:]}")
        os.close(target)
    expect_rejected("tail-call-with-callback", types,
                    tail_and_callback_program(programs), errno.EINVAL,
                    "mixing of tail_calls and bpf-to-bpf calls is not supported")
    os.close(programs)

    fds, error = fill_slots(types, 4096)
    capacity = len(fds)
    ok = (error is not None and error.errno == errno.ENOSPC and
          "no free interpreter callback slot" in error.log)
    report("slots-exhausted", ok and capacity > 0,
           f"{capacity} programs bound a slot, then "
           f"{errno.errorcode.get(error.errno) if error else 'no error'}")
    if fds:
        check_one_free_slot(types, fds)
    for fd in fds:
        os.close(fd)
    again = wait_for_capacity(types, capacity)
    report("slots-released", again == capacity,
           f"{again} slots bound after closing, {capacity} before")

    os.close(types.fd)
    print(f"{failures} failure(s)", flush=True)
    return failures


if __name__ == "__main__":
    sys.exit(main())
