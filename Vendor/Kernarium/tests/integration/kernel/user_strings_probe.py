#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check how the kernel copies path and name strings from user memory.

This runs inside the guest as root. Every check passes a string at a chosen
offset of an anonymous mapping to a system call that copies it with
strncpy_from_user(): newfstatat() and symlinkat() through getname(), and
prctl(PR_SET_NAME). The checks cover lengths around the embedded name buffer
and PATH_MAX, strings that cross a page boundary or end on its last byte,
unterminated strings, faults on NULL, unmapped and PROT_NONE memory, and the
exact bytes a symlink target and a task name keep. Each check prints one PASS
or FAIL line; the exit status is the failure count. The contract is
kernel/common/README.md.
"""

from __future__ import annotations

import ctypes
import errno
import mmap
import os
import platform
import sys


SYSCALLS = {
    "aarch64": {"unlinkat": 35, "symlinkat": 36, "readlinkat": 78,
                "newfstatat": 79, "prctl": 167},
    "x86_64": {"newfstatat": 262, "unlinkat": 263, "symlinkat": 266,
               "readlinkat": 267, "prctl": 157},
}[platform.machine()]
AT_FDCWD = -100
AT_EMPTY_PATH = 0x1000
PR_SET_NAME = 15
PROT_NONE = 0
PAGE = os.sysconf("SC_PAGE_SIZE")
# getname() copies into a name buffer embedded in its page-sized struct.
EMBEDDED_NAME_MAX = 4096 - 32
PATH_MAX = 4096

libc = ctypes.CDLL(None, use_errno=True)
libc.syscall.restype = ctypes.c_long
libc.mmap.restype = ctypes.c_void_p
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, ctypes.c_long]
libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
failures = 0
statbuf = ctypes.create_string_buffer(256)


def check(name: str, expected: object, actual: object) -> None:
    global failures
    ok = expected == actual
    if not ok:
        failures += 1
    print(f"{'PASS' if ok else 'FAIL'} {name}: expected {expected!r}, "
          f"got {actual!r}", flush=True)


def region(pages: int) -> int:
    address = libc.mmap(None, pages * PAGE, mmap.PROT_READ | mmap.PROT_WRITE,
                        mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS, -1, 0)
    if address in (None, ctypes.c_void_p(-1).value):
        raise OSError(ctypes.get_errno(), "mmap failed")
    return address


def put(address: int, data: bytes) -> None:
    ctypes.memmove(address, data, len(data))


def call(name: str, *arguments: object) -> int:
    """Return 0 or the negated errno of one raw system call."""
    ctypes.set_errno(0)
    rc = libc.syscall(SYSCALLS[name], *arguments)
    return 0 if rc >= 0 else -ctypes.get_errno()


def stat_at(address: int, flags: int = 0, fd: int = AT_FDCWD) -> int:
    return call("newfstatat", fd, ctypes.c_void_p(address), statbuf, flags)


def path_of(length: int) -> bytes:
    """A path of exactly length bytes that resolves to /usr/bin."""
    return b"/" * (length - 7) + b"usr/bin"


def main() -> int:
    base = region(4)

    def clear() -> None:
        ctypes.memset(base, 0x41, 4 * PAGE)

    # A short path inside a page, with its NUL on the page's last byte or
    # the next page's first, and straddling the boundary.
    short = b"/usr/bin"
    for label, offset in (("start", 0), ("odd", 1), ("inside", PAGE - 64),
                          ("nul-last-byte", PAGE - len(short) - 1),
                          ("nul-next-page", PAGE - len(short)),
                          ("straddle-4", PAGE - 4), ("straddle-1", PAGE - 1)):
        clear()
        put(base + offset, short + b"\0")
        check(f"short-{label}", 0, stat_at(base + offset))

    # Lengths around the embedded name buffer and PATH_MAX, page-aligned and
    # crossing a page boundary early and late.
    for length, expected in ((EMBEDDED_NAME_MAX - 1, 0), (EMBEDDED_NAME_MAX, 0),
                             (EMBEDDED_NAME_MAX + 1, 0), (PATH_MAX - 1, 0),
                             (PATH_MAX, -errno.ENAMETOOLONG),
                             (PATH_MAX + 1, -errno.ENAMETOOLONG),
                             (2 * PATH_MAX, -errno.ENAMETOOLONG)):
        for label, offset in (("aligned", 0), ("crossing", PAGE - 100),
                              ("late", PAGE - 7)):
            clear()
            put(base + offset, path_of(length) + b"\0")
            check(f"len{length}-{label}", expected, stat_at(base + offset))
    # No NUL within PATH_MAX, the bytes continuing well past it.
    clear()
    check("unterminated", -errno.ENAMETOOLONG, stat_at(base + 8))

    # An empty path, and the same with AT_EMPTY_PATH on a descriptor.
    clear()
    put(base + PAGE - 1, b"\0")
    check("empty", -errno.ENOENT, stat_at(base + PAGE - 1))
    fd = os.open("/usr/bin", os.O_RDONLY)
    check("empty-at-empty-path", 0, stat_at(base + PAGE - 1, AT_EMPTY_PATH, fd))
    os.close(fd)

    # Faults: NULL, an unmapped page, PROT_NONE, a string that runs off its
    # mapping, and strings whose NUL is the last mapped byte.
    check("null", -errno.EFAULT, stat_at(0))
    hole = region(2)
    libc.munmap(hole + PAGE, PAGE)
    check("unmapped", -errno.EFAULT, stat_at(hole + PAGE))
    ctypes.memset(hole, 0x2F, PAGE)
    check("runs-off-mapping", -errno.EFAULT, stat_at(hole + PAGE - 100))
    put(hole + PAGE - 9, short + b"\0")
    check("nul-last-mapped-byte", 0, stat_at(hole + PAGE - 9))
    long_path = path_of(EMBEDDED_NAME_MAX) + b"\0"
    ctypes.memset(hole, 0x2F, PAGE)
    put(hole + PAGE - len(long_path), long_path)
    check("long-nul-last-mapped-byte", 0, stat_at(hole + PAGE - len(long_path)))
    guard = region(2)
    libc.mprotect(guard + PAGE, PAGE, PROT_NONE)
    ctypes.memset(guard, 0x2F, PAGE)
    check("runs-into-prot-none", -errno.EFAULT, stat_at(guard + PAGE - 50))
    check("prot-none", -errno.EFAULT, stat_at(guard + PAGE))

    # A symlink keeps exactly the bytes of its target.
    link = ctypes.create_string_buffer(b"/tmp/user-strings-link\0")
    out = ctypes.create_string_buffer(2 * PATH_MAX)
    for length in (1, 9, 100, EMBEDDED_NAME_MAX - 1, EMBEDDED_NAME_MAX,
                   PATH_MAX - 1):
        for label, offset in (("aligned", 0),
                              ("crossing", PAGE - length // 2 - 1)):
            clear()
            target = bytes(0x61 + index % 26 for index in range(length))
            put(base + offset, target + b"\0")
            call("unlinkat", AT_FDCWD, link, 0)
            status = call("symlinkat", ctypes.c_void_p(base + offset),
                          AT_FDCWD, link)
            kept = b""
            if status == 0:
                count = libc.syscall(SYSCALLS["readlinkat"], AT_FDCWD, link,
                                     out, len(out))
                kept = out.raw[:count] if count >= 0 else b""
            check(f"symlink{length}-{label}", (0, True),
                  (status, kept == target))
    call("unlinkat", AT_FDCWD, link, 0)
    clear()
    put(base + PAGE - 10, b"x" * PATH_MAX + b"\0")
    check("symlink-too-long", -errno.ENAMETOOLONG,
          call("symlinkat", ctypes.c_void_p(base + PAGE - 10), AT_FDCWD, link))

    # PR_SET_NAME keeps at most 15 bytes, and a name that crosses a page
    # boundary arrives whole.
    for label, name, offset in (
            ("long-crossing", b"abcdefghijklmnopqrstuvwxyz", PAGE - 7),
            ("short-crossing", b"user-str", PAGE - 3),
            ("exact-15", b"abcdefghijklmno", PAGE - 15)):
        clear()
        put(base + offset, name + b"\0")
        status = call("prctl", PR_SET_NAME, ctypes.c_void_p(base + offset),
                      0, 0, 0)
        with open("/proc/self/comm", "rb") as comm:
            kept = comm.read().rstrip(b"\n")
        check(f"comm-{label}", (0, name[:15]), (status, kept))

    print(f"{failures} failure(s)", flush=True)
    return failures


if __name__ == "__main__":
    sys.exit(main())
