#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the manual overlay launcher's EROFS boot options in dry-run mode.

The launcher boots through the emulator's per-run initramfs and the harness
session, so its command names their parameter files and never an init."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import shlex
import subprocess
import tempfile
from pathlib import Path


def run(
    launcher: Path,
    rootfs: Path,
    *kernel_arguments: str,
    options: tuple[str, ...] = (),
) -> subprocess.CompletedProcess[str]:
    # macOS runs the launcher under its own Bash 3.2, whatever an env lookup
    # would find first, so the test does too when that shell exists.
    shell = ["/bin/bash"] if Path("/bin/bash").is_file() else []
    command = [
        *shell,
        str(launcher),
        "--emulator",
        "/usr/bin/true",
        "--rootfs",
        str(rootfs),
        "--dry-run",
        *options,
    ]
    if kernel_arguments:
        command.append("--")
        command.extend(kernel_arguments)
    return subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def parameters(path: Path) -> dict[str, str]:
    """The KEY=VALUE lines of a /boot.env-syntax file."""
    values = {}
    for line in path.read_text().splitlines():
        if line and not line.startswith("#"):
            key, _, value = line.partition("=")
            values[key] = value
    return values


def guest_files(command: list[str]) -> dict[str, Path]:
    prefix = "--guest-file="
    files = {}
    for argument in command:
        if argument.startswith(prefix):
            archive, _, host = argument[len(prefix):].partition("=")
            files[archive] = Path(host)
    return files


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--launcher", required=True, type=Path)
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="uml-overlay-launcher-") as name:
        rootfs = Path(name) / "rootfs image.erofs"
        rootfs.touch()

        # No kernel arguments leaves an empty array, which Bash 3.2 rejects
        # under set -u unless the launcher guards its expansion.
        bare = run(arguments.launcher, rootfs)
        require(bare.returncode == 0, f"bare dry run failed:\n{bare.stdout}")

        valid = run(arguments.launcher, rootfs, "quiet")
        require(valid.returncode == 0, f"valid dry run failed:\n{valid.stdout}")
        command = shlex.split(valid.stdout)
        require(
            f"ubd0c={rootfs}" in command and command[-1] == "quiet",
            f"dry run lacks the EROFS disk or the caller's argument:\n{valid.stdout}",
        )
        require(
            not any(
                argument.startswith(("init=", "initrd=", "root=", "rootfstype="))
                for argument in command
            ),
            f"dry run bypasses the per-run initramfs:\n{valid.stdout}",
        )
        files = guest_files(command)
        require(
            set(files)
            == {
                "/boot.env",
                "/payload/usr/local/libexec/uml-session",
                "/payload/etc/uml-session.env",
            }
            and all(path.is_file() for path in files.values()),
            f"dry run lacks the session files:\n{valid.stdout}",
        )
        require(
            parameters(files["/boot.env"])
            == {
                "UML_BOOT_ROOT": "/dev/ubda",
                "UML_BOOT_ROOT_TYPE": "erofs",
                "UML_BOOT_UPPER": "tmpfs",
                "UML_BOOT_ENTRY": "/usr/local/libexec/uml-session",
            },
            "the boot parameters are not an EROFS overlay entering the session",
        )
        require(
            parameters(files["/payload/etc/uml-session.env"])
            == {
                "UML_SESSION_ENTRY": "systemd",
                "UML_SESSION_RESOLVER": "user-network",
                "UML_SESSION_CONTAINER_STATE": "tmpfs",
            },
            "the session does not start systemd with its resolver and "
            "container state",
        )

        retired = run(arguments.launcher, rootfs, options=("--root-device", "/dev/ubdb"))
        require(
            retired.returncode == 2 and "unknown option" in retired.stdout,
            f"the retired --root-device option was accepted:\n{retired.stdout}",
        )

        rejected = {
            "ubd0=/tmp/other.erofs": "controlled by the launcher",
            "ubd0c=/tmp/other.erofs": "controlled by the launcher",
            "ubd0r=/tmp/other.erofs": "controlled by the launcher",
            "root=/dev/ubdb": "controlled by the launcher",
            "rootfstype=ext4": "controlled by the launcher",
            "init=/bin/sh": "controlled by the launcher",
            "initrd=/tmp/other.cpio": "controlled by the launcher",
            "--guest-init=/tmp/init": "controlled by the launcher",
            "--guest-file=/boot.env=/tmp/boot.env": "controlled by the launcher",
            "mem=1G": "controlled by the launcher",
            "console=null": "controlled by the launcher",
            "systemd.unit=rescue.target": "controlled by the launcher",
        }
        for kernel_argument, diagnostic in rejected.items():
            result = run(arguments.launcher, rootfs, kernel_argument)
            require(
                result.returncode == 2,
                f"protected option was accepted: {kernel_argument}\n{result.stdout}",
            )
            require(
                diagnostic in result.stdout,
                f"protected option failed without its diagnostic: "
                f"{kernel_argument}\n{result.stdout}",
            )

    print("overlay launcher rootfs option policy tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
