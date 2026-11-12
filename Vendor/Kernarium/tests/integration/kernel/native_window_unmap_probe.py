#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unmap user memory on both sides of the kernel's native window.

This runs inside the guest as root. The Darwin kernel shares its address space
with its tasks: a native window holds guest RAM, kernel virtual mappings and
the host's stub pages, and no user mapping may be placed inside it. A task can
still map pages right below and right above the window and remove them with one
munmap() whose range spans the window. The kernel then flushes the page-table
pages it frees on both sides as one range, and a page-table page that maps the
first page above the window also covers the window's last pages.

The probe finds the window from the addresses mmap() refuses, then unmaps
pages below it, above it, and across it. Each check prints one PASS or FAIL
line; the exit status is the failure count. A kernel that hands such a range
to the host whole panics instead.
"""

from __future__ import annotations

import ctypes
import errno
import mmap
import os
import sys


MAP_FIXED_NOREPLACE = 0x100000
PAGE = os.sysconf("SC_PAGE_SIZE")
# One page-table page maps this much: a page of eight-byte entries.
TABLE_SPAN = PAGE * (PAGE // 8)
SCAN_START = 1 << 32
SCAN_END = 1 << 47
SCAN_STEP = 1 << 30

libc = ctypes.CDLL(None, use_errno=True)
libc.mmap.restype = ctypes.c_void_p
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, ctypes.c_long]
libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
MAP_FAILED = ctypes.c_void_p(-1).value
failures = 0


def check(name: str, ok: bool, detail: str) -> None:
    global failures
    if not ok:
        failures += 1
    print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}", flush=True)


def map_page(address: int) -> int:
    """Map one page at exactly address; return 0 or the negated errno."""
    ctypes.set_errno(0)
    result = libc.mmap(address, PAGE, mmap.PROT_READ | mmap.PROT_WRITE,
                       mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS |
                       MAP_FIXED_NOREPLACE, -1, 0)
    if result in (None, MAP_FAILED):
        return -ctypes.get_errno()
    if result != address:
        libc.munmap(result, PAGE)
        return -errno.EEXIST
    return 0


def unmap(address: int, length: int) -> int:
    ctypes.set_errno(0)
    return 0 if libc.munmap(address, length) == 0 else -ctypes.get_errno()


def refused(address: int) -> bool:
    """Whether mmap() refuses address as outside every guest range."""
    result = map_page(address)
    if result == 0:
        unmap(address, PAGE)
    return result == -errno.EINVAL


def boundary(low: int, high: int, low_refused: bool) -> int:
    """Return the lowest page in (low, high] whose refusal differs from low's."""
    while high - low > PAGE:
        middle = (low + high) // 2 // PAGE * PAGE
        if refused(middle) == low_refused:
            low = middle
        else:
            high = middle
    return high


def kernel_text() -> int | None:
    """The kernel's text lies in the window; start there when it is visible."""
    try:
        with open("/proc/kallsyms", encoding="ascii") as symbols:
            for line in symbols:
                fields = line.split()
                if fields[2] == "_stext" and int(fields[0], 16):
                    return int(fields[0], 16) // SCAN_STEP * SCAN_STEP
    except OSError:
        pass
    return None


def find_window() -> tuple[int, int]:
    address = kernel_text()
    if address is None or not refused(address):
        address = SCAN_START
        while address < SCAN_END and not refused(address):
            address += SCAN_STEP
    else:
        while refused(address - SCAN_STEP):
            address -= SCAN_STEP
    if address >= SCAN_END:
        raise RuntimeError("mmap() refused no address below the scan limit")
    start = boundary(address - SCAN_STEP, address, False)
    while refused(address):
        address += SCAN_STEP
        if address >= SCAN_END:
            raise RuntimeError("the native window reaches the scan limit")
    end = boundary(address - SCAN_STEP, address, True)
    return start, end


def mappings() -> list[tuple[int, int]]:
    with open("/proc/self/maps", encoding="ascii") as maps:
        return [tuple(int(value, 16) for value in line.split()[0].split("-"))
                for line in maps]


def overlapping(start: int, end: int) -> list[str]:
    return [f"{first:#x}-{last:#x}" for first, last in mappings()
            if first < end and last > start]


def populate(address: int) -> bool:
    if map_page(address) != 0:
        return False
    ctypes.memset(address, 0x5a, PAGE)
    return True


def unmapped(address: int) -> bool:
    return not overlapping(address, address + PAGE)


def main() -> int:
    window_start, window_end = find_window()
    below = window_start - PAGE
    above = window_end
    print(f"window {window_start:#x}-{window_end:#x}, page {PAGE}, "
          f"table span {TABLE_SPAN:#x}", flush=True)
    # Each side's page table must map nothing else, or unmapping the probe's
    # page frees no table and the check proves nothing.
    for name, first, last in (
            ("below", window_start - TABLE_SPAN, window_start),
            ("above", window_end, window_end + TABLE_SPAN)):
        occupied = overlapping(first, last)
        check(f"{name} table unused", not occupied,
              f"{first:#x}-{last:#x} holds {occupied}")
    if failures:
        return failures

    # The page table that maps the first page above the window starts inside
    # the window whenever its end is not table-aligned.
    ok = populate(above)
    check("map above", ok, f"{above:#x}")
    if ok:
        check("unmap above", unmap(above, PAGE) == 0 and unmapped(above),
              f"{above:#x}")

    ok = populate(below)
    check("map below", ok, f"{below:#x}")
    if ok:
        check("unmap below", unmap(below, PAGE) == 0 and unmapped(below),
              f"{below:#x}")

    ok = populate(below) and populate(above)
    check("map both sides", ok, f"{below:#x} and {above:#x}")
    if ok:
        length = above + PAGE - below
        check("unmap across", unmap(below, length) == 0 and
              unmapped(below) and unmapped(above),
              f"{below:#x}+{length:#x}")

    # A fresh page at the same address must not show the unmapped one.
    ok = map_page(above) == 0
    check("remap above", ok, f"{above:#x}")
    if ok:
        check("fresh page", ctypes.string_at(above, PAGE) == bytes(PAGE),
              f"{above:#x} reads zeroes")
        unmap(above, PAGE)
    return failures


if __name__ == "__main__":
    sys.exit(main())
