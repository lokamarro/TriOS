#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot a guest on an Android release's disks and a blank data disk.

The release is the small one tests/support/android/android_release_fixture.py
assembles with the producer's own tools. The host creates the data disk with
the library both launchers use, and the guest, the Linux kernel on the test
root filesystem, sees exactly the layout the release describes:

- the system disk is read-only, holds the layout's partitions at the layout's
  offsets and sizes, each partition hashes to the layout's sha256, and the
  bytes after its image are zero;
- the data disk is blank: its partitions are the template's for the capacity,
  `misc` is zero, `metadata` and `userdata` carry no filesystem, and the queue
  accepts discards;
- the guest can format and use `metadata` and `userdata` itself, as Android's
  first boot does;
- after the guest has run, the library still accepts both disks, the system
  disk is the release's bytes, and the data disk is still sparse;
- the host grows the data disk, a second guest sees the larger table with the
  first guest's file intact, grows the filesystem into the new space, and
  the library refuses to shrink the disk.

The contract is darwin/device/block/README.md.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402

FIXTURE = REPOSITORY / "tests" / "support" / "android" / "android_release_fixture.py"
FIRST_GIB = 4
GROWN_GIB = 5
SECTOR = 512
GIB = 1 << 30
# The guest names its disks in the order of the arguments: the root filesystem
# is ubd0, the system disk ubd1 and the data disk ubd2.
SYSTEM_DEVICE = "ubdb"
DATA_DEVICE = "ubdc"
PATTERN_BYTES = 8 << 20
PATTERN_LINE = "ANDROID-DISK-PATTERN"
MOUNT = "/mnt/userdata"
# Hosts keep a sparse 4 GiB data disk far below its size after a mkfs.
SPARSE_LIMIT_BYTES = 512 << 20
GPT_WARNING = re.compile(
    r"GPT:|Alternate GPT|Primary GPT|invalid partition|corrupt", re.IGNORECASE
)


class Failures:
    def __init__(self) -> None:
        self.lines: list[str] = []

    def expect(self, condition: bool, message: str) -> None:
        if not condition:
            self.lines.append(message)

    def raise_if_any(self, boot: str) -> None:
        if self.lines:
            raise RuntimeError(
                f"{boot}: " + "; ".join(self.lines)
            )


def tool(disk_tool: pathlib.Path, *arguments: object) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(disk_tool), *map(str, arguments)],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=120,
    )


def tool_must_succeed(disk_tool: pathlib.Path, *arguments: object) -> dict[str, str]:
    result = tool(disk_tool, *arguments)
    if result.returncode != 0:
        raise RuntimeError(
            f"android_disk_tool {arguments[0]} failed ({result.returncode}): "
            f"{result.stderr.strip()}"
        )
    return dict(line.split("=", 1) for line in result.stdout.splitlines())


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def data_expectation(layout: dict, gib: int) -> tuple[int, dict[str, tuple[int, int]]]:
    """Sectors of the data disk and each partition's (start, size) sectors."""
    data = layout["data_disk"]
    total = gib * GIB
    end = total - data["trailing_free_bytes"]
    partitions = {}
    for partition in data["partitions"]:
        size = partition["size_bytes"]
        if size is None:
            size = end - partition["start_bytes"]
        partitions[partition["name"]] = (
            partition["start_bytes"] // SECTOR,
            size // SECTOR,
        )
    return total // SECTOR, partitions


def system_expectation(layout: dict) -> tuple[int, dict[str, dict]]:
    system = layout["system_disk"]
    return system["size_bytes"] // SECTOR, {
        partition["name"]: partition for partition in system["partitions"]
    }


def partitions_of(guest: Guest, device: str) -> dict[str, tuple[str, int, int]]:
    """name -> (device node, start sector, size in sectors), from sysfs."""
    output = guest.check_command(
        f"for p in /sys/block/{device}/{device}[0-9]*; do n=${{p##*/}}; "
        'printf "%s %s %s %s\\n" "$n" "$(cat $p/start)" "$(cat $p/size)" '
        '"$(sed -n s/^PARTNAME=//p $p/uevent)"; done',
        30,
    )
    found = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[0].startswith(device):
            found[fields[3]] = (fields[0], int(fields[1]), int(fields[2]))
    return found


def sysfs_number(guest: Guest, path: str) -> int:
    return int(guest.check_command(f"cat {path}", 30).strip().splitlines()[-1])


def check_system_disk(guest: Guest, layout: dict, failures: Failures) -> None:
    sectors, expected = system_expectation(layout)
    failures.expect(
        sysfs_number(guest, f"/sys/block/{SYSTEM_DEVICE}/size") == sectors,
        "the system disk has the wrong size",
    )
    failures.expect(
        sysfs_number(guest, f"/sys/block/{SYSTEM_DEVICE}/ro") == 1,
        "the system disk is not read-only",
    )
    found = partitions_of(guest, SYSTEM_DEVICE)
    failures.expect(
        set(found) == set(expected),
        f"the system disk's partitions are {sorted(found)}, "
        f"expected {sorted(expected)}",
    )
    for name, partition in expected.items():
        if name not in found:
            continue
        node, start, size = found[name]
        failures.expect(
            (start, size)
            == (partition["start_bytes"] // SECTOR, partition["size_bytes"] // SECTOR),
            f"{name} is at sector {start} for {size}",
        )
        image = partition["image_bytes"]
        digest = guest.check_command(
            f"head -c {partition['size_bytes']} /dev/{node} | sha256sum", 60
        ).split()[0]
        failures.expect(digest == partition["sha256"], f"{name} hashes to {digest}")
        stray = guest.check_command(
            f"tail -c +{image + 1} /dev/{node} | tr -d '\\0' | wc -c", 60
        ).strip().splitlines()[-1]
        failures.expect(stray == "0", f"{name} has {stray} nonzero bytes of padding")
    node = found.get("system", ("", 0, 0))[0]
    if node:
        written = guest.run_command(
            f"dd if=/dev/zero of=/dev/{node} bs=4096 count=1 conv=notrunc "
            "oflag=direct",
            30,
        )
        failures.expect(
            written.return_code != 0, "a write to the system disk succeeded"
        )


def check_data_disk(
    guest: Guest, layout: dict, gib: int, failures: Failures
) -> dict[str, str]:
    sectors, expected = data_expectation(layout, gib)
    failures.expect(
        sysfs_number(guest, f"/sys/block/{DATA_DEVICE}/size") == sectors,
        f"the data disk is not {gib} GiB",
    )
    failures.expect(
        sysfs_number(guest, f"/sys/block/{DATA_DEVICE}/ro") == 0,
        "the data disk is read-only",
    )
    failures.expect(
        sysfs_number(guest, f"/sys/block/{DATA_DEVICE}/queue/discard_max_bytes") > 0,
        "the data disk does not take discards",
    )
    found = partitions_of(guest, DATA_DEVICE)
    failures.expect(
        set(found) == set(expected),
        f"the data disk's partitions are {sorted(found)}, expected {sorted(expected)}",
    )
    nodes = {}
    for name, (start, size) in expected.items():
        if name not in found:
            continue
        node, found_start, found_size = found[name]
        nodes[name] = node
        failures.expect(
            (found_start, found_size) == (start, size),
            f"{name} is at sector {found_start} for {found_size}, "
            f"expected {start} for {size}",
        )
    return nodes


def check_blank(guest: Guest, nodes: dict[str, str], failures: Failures) -> None:
    for name, node in nodes.items():
        stray = guest.check_command(
            f"head -c 4096 /dev/{node} | tr -d '\\0' | wc -c", 30
        ).strip().splitlines()[-1]
        failures.expect(stray == "0", f"the blank {name} holds {stray} nonzero bytes")
    for name in ("metadata", "userdata"):
        signature = guest.run_command(
            f"blkid -p -s TYPE -o value /dev/{nodes[name]}", 30
        )
        failures.expect(
            signature.output.strip() == "",
            f"the blank {name} carries a {signature.output.strip()} signature",
        )


def format_and_write(guest: Guest, nodes: dict[str, str]) -> str:
    guest.check_command(f"mkfs.ext4 -q -F -L at-metadata /dev/{nodes['metadata']}", 120)
    guest.check_command(f"mkfs.ext4 -q -F -L at-userdata /dev/{nodes['userdata']}", 300)
    guest.check_command(
        f"mkdir -p {MOUNT} && mount -t ext4 /dev/{nodes['userdata']} {MOUNT}", 60
    )
    guest.check_command(
        f"yes {PATTERN_LINE} | head -c {PATTERN_BYTES} > {MOUNT}/pattern && sync", 120
    )
    digest = guest.check_command(f"sha256sum {MOUNT}/pattern", 60).split()[0]
    guest.check_command(f"umount {MOUNT}", 60)
    guest.check_command(f"e2fsck -fn /dev/{nodes['userdata']}", 120)
    guest.check_command(f"e2fsck -fn /dev/{nodes['metadata']}", 60)
    return digest


def grow_and_verify(guest: Guest, nodes: dict[str, str], digest: str) -> int:
    node = nodes["userdata"]
    guest.check_command(f"e2fsck -fp /dev/{node}", 120)
    guest.check_command(f"resize2fs /dev/{node}", 300)
    guest.check_command(f"mkdir -p {MOUNT} && mount -t ext4 /dev/{node} {MOUNT}", 60)
    kept = guest.check_command(f"sha256sum {MOUNT}/pattern", 60).split()[0]
    if kept != digest:
        raise RuntimeError("the first guest's file changed across the grow")
    block_size, blocks = guest.check_command(
        f"stat -f -c '%S %b' {MOUNT}", 30
    ).split()[-2:]
    capacity = int(block_size) * int(blocks)
    guest.check_command(f"umount {MOUNT}", 60)
    guest.check_command(f"e2fsck -fn /dev/{node}", 120)
    return capacity


def boot(
    emulator: pathlib.Path,
    rootfs: pathlib.Path,
    system_disk: str,
    data_disk: str,
    engine: str,
) -> Guest:
    return Guest(
        emulator,
        rootfs,
        "1G",
        "tmpfs",
        ("ncpus=1", f"ubd1r={system_disk}", f"ubd2={data_disk}"),
        init_shell=True,
        engine=engine,
    )


def power_off(guest: Guest, label: str) -> None:
    if not guest.shutdown(60):
        raise RuntimeError(f"{label}: the guest did not power off")
    if guest.return_code != 0:
        raise RuntimeError(
            f"{label}: the emulator exited with status {guest.return_code}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--disk-tool", required=True, type=pathlib.Path)
    parser.add_argument("--android-root", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    emulator = arguments.emulator.resolve()
    rootfs = arguments.rootfs.resolve()
    disk_tool = arguments.disk_tool.resolve()

    with tempfile.TemporaryDirectory(prefix="uml-android-disks-") as scratch:
        root = pathlib.Path(scratch).resolve()
        subprocess.run(
            [
                sys.executable, "-B", str(FIXTURE),
                "--android-root", str(arguments.android_root.resolve()),
                "--output", str(root / "fixture"),
            ],
            check=True,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            timeout=120,
        )
        release = root / "fixture" / "release"
        data_path = root / "instance" / "data.img"
        data_path.parent.mkdir()
        layout = json.loads((release / "guest/partition-layout.json").read_text())
        system_hash = layout["system_disk"]["sha256"]

        disks = tool_must_succeed(disk_tool, "prepare", release, data_path, FIRST_GIB)
        if disks["capacity-gib"] != str(FIRST_GIB):
            raise RuntimeError(f"the new data disk is {disks['capacity-gib']} GiB")
        system_disk, data_disk = disks["system-disk"], disks["data-disk"]
        if sha256_file(pathlib.Path(system_disk)) != system_hash:
            raise RuntimeError("the release's system disk is not the layout's")

        with boot(emulator, rootfs, system_disk, data_disk, arguments.engine) as guest:
            guest.boot_to_shell(180)
            failures = Failures()
            check_system_disk(guest, layout, failures)
            nodes = check_data_disk(guest, layout, FIRST_GIB, failures)
            failures.raise_if_any("first boot")
            check_blank(guest, nodes, failures)
            digest = format_and_write(guest, nodes)
            warnings = GPT_WARNING.findall(guest.output())
            failures.expect(not warnings, f"the kernel reported {warnings}")
            failures.raise_if_any("first boot")
            power_off(guest, "first boot")
        print(f"first boot: {FIRST_GIB} GiB blank data disk used, file {digest[:12]}")

        tool_must_succeed(disk_tool, "check", release, data_path)
        if sha256_file(pathlib.Path(system_disk)) != system_hash:
            raise RuntimeError("the guest changed the system disk")
        allocated = os.stat(data_path).st_blocks * SECTOR
        if allocated > SPARSE_LIMIT_BYTES:
            raise RuntimeError(
                f"the data disk holds {allocated} bytes of its {FIRST_GIB} GiB"
            )
        shrink = tool(disk_tool, "prepare", release, data_path, FIRST_GIB - 1)
        if shrink.returncode == 0 or os.stat(data_path).st_size != FIRST_GIB * GIB:
            raise RuntimeError("the library accepted a smaller data disk")
        tool_must_succeed(disk_tool, "prepare", release, data_path, GROWN_GIB)
        if os.stat(data_path).st_size != GROWN_GIB * GIB:
            raise RuntimeError("the data disk did not grow")
        print(f"host: guest left both disks valid; grown to {GROWN_GIB} GiB; shrink refused")

        with boot(emulator, rootfs, system_disk, data_disk, arguments.engine) as guest:
            guest.boot_to_shell(180)
            failures = Failures()
            check_system_disk(guest, layout, failures)
            nodes = check_data_disk(guest, layout, GROWN_GIB, failures)
            failures.raise_if_any("second boot")
            grown = grow_and_verify(guest, nodes, digest)
            warnings = GPT_WARNING.findall(guest.output())
            failures.expect(not warnings, f"the kernel reported {warnings}")
            failures.expect(
                grown > (GROWN_GIB - 1) * GIB,
                f"the filesystem is {grown} bytes after the grow",
            )
            failures.raise_if_any("second boot")
            power_off(guest, "second boot")
        tool_must_succeed(disk_tool, "check", release, data_path)
        print(f"second boot: file kept; filesystem grown to {grown >> 20} MiB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
