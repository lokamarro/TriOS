#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot the immutable x86-64 Steam appliance with a retained GPT user-data disk."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import fcntl
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

NEW_DISK = b"KERNARIUM NEW USER DATA v1\n"
# The x86-64 guest has one execution engine, named explicitly so the launch
# contract never depends on the emulator's default. Virtio sound plays through
# the Apple audio output.
OPTIONS = ("--execution-engine=gadgets", "--audio")
# The launcher owns the storage, init, console and memory arguments and its own
# emulator options, so a replacement rootfs or data disk can never be selected
# from the command line.
RESERVED = {"root", "rootfstype", "init", "mem", "console", "systemd.unit",
            *(option.split("=", 1)[0] for option in OPTIONS)}


def disk_command(*args: str) -> str:
    return subprocess.run(["sgdisk", *args], text=True, check=True, capture_output=True,
                          env={**os.environ, "LC_ALL": "C"}, timeout=30).stdout


def prepare_disk(path: Path, size_gib: int) -> None:
    """Publish a new blank partition once; never format or replace an existing disk."""
    if not 4 <= size_gib <= 2048:
        raise ValueError("user-data capacity must be between 4 and 2048 GiB")
    if path.is_symlink():
        raise ValueError("user-data disk must not be a symlink")
    if not path.exists():
        descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
        staged = Path(temporary)
        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.truncate(size_gib << 30)
            disk_command("--clear", "--new=1:2048:0", "--typecode=1:8300",
                         "--change-name=1:kernarium-data", str(staged))
            with staged.open("r+b") as stream:
                stream.seek((2048 * 512) + 1024)
                stream.write(NEW_DISK)
                stream.flush()
                os.fsync(stream.fileno())
            os.link(staged, path)
        finally:
            staged.unlink(missing_ok=True)
    if not path.is_file():
        raise ValueError("user-data disk must be a regular file")
    verified = disk_command("--verify", str(path))
    table = disk_command("--print", str(path))
    partitions = re.findall(r"^\s+\d+\s+\d+\s+\d+.*$", table, re.M)
    if "No problems found" not in verified or len(partitions) != 1 or not re.search(
            r"^\s+1\s+2048\s+\d+\s+.*\s8300\s+kernarium-data\s*$", partitions[0]):
        raise ValueError("user-data GPT is invalid; preserve the disk and repair it before booting")


def command(emulator: Path, rootfs: Path, userdata: Path, memory: str, unit: str,
            extra: list[str]) -> list[str]:
    for argument in extra:
        if argument.split("=", 1)[0] in RESERVED or argument.startswith("ubd"):
            raise ValueError(f"launcher controls this argument: {argument}")
    return [str(emulator), *OPTIONS, f"ubd0rc={rootfs}", f"ubd1={userdata}", "root=/dev/ubda",
            "rootfstype=erofs", "ro", "init=/usr/local/libexec/kernarium-init",
            f"mem={memory}", "console=ttyS0", "ip=off", "quiet", "loglevel=3",
            "systemd.show_status=false", f"systemd.unit={unit}", *extra]


def parser() -> argparse.ArgumentParser:
    parsed = argparse.ArgumentParser(description=__doc__)
    parsed.add_argument("--emulator", required=True, type=Path)
    parsed.add_argument("--rootfs", required=True, type=Path)
    parsed.add_argument("--userdata", required=True, type=Path,
                        help="persistent disk, independent of the rootfs version")
    parsed.add_argument("--size-gib", type=int, default=32, help="capacity on first creation only")
    parsed.add_argument("--memory", default="8G")
    parsed.add_argument("--unit", default="graphical.target")
    parsed.add_argument("--dry-run", action="store_true")
    parsed.add_argument("extra", nargs=argparse.REMAINDER)
    return parsed


def launch(args: argparse.Namespace) -> int:
    extra = args.extra[1:] if args.extra[:1] == ["--"] else args.extra
    disk = args.userdata.absolute()
    argv = command(args.emulator.resolve(strict=True), args.rootfs.resolve(strict=True),
                   disk, args.memory, args.unit, extra)
    if args.dry_run:
        print(shlex.join(argv))
        return 0
    disk.parent.mkdir(parents=True, exist_ok=True)
    with Path(str(disk) + ".lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        prepare_disk(disk, args.size_gib)
        # Retain the lock until the emulator has released the persistent disk.
        process = subprocess.Popen(argv)
        try:
            return process.wait()
        except KeyboardInterrupt:
            try:
                return process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    return process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    return process.wait(timeout=10)


def main() -> int:
    return launch(parser().parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
