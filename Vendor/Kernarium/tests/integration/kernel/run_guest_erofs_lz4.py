#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read files back from an LZ4 EROFS image with small physical clusters.

EROFS decompresses an LZ4 cluster whose output spans several page-cache
folios through a vm_map_ram() alias of those folios, and readers then see
the folios through the linear map. Such an alias is a private host slot until
the kernel writes it back, which flush_cache_vunmap() does before the alias
goes (kernel/common/README.md). An image built as `mkfs.erofs
-zlz4hc`, with the default 4 KiB clusters and no big-cluster feature, takes
that path for most clusters.

The runner writes seeded, partly compressible files, builds such an image of
them on the host and checks it with fsck.erofs, then attaches it read-only as
the guest's second disk. The guest mounts it, and every file must read back
with its SHA-256, first and from the page cache, in each of several rounds
separated by dropping the page cache, so each round decompresses again. No
EROFS error, kernel warning or fault report may reach the console.

It works with the arm64 integration rootfs and, run
directly, with an x86-64 image on the x86_64-userspace slice. It needs
erofs-utils on the host.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import os
import pathlib
import random
import re
import shutil
import subprocess
import sys
import tempfile


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


VCPUS = 2
ROUNDS = 3
FILES = 64
SEED = 20261018
MOUNT = "/mnt/erofs-lz4"
TOOL_TIMEOUT_SECONDS = 300
# Homebrew's erofs-utils is not keg-only, but a PATH without the Homebrew
# prefix still finds it there.
TOOL_DIRECTORIES = ("/opt/homebrew/bin", "/usr/local/bin")
WORDS = (
    "erofs", "cluster", "folio", "alias", "linear", "page", "slot", "host",
    "guest", "kernel", "frame", "shadow", "write", "back", "vmap", "decompress",
)
KERNEL_FAILURE = re.compile(
    r"\[\s*\d+\.\d+\].*(?:erofs.*(?:error|corrupt|fail)|WARNING:|BUG:|"
    r"Oops|Call Trace:|Kernel panic)",
    re.IGNORECASE,
)
HOST_FAILURE = re.compile(r"Darwin UML panic|Darwin POSIX|Kernel mode fault")
SUM_LINE = re.compile(r"^([0-9a-f]{64})  (file-\d+)$")


def find_tool(name: str) -> str:
    found = shutil.which(name)
    if found is not None:
        return found
    for directory in TOOL_DIRECTORIES:
        candidate = pathlib.Path(directory, name)
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    raise SystemExit(f"{name} is not installed (erofs-utils provides it)")


def file_contents(generator: random.Random, size: int) -> bytes:
    """Words with runs of random bytes, as real files mix text and data.

    A 4 KiB cluster of it holds two to three times its size, so its output
    spans several folios."""
    data = bytearray()
    while len(data) < size:
        if generator.random() < 0.25:
            data += generator.randbytes(generator.randrange(64, 512))
        else:
            data += " ".join(
                generator.choice(WORDS)
                for _ in range(generator.randrange(8, 64))
            ).encode() + b"\n"
    return bytes(data[:size])


def build_image(directory: pathlib.Path) -> tuple[pathlib.Path, dict[str, str]]:
    """Write the source files and their LZ4 image; return it and the sums."""
    source = directory / "source"
    source.mkdir()
    generator = random.Random(SEED)
    sums = {}
    for index in range(FILES):
        name = f"file-{index:02d}"
        contents = file_contents(
            generator, generator.choice((3000, 9000, 70000, 260000, 600000))
        )
        (source / name).write_bytes(contents)
        sums[name] = hashlib.sha256(contents).hexdigest()
    image = directory / "lz4.erofs"
    for command in (
        # mkfs.erofs defaults to the host's page size for its blocks; the
        # x86-64 LTP image, built on Linux, has 4 KiB blocks.
        [find_tool("mkfs.erofs"), "--quiet", "-b4096", "-zlz4hc", str(image),
         str(source)],
        [find_tool("fsck.erofs"), str(image)],
    ):
        subprocess.run(
            command, check=True, stdin=subprocess.DEVNULL,
            timeout=TOOL_TIMEOUT_SECONDS,
        )
    return image, sums


def read_back(guest: Guest, sums: dict[str, str], label: str) -> list[str]:
    """Hash every mounted file in the guest and return the mismatches."""
    result = guest.run_command(f"(cd {MOUNT} && sha256sum -- file-*)", 300)
    seen = {}
    for line in result.output.splitlines():
        match = SUM_LINE.match(line.strip())
        if match is not None:
            seen[match.group(2)] = match.group(1)
    failures = [
        f"{label}: {name} reads as {seen.get(name, 'nothing')}, "
        f"wrote {expected}"
        for name, expected in sorted(sums.items())
        if seen.get(name) != expected
    ]
    if result.return_code != 0:
        failures.append(f"{label}: sha256sum exited {result.return_code}")
    matched = sum(seen.get(name) == expected for name, expected in sums.items())
    print(f"{label}: {matched} of {len(sums)} files match", flush=True)
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("gadgets", "jit"), default="jit")
    parser.add_argument("--timeout", type=int, default=180, help="boot timeout")
    arguments = parser.parse_args()

    emulator = arguments.emulator.resolve()
    rootfs = arguments.rootfs.resolve()
    if not emulator.is_file():
        parser.error(f"emulator is not a file: {emulator}")
    if not rootfs.is_file():
        parser.error(f"rootfs is not a file: {rootfs}")

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="uml-erofs-lz4-") as directory:
        image, sums = build_image(pathlib.Path(directory))
        with Guest(
            emulator,
            rootfs,
            "1G",
            "tmpfs",
            (f"ncpus={VCPUS}", f"ubd1r={image}"),
            init_shell=True,
            engine=arguments.engine,
        ) as guest:
            guest.boot_to_shell(arguments.timeout)
            start = len(guest.output())
            guest.check_command(
                f"mkdir -p {MOUNT} && mount -t erofs -o ro /dev/ubdb {MOUNT}",
                60,
            )
            for round in range(ROUNDS):
                failures += read_back(guest, sums, f"round {round} first read")
                failures += read_back(guest, sums, f"round {round} cached read")
                guest.check_command(
                    "sync; echo 3 > /proc/sys/vm/drop_caches", 60
                )
            guest.check_command(f"umount {MOUNT}", 60)
            console = guest.output()[start:]
    failures += [
        f"console: {line.strip()}"
        for line in console.splitlines()
        if KERNEL_FAILURE.search(line) or HOST_FAILURE.search(line)
    ]
    for failure in failures[:40]:
        print(f"FAIL {failure}")
    if failures:
        raise RuntimeError(f"{len(failures)} file or console failure(s)")
    print("erofs-lz4: PASS", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
