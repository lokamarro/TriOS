#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Evict ext4 inodes that hold preallocations while the guest is under memory pressure.

An inode shrinker that evicts a cached ext4 inode releases the inode's block
preallocation, and the release must not allocate memory: it runs under
PF_MEMALLOC, where the page allocator warns about the __GFP_NOFAIL request
that reloading an uncached buddy folio or bitmap buffer would make. While a
block group holds an inode preallocation the kernel keeps the group's bitmap
and buddy folios and its bitmap buffer referenced, so the release finds them
in memory. The contract is the ext4 paragraph of kernel/common/README.md.

The guest formats an ext4 disk with fs-verity, which leaves an inode
preallocation on every closed file it protects, writes thousands of files,
drops the clean caches and allocates anonymous memory until reclaim evicts
the inodes (ext4_prealloc_reclaim_probe.py). Tracepoints must then show
inode preallocations released from reclaim, with no __GFP_NOFAIL page
allocation made by the release. The filesystem is unmounted and checked, its
free block count must not change, and nothing may print a kernel warning,
oops or filesystem error. Every `--block-size` is a round; sizes below the
4 KiB page put several groups in one buddy-cache folio.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import re
import sys
import tempfile

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402


VCPUS = 4
DISK_BYTES = 1 << 30
PROBE = "/opt/uml-test/ext4_prealloc_reclaim_probe.py"
PROBE_SECONDS = 900
# The release is measured by the preallocations reclaim released; fewer means
# the pressure never reached the inodes and the round proved nothing.
MINIMUM_RELEASES = 200
FAILURE = re.compile(
    r"WARNING:|BUG:|Oops|Call Trace:|EXT4-fs (error|warning)|Kernel panic|"
    r"Darwin UML panic|Darwin POSIX|Kernel mode fault"
)
RESULT = re.compile(
    r"^EXT4_PA_(RECLAIM|FREE|CONTENT|FSCK) (.*)$", re.MULTILINE
)


def parse(output: str) -> dict[str, dict[str, int]]:
    results: dict[str, dict[str, int]] = {}
    for match in RESULT.finditer(output):
        results[match[1]] = {
            key: int(value)
            for key, value in (field.split("=") for field in match[2].split())
        }
    return results


def round_trip(
    emulator: pathlib.Path,
    rootfs: pathlib.Path,
    engine: str,
    block_size: int,
) -> None:
    probe = (
        REPOSITORY / "tests/integration/kernel/ext4_prealloc_reclaim_probe.py"
    ).read_bytes()
    with tempfile.TemporaryDirectory(prefix="uml-ext4-prealloc-") as scratch:
        image = pathlib.Path(scratch) / "disk.img"
        with open(image, "wb") as handle:
            handle.truncate(DISK_BYTES)
        with Guest(
            emulator,
            rootfs,
            "512M",
            "tmpfs",
            (f"ncpus={VCPUS}", f"ubd1={image}"),
            init_shell=True,
            engine=engine,
            payloads={PROBE: GuestFile(probe, 0o755)},
        ) as guest:
            guest.boot_to_shell(180)
            guest.check_command("mount -t tmpfs -o mode=1777 tmpfs /tmp", 20)
            mark = len(guest.output())
            output = guest.check_command(
                f"python3 {PROBE} /dev/ubdb {block_size}", PROBE_SECONDS
            )
            results = parse(output)
            for name in ("RECLAIM", "FREE", "CONTENT", "FSCK"):
                if name not in results:
                    raise RuntimeError(
                        f"block size {block_size}: the probe printed no "
                        f"EXT4_PA_{name} line:\n" + output[-4000:]
                    )
            reclaim = results["RECLAIM"]
            if reclaim["releases"] < MINIMUM_RELEASES:
                raise RuntimeError(
                    f"block size {block_size}: reclaim released only "
                    f"{reclaim['releases']} inode preallocations "
                    f"(at least {MINIMUM_RELEASES} expected):\n" + output[-4000:]
                )
            if reclaim["nofail_in_release"] != 0:
                raise RuntimeError(
                    f"block size {block_size}: releasing an inode "
                    f"preallocation allocated {reclaim['nofail_in_release']} "
                    f"times with __GFP_NOFAIL at reclaim time:\n" + output[-4000:]
                )
            if results["FREE"]["before"] != results["FREE"]["after"]:
                raise RuntimeError(
                    f"block size {block_size}: the free block count moved "
                    f"from {results['FREE']['before']} to "
                    f"{results['FREE']['after']}"
                )
            if results["CONTENT"]["mismatches"] != 0:
                raise RuntimeError(
                    f"block size {block_size}: "
                    f"{results['CONTENT']['mismatches']} files read back wrong"
                )
            if results["FSCK"]["rc"] != 0:
                raise RuntimeError(
                    f"block size {block_size}: e2fsck -fn failed:\n" + output[-4000:]
                )
            failure = FAILURE.search(guest.output()[mark:])
            if failure is not None:
                tail = guest.output()[mark:]
                raise RuntimeError(
                    f"block size {block_size}: the guest printed "
                    f"{failure[0]!r}:\n" + tail[max(0, failure.start() - 2000):][:8000]
                )
            if not guest.shutdown(60) or guest.return_code != 0:
                raise RuntimeError(
                    f"block size {block_size}: the guest did not power off cleanly"
                )
            print(
                f"block size {block_size}: {reclaim['releases']} inode "
                f"preallocations released at reclaim time, "
                f"{reclaim['nofail']} NOFAIL allocations there, none from the "
                f"release; fsck clean"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument(
        "--block-size", type=int, action="append", choices=(1024, 2048, 4096)
    )
    arguments = parser.parse_args()
    for block_size in arguments.block_size or (4096, 1024):
        round_trip(
            arguments.emulator.resolve(),
            arguments.rootfs.resolve(),
            arguments.engine,
            block_size,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
