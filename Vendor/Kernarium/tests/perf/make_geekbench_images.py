#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Make the local images a Geekbench 5 measurement needs: armv7 by default, or
the same release's geekbench_aarch64 with --program.

Neither the Geekbench payload nor an image that carries it is ever published
or committed: the tool refuses to write one into the repository outside its
ignored .build tree. It only packages a Geekbench release directory and checks
its program SHA-256.

  payload  an EROFS image with the program and geekbench.plar at its root,
           for run_guest_geekbench.py --payload (Mac and MacBook Air).
  rootfs   an unpartitioned ext4 filesystem cut from the armhf starter disk's
           partition, for run_guest_geekbench.py --root-type ext4. With
           --release-dir it also carries the payload under /opt/geekbench, as
           a device with no second disk needs (KERNARIUM_ROOTFS on the iPad).

The tools are mkfs.erofs and fsck.erofs, and e2fsprogs' e2fsck, resize2fs and
debugfs, found on PATH or in Homebrew's keg-only e2fsprogs."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY / "tools" / "rootfs" / "starter"))
import gpt  # noqa: E402

PROGRAMS = ("geekbench_armv7", "geekbench_aarch64")
PROGRAM = PROGRAMS[0]
DATA = "geekbench.plar"
# Where an image carries the release; geekbench.sh finds it by name.
INSTALL_DIRECTORIES = ("/opt", "/opt/geekbench",
                       "/opt/geekbench/release")
SHA256 = re.compile(r"[0-9a-f]{64}")
# Room beyond the payload for the filesystem's own bookkeeping.
GROWTH_MARGIN = 16 << 20
HOMEBREW_E2FSPROGS = (
    "/opt/homebrew/opt/e2fsprogs/sbin",
    "/usr/local/opt/e2fsprogs/sbin",
)
# A hard bound on every external tool; the largest step moves ~150 MB.
TOOL_TIMEOUT = 900


class ImageToolError(RuntimeError):
    """A problem the person running the tool can act on."""


def find_tool(name: str) -> str:
    found = shutil.which(name)
    if found is None:
        for directory in HOMEBREW_E2FSPROGS:
            candidate = pathlib.Path(directory, name)
            if candidate.is_file() and os.access(candidate, os.X_OK):
                return str(candidate)
        raise ImageToolError(
            f"{name} is not installed (erofs-utils and e2fsprogs provide it)"
        )
    return found


def run(arguments: list[str], *, check: bool = True
        ) -> subprocess.CompletedProcess[str]:
    try:
        result = subprocess.run(
            arguments, stdin=subprocess.DEVNULL, capture_output=True,
            text=True, timeout=TOOL_TIMEOUT, check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise ImageToolError(f"{arguments[0]} timed out") from error
    if check and result.returncode != 0:
        raise ImageToolError(
            f"{pathlib.Path(arguments[0]).name} failed with status "
            f"{result.returncode}: {(result.stderr or result.stdout).strip()}"
        )
    return result


def digest(path: pathlib.Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def release_files(release: pathlib.Path, program_sha256: str
                  ) -> dict[str, pathlib.Path]:
    """The release's program and data, once the program matches its pin."""
    if not SHA256.fullmatch(program_sha256):
        raise ImageToolError("--program-sha256 must be lowercase hex SHA-256")
    files = {name: release / name for name in (PROGRAM, DATA)}
    for name, path in files.items():
        if path.is_symlink() or not path.is_file():
            raise ImageToolError(f"{release} has no regular file {name}")
    if digest(files[PROGRAM]) != program_sha256:
        raise ImageToolError(f"{files[PROGRAM]} does not match "
                             "--program-sha256")
    return files


def checked_output(path: pathlib.Path, suffix: str) -> pathlib.Path:
    """A new file's path that cannot end up in the repository or a release."""
    output = path.parent.resolve() / path.name
    if output.suffix != suffix:
        raise ImageToolError(f"the output must be named *{suffix}: {output}")
    if output.exists() or output.is_symlink():
        raise ImageToolError(f"refusing to replace {output}")
    if not output.parent.is_dir():
        raise ImageToolError(f"{output.parent} is not a directory")
    # Only the ignored build tree may hold such an image.
    if REPOSITORY in output.parents and (
        REPOSITORY / ".build" not in output.parents
    ):
        raise ImageToolError(
            f"an image carrying Geekbench must stay out of the repository "
            f"(outside .build): {output}"
        )
    return output


def publish(temporary: pathlib.Path, output: pathlib.Path) -> None:
    """Move a finished image into place without ever replacing a file."""
    os.link(temporary, output)
    temporary.unlink()
    print(f"{output}\n  size {output.stat().st_size}\n  sha256 {digest(output)}")


def make_payload(release: pathlib.Path, program_sha256: str,
                 output: pathlib.Path) -> None:
    files = release_files(release, program_sha256)
    mkfs, fsck = find_tool("mkfs.erofs"), find_tool("fsck.erofs")
    output = checked_output(output, ".erofs")
    temporary = output.with_name(f".{output.name}.{os.getpid()}")
    try:
        with tempfile.TemporaryDirectory(prefix="uml-geekbench-payload-") as name:
            stage = pathlib.Path(name)
            for file_name, source in files.items():
                # A plain copy: nothing of the source's extended attributes.
                target = stage / file_name
                shutil.copyfile(source, target)
                target.chmod(0o755 if file_name == PROGRAM else 0o644)
            # Uncompressed so decompression is not in the measurement; a
            # 4 KiB block size mounts on the 4 KiB guest, where the default
            # (the host page size) would not.
            run([mkfs, "-b4096", "-x-1", "--all-root", str(temporary),
                 str(stage)])
        run([fsck, str(temporary)])
        with tempfile.TemporaryDirectory(prefix="uml-geekbench-check-") as name:
            extracted = pathlib.Path(name, "tree")
            run([fsck, f"--extract={extracted}", str(temporary)])
            for file_name, source in files.items():
                if digest(extracted / file_name) != digest(source):
                    raise ImageToolError(f"{file_name} differs in the image")
        publish(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


def copy_partition(starter: pathlib.Path, destination: pathlib.Path) -> None:
    """Copy the starter disk's ext4 filesystem out of its sole partition."""
    try:
        disk = gpt.inspect(starter.resolve())
    except (gpt.ImageError, OSError) as error:
        raise ImageToolError(f"{starter} is not a starter image: {error}")
    offset = int(disk["partition"]["first_lba"]) * gpt.SECTOR_SIZE
    remaining = int(disk["filesystem"]["bytes"])
    with starter.open("rb") as source, destination.open("wb") as target:
        source.seek(offset)
        while remaining:
            chunk = source.read(min(remaining, 8 << 20))
            if not chunk:
                raise ImageToolError(f"{starter} is truncated")
            target.write(chunk)
            remaining -= len(chunk)


def debugfs_commands(image: pathlib.Path, commands: list[str]) -> str:
    """Run debugfs commands writing to the image and return what it said."""
    with tempfile.TemporaryDirectory(prefix="uml-geekbench-debugfs-") as name:
        script = pathlib.Path(name, "commands")
        script.write_text("".join(f"{command}\n" for command in commands))
        result = run([find_tool("debugfs"), "-w", "-f", str(script),
                      str(image)])
    return result.stdout + result.stderr


def install_release(image: pathlib.Path, files: dict[str, pathlib.Path]
                    ) -> None:
    """Grow the filesystem and write the release under /opt/geekbench."""
    e2fsck, resize2fs = find_tool("e2fsck"), find_tool("resize2fs")
    run([e2fsck, "-f", "-p", str(image)])
    needed = sum(path.stat().st_size for path in files.values())
    current = image.stat().st_size
    run([resize2fs, str(image),
         f"{(current + needed + GROWTH_MARGIN + 1023) // 1024}K"])
    directory = INSTALL_DIRECTORIES[-1]
    commands = []
    for path in INSTALL_DIRECTORIES:
        stat = run([find_tool("debugfs"), "-R", f"stat {path}", str(image)],
                   check=False)
        if stat.returncode != 0 or "Type: directory" not in stat.stdout:
            commands.append(f"mkdir {path}")
    commands.append(f"cd {directory}")
    for name, source in files.items():
        mode = 0o100755 if name == PROGRAM else 0o100644
        commands += [f"write {source} {name}", f"sif {name} mode 0{mode:o}",
                     f"sif {name} uid 0", f"sif {name} gid 0"]
    debugfs_commands(image, commands)
    for name, source in files.items():
        with tempfile.TemporaryDirectory(prefix="uml-geekbench-dump-") as dump:
            copy = pathlib.Path(dump, name)
            run([find_tool("debugfs"), "-R",
                 f"dump {directory}/{name} {copy}", str(image)])
            if not copy.is_file() or digest(copy) != digest(source):
                raise ImageToolError(f"{name} differs in the image")


def make_rootfs(starter: pathlib.Path, release: pathlib.Path | None,
                program_sha256: str | None, output: pathlib.Path) -> None:
    files = None
    if release is not None:
        if program_sha256 is None:
            raise ImageToolError("--release-dir needs --program-sha256")
        files = release_files(release, program_sha256)
    elif program_sha256 is not None:
        raise ImageToolError("--program-sha256 needs --release-dir")
    e2fsck = find_tool("e2fsck")
    output = checked_output(output, ".ext4")
    temporary = output.with_name(f".{output.name}.{os.getpid()}")
    try:
        copy_partition(starter, temporary)
        run([e2fsck, "-f", "-n", str(temporary)])
        if files is not None:
            install_release(temporary, files)
        run([e2fsck, "-f", "-n", str(temporary)])
        publish(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    global PROGRAM
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--program", choices=PROGRAMS, default=PROGRAM,
                        help="the Geekbench program to package (default: "
                        "%(default)s)")
    commands = parser.add_subparsers(dest="command", required=True)
    payload = commands.add_parser("payload", help="make the EROFS payload")
    payload.add_argument("--release-dir", required=True, type=pathlib.Path,
                         help="the Geekbench 5 release directory")
    payload.add_argument("--program-sha256", required=True,
                         help="the release's program SHA-256")
    payload.add_argument("--output", required=True, type=pathlib.Path)
    rootfs = commands.add_parser("rootfs", help="make the ext4 root image")
    rootfs.add_argument("--starter-image", required=True, type=pathlib.Path,
                        help="the armhf starter disk (debian-13-minimal-"
                        "armhf.img)")
    rootfs.add_argument("--release-dir", type=pathlib.Path,
                        help="also install this Geekbench 5 release")
    rootfs.add_argument("--program-sha256",
                        help="the release's program SHA-256")
    rootfs.add_argument("--output", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    PROGRAM = arguments.program
    try:
        if arguments.command == "payload":
            make_payload(arguments.release_dir, arguments.program_sha256,
                         arguments.output)
        else:
            make_rootfs(arguments.starter_image, arguments.release_dir,
                        arguments.program_sha256, arguments.output)
    except ImageToolError as error:
        print(f"make_geekbench_images: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
