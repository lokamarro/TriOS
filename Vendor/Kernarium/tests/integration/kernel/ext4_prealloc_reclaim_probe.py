#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guest side of guest-ext4-prealloc-reclaim: evict inodes that hold preallocations.

Usage: ext4_prealloc_reclaim_probe.py DEVICE BLOCK_SIZE

Formats DEVICE as ext4 with fs-verity, which writes a file's Merkle tree
past its end without delayed allocation and so leaves an inode
preallocation on a closed inode, writes and verifies many files, drops the
clean caches and applies anonymous memory pressure until reclaim evicts the
inodes. The pressure stops a reserve short of the point where the page
allocator gives up and kills, with watermark boosting off so that point stays
put. Each unit of it is a private anonymous mapping, so dropping one returns
its pages at once; freed malloc memory can stay in the heap and hold the
guest at that point with nothing left to release. Tracepoints count the inode
preallocations released from a shrinker or reclaim stack and the
__GFP_NOFAIL page allocations on that path, and the filesystem is checked
after it is unmounted.

Prints one line per result:
  EXT4_PA_RECLAIM releases=N nofail=M nofail_in_release=K
  EXT4_PA_FREE before=N after=N
  EXT4_PA_CONTENT mismatches=N
  EXT4_PA_FSCK rc=N
"""

import collections
import fcntl
import hashlib
import mmap
import os
import re
import struct
import subprocess
import sys
import time

TRACING = "/sys/kernel/tracing"
BOOST_FACTOR = "/proc/sys/vm/watermark_boost_factor"
DIRECTORIES = 8
FILES = 300
SIZE = 300000
# Headroom kept above the out-of-memory point.
RESERVE_KB = 8192
CHUNK = 2 << 20
CHURN_SECONDS = 30
ENABLE_VERITY = 0x40806685
NOFAIL = 0x8000
SHRINKER_FRAMES = (
    "shrink_slab",
    "balance_pgdat",
    "try_to_free_pages",
    "shrink_node",
    "super_cache_scan",
)


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


def headroom_kb() -> int:
    """Free and file pages above the minimum watermark, in KiB.

    The page allocator goes out of memory once free and reclaimable pages,
    which without swap are the file pages, no longer clear the minimum
    watermark (should_reclaim_retry() in mm/page_alloc.c). The inode shrinker
    evicts in bulk only when kswapd cannot reach the high watermark, a few
    megabytes above that point. A watermark boost would lift the minimum past
    it, by up to 1.5 times the high watermark, until kswapd next balanced the
    node, so pressure() turns boosting off."""
    wanted = ("min", "nr_zone_active_file", "nr_zone_inactive_file")
    zone: dict[str, int] = {}
    with open("/proc/zoneinfo") as zoneinfo:
        normal = False
        for line in zoneinfo:
            if line.startswith("Node"):
                normal = line.split()[-1] == "Normal"
                continue
            fields = line.split()
            if not normal:
                continue
            if fields[:2] == ["pages", "free"]:
                zone["free"] = int(fields[2])
            elif len(fields) == 2 and fields[0] in wanted:
                zone.setdefault(fields[0], int(fields[1]))
    pages = (zone["free"] + zone["nr_zone_active_file"]
             + zone["nr_zone_inactive_file"] - zone["min"])
    return pages * mmap.PAGESIZE // 1024


def start_tracing() -> None:
    run(f"mountpoint -q {TRACING} || mount -t tracefs nodev {TRACING}")
    write(f"{TRACING}/tracing_on", "0")
    write(f"{TRACING}/trace", "")
    write(f"{TRACING}/buffer_size_kb", "16384")
    page_alloc = f"{TRACING}/events/kmem/mm_page_alloc"
    write(f"{page_alloc}/filter", f"gfp_flags & {NOFAIL:#x}")
    write(f"{page_alloc}/enable", "1")
    write(f"{TRACING}/events/ext4/ext4_mb_release_inode_pa/enable", "1")
    write(f"{TRACING}/options/stacktrace", "1")
    write(f"{TRACING}/tracing_on", "1")


def trace_counts() -> tuple[int, int, int]:
    """Releases at reclaim time, NOFAIL allocations at reclaim time, and the
    NOFAIL allocations made by a preallocation release at reclaim time."""
    write(f"{TRACING}/tracing_on", "0")
    event = re.compile(r"^\s*\S+-\d+\s+\[\d+\]\s+\S+\s+[\d.]+:\s+(\w+):")
    events: list[tuple[str, list[str]]] = []
    with open(f"{TRACING}/trace", errors="replace") as trace:
        for line in trace:
            match = event.match(line)
            if match:
                events.append((match.group(1), []))
            elif events and "=>" in line:
                events[-1][1].append(line.split("=>", 1)[1].strip())

    def in_reclaim(stack: list[str]) -> bool:
        return any(frame in entry for entry in stack for frame in SHRINKER_FRAMES)

    releases = sum(
        1 for name, stack in events
        if name == "ext4_mb_release_inode_pa" and in_reclaim(stack)
    )
    nofail = [stack for name, stack in events
              if name == "mm_page_alloc" and in_reclaim(stack)]
    in_release = sum(
        1 for stack in nofail
        if any("ext4_discard_preallocations" in entry for entry in stack)
    )
    return releases, len(nofail), in_release


def pressure() -> None:
    """Allocate anonymous memory down to the reserve, then churn at its edge.

    Watermark boosting is off meanwhile. Every chunk is unmapped when it is
    dropped, and all of them before this returns."""
    chunks: list[mmap.mmap] = []
    start = time.time()
    reached = None
    with open(BOOST_FACTOR) as handle:
        boost_factor = handle.read()
    write(BOOST_FACTOR, "0")
    try:
        while time.time() - start < CHURN_SECONDS + 60:
            if headroom_kb() > RESERVE_KB:
                chunk = mmap.mmap(-1, CHUNK, flags=mmap.MAP_PRIVATE)
                for offset in range(0, CHUNK, mmap.PAGESIZE):
                    chunk[offset] = 1
                chunks.append(chunk)
                time.sleep(0.004)
                continue
            reached = reached or time.time()
            if time.time() - reached > CHURN_SECONDS:
                break
            for _ in range(max(1, len(chunks) // 16)):
                if chunks:
                    chunks.pop().close()
    finally:
        for chunk in chunks:
            chunk.close()
        write(BOOST_FACTOR, boost_factor)


def main() -> int:
    device, block_size = sys.argv[1], int(sys.argv[2])
    run("umount /mnt 2>/dev/null")
    status, output = run(
        f"mkfs.ext4 -F -q -E nodiscard -O verity -b {block_size} {device}"
    )
    if status:
        print(output)
        return 2
    status, output = run(f"mount {device} /mnt")
    if status:
        print(output)
        return 2
    start_tracing()

    digests = {}
    for directory in range(DIRECTORIES):
        os.mkdir(f"/mnt/d{directory}")
        for index in range(FILES):
            data = bytes([(directory * 31 + index) & 0xFF]) * SIZE
            path = f"/mnt/d{directory}/f{index}"
            with open(path, "wb") as handle:
                handle.write(data)
            digests[path] = hashlib.sha256(data).hexdigest()
    run("sync")
    argument = struct.pack("=IIIIQIIQ88s", 1, 1, block_size, 0, 0, 0, 0, 0, bytes(88))
    for path in digests:
        descriptor = os.open(path, os.O_RDONLY)
        try:
            fcntl.ioctl(descriptor, ENABLE_VERITY, argument)
        finally:
            os.close(descriptor)
    run("sync")
    before = os.statvfs("/mnt").f_bfree
    write("/proc/sys/vm/drop_caches", "1")

    pressure()
    releases, nofail, in_release = trace_counts()
    print(f"EXT4_PA_RECLAIM releases={releases} nofail={nofail} "
          f"nofail_in_release={in_release}")

    run("sync")
    after = os.statvfs("/mnt").f_bfree
    print(f"EXT4_PA_FREE before={before} after={after}")
    mismatches = 0
    for path, digest in digests.items():
        with open(path, "rb") as handle:
            if hashlib.sha256(handle.read()).hexdigest() != digest:
                mismatches += 1
    print(f"EXT4_PA_CONTENT mismatches={mismatches}")
    status, output = run("umount /mnt")
    if status:
        print(output)
        return 2
    status, output = run(f"e2fsck -fn {device}")
    print(f"EXT4_PA_FSCK rc={status}")
    if status:
        print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
