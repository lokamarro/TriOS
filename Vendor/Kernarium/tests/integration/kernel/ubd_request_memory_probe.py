#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guest side of guest-ubd-request-memory: UBD I/O with every allocation traced.

Usage: ubd_request_memory_probe.py DEVICE

Traces slab and non-sleeping page allocations with their stacks while it
drives DEVICE, a UBD disk it may overwrite, with direct writes and reads of
one segment up to the disk's 64, a split 1 MiB transfer, buffered writes
that writeback submits and a flush ends, a discard and a write-zeroes, and
checks what it reads back. An allocation is the driver's when a UBD frame
lies nearer to it than any completion frame: what an upper layer allocates
while a UBD completion runs is that layer's own.

Prints one line per result:
  UBD_REQUESTS reads=N writes=N discards=N flushes=N
  UBD_ALLOCATIONS events=N submissions=N driver=N lost=N
  UBD_DRIVER_STACK <frames>     (for at most three driver allocations)
  UBD_DATA mismatches=N
"""

import fcntl
import mmap
import os
import re
import struct
import subprocess
import sys

TRACING = "/sys/kernel/tracing"
PAGE = 4096
SECTOR = 512
MAX_SEGMENTS = 64
BLKDISCARD = 0x1277
BLKZEROOUT = 0x127F
# __GFP_DIRECT_RECLAIM: a page allocation that may not sleep lacks it, as
# every allocation a blk-mq ->queue_rq() makes must.
DIRECT_RECLAIM = 0x400
DRIVER_FRAME = re.compile(r"^(ubd_|darwin_ubd_|do_io\b)")
COMPLETION_FRAME = re.compile(
    r"^(blk_mq_end_request|__blk_mq_end_request|blk_update_request|bio_endio)\b"
)
EVENT = re.compile(r"^\s*\S+-\d+\s+\[\d+\]\s+\S+\s+[\d.]+:\s+(\w+):")


def run(command: str) -> tuple[int, str]:
    result = subprocess.run(
        command,
        shell=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return result.returncode, result.stdout


def write(path: str, text: str) -> None:
    with open(path, "w") as handle:
        handle.write(text)


def start_tracing() -> None:
    run(f"mountpoint -q {TRACING} || mount -t tracefs nodev {TRACING}")
    write(f"{TRACING}/tracing_on", "0")
    write(f"{TRACING}/trace", "")
    write(f"{TRACING}/buffer_size_kb", "8192")
    for event in ("kmalloc", "kmem_cache_alloc"):
        write(f"{TRACING}/events/kmem/{event}/enable", "1")
    page_alloc = f"{TRACING}/events/kmem/mm_page_alloc"
    write(f"{page_alloc}/filter", f"!(gfp_flags & {DIRECT_RECLAIM:#x})")
    write(f"{page_alloc}/enable", "1")
    write(f"{TRACING}/options/stacktrace", "1")
    write(f"{TRACING}/tracing_on", "1")


def lost_events() -> int:
    lost = 0
    for cpu in os.listdir(f"{TRACING}/per_cpu"):
        with open(f"{TRACING}/per_cpu/{cpu}/stats") as stats:
            for line in stats:
                name, _, value = line.partition(":")
                if name.strip() in ("overrun", "dropped events"):
                    lost += int(value.split()[0])
    return lost


def allocations() -> tuple[int, int, list[list[str]], int]:
    """Traced events, events from the block submission path, the driver's
    allocations and the events the trace buffer lost."""
    write(f"{TRACING}/tracing_on", "0")
    lost = lost_events()
    events: list[list[str]] = []
    with open(f"{TRACING}/trace", errors="replace") as trace:
        for line in trace:
            if EVENT.match(line):
                events.append([])
            elif events and "=>" in line:
                events[-1].append(line.split("=>", 1)[1].strip())
    for event in ("kmalloc", "kmem_cache_alloc", "mm_page_alloc"):
        write(f"{TRACING}/events/kmem/{event}/enable", "0")
    write(f"{TRACING}/options/stacktrace", "0")

    def driver(stack: list[str]) -> bool:
        for frame in stack:
            if COMPLETION_FRAME.match(frame):
                return False
            if DRIVER_FRAME.match(frame):
                return True
        return False

    submissions = sum(
        1 for stack in events if any("blk_mq_submit_bio" in frame for frame in stack)
    )
    return len(events), submissions, [stack for stack in events if driver(stack)], lost


def statistics(device: str) -> list[int]:
    with open(f"/sys/block/{os.path.basename(device)}/stat") as stat:
        return [int(field) for field in stat.read().split()]


def buffer(size: int) -> mmap.mmap:
    """Page-aligned memory, as direct I/O needs."""
    return mmap.mmap(-1, size)


def scattered_round_trip(descriptor: int, offset: int, segments: int) -> int:
    """Write and read back one request of `segments` sectors, each in a page of
    its own, so the request has that many segments."""
    source = buffer(segments * PAGE)
    for index in range(segments):
        source[index * PAGE:index * PAGE + SECTOR] = os.urandom(SECTOR)
    vectors = [memoryview(source)[i * PAGE:i * PAGE + SECTOR] for i in range(segments)]
    if os.pwritev(descriptor, vectors, offset) != segments * SECTOR:
        return 1
    target = buffer(segments * PAGE)
    back = [memoryview(target)[i * PAGE:i * PAGE + SECTOR] for i in range(segments)]
    if os.preadv(descriptor, back, offset) != segments * SECTOR:
        return 1
    mismatched = sum(1 for a, b in zip(vectors, back) if bytes(a) != bytes(b))
    for view in vectors + back:
        view.release()
    source.close()
    target.close()
    return mismatched


def direct_read(descriptor: int, offset: int, length: int) -> bytes:
    target = buffer(length)
    if os.preadv(descriptor, [target], offset) != length:
        raise RuntimeError(f"short direct read at {offset}")
    data = bytes(target)
    target.close()
    return data


def main() -> int:
    device = sys.argv[1]
    before = statistics(device)
    start_tracing()
    mismatches = 0

    direct = os.open(device, os.O_RDWR | os.O_DIRECT)
    offset = 0
    for segments in (1, 2, 7, 32, MAX_SEGMENTS - 1, MAX_SEGMENTS):
        mismatches += scattered_round_trip(direct, offset, segments)
        offset += MAX_SEGMENTS * SECTOR

    # One 1 MiB transfer, which the block layer splits into several requests.
    large = 1 << 20
    pattern = os.urandom(large)
    source = buffer(large)
    source[:] = pattern
    offset = 1 << 20
    if os.pwritev(direct, [source], offset) != large:
        mismatches += 1
    source.close()
    if direct_read(direct, offset, large) != pattern:
        mismatches += 1

    # Buffered writes go through writeback; fsync() ends with a flush.
    buffered = os.open(device, os.O_RDWR)
    pattern = os.urandom(4 << 20)
    offset = 4 << 20
    if os.pwrite(buffered, pattern, offset) != len(pattern):
        mismatches += 1
    os.fsync(buffered)
    os.close(buffered)
    if direct_read(direct, offset, len(pattern)) != pattern:
        mismatches += 1

    offset = 16 << 20
    fcntl.ioctl(direct, BLKDISCARD, struct.pack("=QQ", offset, 1 << 20))
    fcntl.ioctl(direct, BLKZEROOUT, struct.pack("=QQ", offset, 1 << 20))
    if direct_read(direct, offset, 1 << 20) != bytes(1 << 20):
        mismatches += 1
    os.close(direct)

    events, submissions, driver, lost = allocations()
    after = statistics(device)
    print(
        f"UBD_REQUESTS reads={after[0] - before[0]} writes={after[4] - before[4]} "
        f"discards={after[11] - before[11]} flushes={after[15] - before[15]}"
    )
    print(
        f"UBD_ALLOCATIONS events={events} submissions={submissions} "
        f"driver={len(driver)} lost={lost}"
    )
    for stack in driver[:3]:
        print("UBD_DRIVER_STACK " + " ".join(stack[:12]))
    print(f"UBD_DATA mismatches={mismatches}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
