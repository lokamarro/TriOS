#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot a guest through the base archive and a per-run initramfs.

The launcher appends a per-run segment (its built-in /init, /boot.env and
/payload files) to the base archive it embeds. A run passes only when the
generic /init mounts the EROFS image beneath a tmpfs overlay, installs the
payload, replacing a file the image carries and leaving the attributes of a
directory the image already has, leaves both layers visible and switches root
into the payload's entry program as PID 1. The same payload must boot over an
ext4 writable layer formatted on a second block device, with the container
state bound from that filesystem, when a second segment appended after the
first replaces its /boot.env and the initrd and the device are named through
symbolic links, and the harness session must run a workload entry and power
the guest off after it. The same payload must also boot from an initrd that
uml-initramfs writes. The remaining cases require every broken
boot to stop with a named reason and a nonzero exit instead of reaching the
image's own init: the base archive alone, a segment appended without the
base's padding, an initrd the kernel never loads, and an invalid /boot.env.
The launcher must remove its scratch initrd when the run returns and when the
guest panics, and the next launch must remove one a run killed outright left
behind. The contract is tools/rootfs/boot-ramdisk/README.md.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile
import uuid


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
GENERIC_INIT = REPOSITORY / "tools" / "rootfs" / "boot-ramdisk" / "init"
SESSION = REPOSITORY / "tools" / "rootfs" / "boot-ramdisk" / "session"
SESSION_PATH = "/usr/local/libexec/uml-session"
WORKLOAD_PATH = "/usr/local/libexec/uml-boot-workload"
WORKLOAD_STATUS = 7
MISSING_INIT = "init=/uml-per-run-initramfs-did-not-run"
BOOT_TIMEOUT_SECONDS = 180
PROBE_PATH = "/uml-boot-probe"
TOKEN_PATH = "/opt/uml-boot-initramfs-test/nested/token"
# A file every supported image carries, which the payload must replace.
REPLACED_PATH = "/etc/hostname"
# A file beneath the image's sticky, world-writable /var/tmp, which must keep
# its mode when the payload adds to it.
KEPT_DIRECTORY = "/var/tmp"
KEPT_DIRECTORY_TOKEN_PATH = f"{KEPT_DIRECTORY}/uml-boot-initramfs-test/token"

# What the writable layer looks like in the new root, by its /boot.env.
TMPFS_STATE_CHECKS = """check state-visible grep -q '^tmpfs /.uml-state tmpfs ' /proc/mounts
"""
EXT4_STATE_CHECKS = """check state-visible grep -q '^/dev/ubdb /.uml-state ext4 ' /proc/mounts
check state-journal sh -c 'dumpe2fs -h /dev/ubdb 2>/dev/null | grep -q has_journal'
check docker-state grep -q '^/dev/ubdb /var/lib/docker ext4 ' /proc/mounts
check containerd-state grep -q '^/dev/ubdb /var/lib/containerd ext4 ' /proc/mounts
check container-state-mode [ "$(stat -c %a /.uml-state/docker)" = 711 ]
"""
EXT4_BOOT_ENV = (
    f"UML_BOOT_UPPER=ext4\nUML_BOOT_UPPER_DEVICE=/dev/ubdb\n"
    f"UML_BOOT_ENTRY={PROBE_PATH}\n"
)
EXT4_DEVICE_BYTES = 256 << 20

# Runs as PID 1 in the new root, with the image's own shell. Each line of
# STATE_CHECKS checks the writable layer this boot asked for.
PROBE_TEMPLATE = f"""#!/bin/sh
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
failures=
check()
{{
	name=$1
	shift
	"$@" || failures="$failures $name"
}}
check pid-1 [ "$$" = 1 ]
check overlay-root grep -q \\
	'^overlay / overlay .*lowerdir=/lower,upperdir=/state/upper,workdir=/state/work' \\
	/proc/mounts
check erofs-lower [ -d /sys/fs/erofs/ubda ]
check lower-visible grep -q '^/dev/ubda /.uml-lower erofs ro[, ]' /proc/mounts
STATE_CHECKS
check initramfs-gone [ ! -e /boot.env ]
token=$(cat {TOKEN_PATH} 2>/dev/null)
check replaced-file [ "$(cat {REPLACED_PATH})" = "$token" ]
check kept-directory [ "$(stat -c %a:%u:%g {KEPT_DIRECTORY})" = \\
	"$(stat -c %a:%u:%g /.uml-lower{KEPT_DIRECTORY})" ]
check new-directory [ "$(stat -c %a {TOKEN_PATH.rsplit("/", 1)[0]})" = 755 ]
check nested-file [ "$(cat {KEPT_DIRECTORY_TOKEN_PATH})" = "$token" ]
echo "UML_BOOT_PROBE_CMDLINE:$(cat /proc/cmdline)"
if [ -z "$failures" ]; then
	echo "UML_BOOT_PROBE_OK:$token"
else
	echo "UML_BOOT_PROBE_FAILED:$failures"
fi
sync
poweroff -f
"""


def probe_text(state_checks: str) -> str:
    return PROBE_TEMPLATE.replace("STATE_CHECKS\n", state_checks)


# A workload the session runs as a child of PID 1, in the runtime
# filesystems it prepared; its status must reach the console.
WORKLOAD = f"""#!/bin/sh
PATH=/usr/sbin:/usr/bin:/sbin:/bin
if [ "$$" != 1 ] && grep -q '^tmpfs /run tmpfs ' /proc/mounts && \\
	grep -q '^tmpfs /tmp tmpfs ' /proc/mounts; then
	echo "UML_BOOT_WORKLOAD:$(cat {TOKEN_PATH})"
fi
exit {WORKLOAD_STATUS}
"""


class CaseFailure(RuntimeError):
    pass


def launch(
    emulator: pathlib.Path,
    rootfs: pathlib.Path,
    engine: str,
    scratch: pathlib.Path,
    extra: list[str],
) -> tuple[int, str]:
    command = [
        str(emulator),
        f"--execution-engine={engine}",
        *extra,
        f"ubd0c={rootfs}",
        "mem=512M",
        "ncpus=1",
    ]
    environment = dict(os.environ, TMPDIR=str(scratch))
    try:
        result = subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=environment,
            timeout=BOOT_TIMEOUT_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        output = (error.output or b"").decode(errors="replace")
        raise CaseFailure(
            f"the guest did not stop within {BOOT_TIMEOUT_SECONDS} seconds\n"
            + output[-8192:]
        ) from error
    return result.returncode, result.stdout.decode(errors="replace")


def require(condition: bool, message: str, output: str) -> None:
    if not condition:
        raise CaseFailure(message + "\n" + output[-8192:])


def require_probe(status: int, output: str, token: str, cmdline: str) -> None:
    require("UML_BOOT_ERROR:" not in output, "the per-run /init failed", output)
    require(
        "UML_BOOT_BASE_ERROR:" not in output,
        "the base archive's /init ran instead of the per-run one",
        output,
    )
    require(
        f"UML_BOOT_SWITCH_ROOT:{PROBE_PATH}" in output,
        "the per-run /init did not switch root into the payload entry",
        output,
    )
    require(
        f"UML_BOOT_PROBE_OK:{token}" in output,
        "the new root is not an overlay on the EROFS image, with both layers "
        "visible and this run's payload",
        output,
    )
    require(
        f"UML_BOOT_PROBE_CMDLINE:" in output and cmdline in output,
        f"the kernel command line lacks {cmdline}",
        output,
    )
    require("Kernel panic" not in output, "the guest panicked", output)
    require(status == 0, f"the guest exited with status {status}", output)


def require_loud_failure(status: int, output: str, marker: str) -> None:
    require(marker in output, f"the guest did not report {marker!r}", output)
    require("Kernel panic" in output, "the guest did not stop at a panic", output)
    require("UML_BOOT_PROBE" not in output, "the payload entry ran", output)
    require(status != 0, "a broken boot exited successfully", output)


def run(arguments: argparse.Namespace) -> None:
    emulator = arguments.emulator.resolve()
    rootfs = arguments.rootfs.resolve()
    base = arguments.base.resolve()
    tool = arguments.initramfs_tool.resolve()
    engine = arguments.engine
    with tempfile.TemporaryDirectory(prefix="uml-boot-initramfs-test-") as name:
        work = pathlib.Path(name)
        scratch = work / "launcher-tmp"
        scratch.mkdir()
        token = uuid.uuid4().hex
        probe = work / "probe"
        probe.write_text(probe_text(TMPFS_STATE_CHECKS))
        probe.chmod(0o755)
        token_file = work / "token"
        token_file.write_text(token + "\n")
        boot_env = work / "boot.env"
        boot_env.write_text(f"UML_BOOT_ENTRY={PROBE_PATH}\n")
        guest_files = [
            f"--guest-file=/boot.env={boot_env}",
            f"--guest-file=/payload{PROBE_PATH}={probe}",
            f"--guest-file=/payload{TOKEN_PATH}={token_file}",
            f"--guest-file=/payload{REPLACED_PATH}={token_file}",
            f"--guest-file=/payload{KEPT_DIRECTORY_TOKEN_PATH}={token_file}",
        ]

        # The kernel never loads the launcher's initrd, so only the
        # launcher's init= stands between the guest and the image's init.
        status, output = launch(
            emulator, rootfs, engine, scratch, [*guest_files, "initrd_offset=x"]
        )
        require_loud_failure(
            status,
            output,
            "Requested init /uml-per-run-initramfs-did-not-run failed",
        )
        leftovers = sorted(path.name for path in scratch.iterdir())
        require(
            not leftovers,
            f"a run that panicked left {leftovers}",
            output,
        )
        print("ok: an initrd the kernel never loads stops at the launcher's init=")

        # A run killed outright removes nothing, so the next launch removes
        # what it left.
        exited = subprocess.Popen(["/usr/bin/true"])
        exited.wait()
        killed = scratch / f"uml-guest-initrd-{exited.pid}-killed"
        killed.mkdir()
        (killed / "initrd.cpio").write_bytes(b"")
        status, output = launch(emulator, rootfs, engine, scratch, guest_files)
        require_probe(status, output, token, MISSING_INIT)
        leftovers = sorted(path.name for path in scratch.iterdir())
        require(
            not leftovers,
            f"the launcher left scratch directories behind: {leftovers}",
            output,
        )
        print("ok: the launcher's initramfs boots the payload on the EROFS overlay")

        # A host that drives the guest elsewhere writes the same initrd with
        # the host tool and names it itself.
        composed = work / "composed.cpio"
        subprocess.run(
            [
                str(tool),
                f"--output={composed}",
                f"--base={base}",
                f"--init={GENERIC_INIT}",
                f"--file=/boot.env={boot_env}",
                f"--file=/payload{PROBE_PATH}={probe}",
                f"--file=/payload{TOKEN_PATH}={token_file}",
                f"--file=/payload{REPLACED_PATH}={token_file}",
                f"--file=/payload{KEPT_DIRECTORY_TOKEN_PATH}={token_file}",
            ],
            check=True,
            timeout=60,
        )
        status, output = launch(
            emulator,
            rootfs,
            engine,
            scratch,
            [f"initrd={composed}", MISSING_INIT],
        )
        require_probe(status, output, token, f"{MISSING_INIT} ")
        print("ok: uml-initramfs writes an initrd that boots the same payload")

        # A second segment appended to the whole initrd replaces the first
        # one's files, as a host payload does on Kernarium: here /boot.env
        # and the probe, which then expects the ext4 writable layer.
        ext4_probe = work / "ext4-probe"
        ext4_probe.write_text(probe_text(EXT4_STATE_CHECKS))
        ext4_probe.chmod(0o755)
        ext4_env = work / "ext4.env"
        ext4_env.write_text(EXT4_BOOT_ENV)
        second = work / "second.cpio"
        subprocess.run(
            [
                str(tool),
                f"--output={second}",
                f"--file=/boot.env={ext4_env}",
                f"--file=/payload{PROBE_PATH}={ext4_probe}",
            ],
            check=True,
            timeout=60,
        )
        layered = work / "layered.cpio"
        first = composed.read_bytes()
        require(len(first) % 4 == 0, "a composed initrd is not padded", "")
        layered.write_bytes(first + second.read_bytes())
        device = work / "writable.img"
        with device.open("wb") as handle:
            handle.truncate(EXT4_DEVICE_BYTES)
        # The kernel sizes an initrd and a disk from the paths it is given,
        # which a user often gives as symbolic links.
        layered_link = work / "layered-link.cpio"
        layered_link.symlink_to(layered)
        device_link = work / "writable-link.img"
        device_link.symlink_to(device)
        status, output = launch(
            emulator,
            rootfs,
            engine,
            scratch,
            [f"initrd={layered_link}", MISSING_INIT, f"ubd1={device_link}"],
        )
        require_probe(status, output, token, f"{MISSING_INIT} ")
        print(
            "ok: a later segment's /boot.env boots the overlay on a fresh ext4 "
            "layer with the container state beside it, both named through "
            "symbolic links"
        )

        status, output = launch(
            emulator, rootfs, engine, scratch, [f"initrd={base}"]
        )
        require_loud_failure(status, output, "UML_BOOT_BASE_ERROR:")
        print("ok: the base archive alone stops at its own /init")

        segment = work / "segment.cpio"
        subprocess.run(
            [
                str(tool),
                f"--output={segment}",
                f"--init={GENERIC_INIT}",
                f"--file=/boot.env={boot_env}",
            ],
            check=True,
            timeout=60,
        )
        base_bytes = base.read_bytes()
        require(
            len(base_bytes) % 4 != 0,
            "the base archive is already four-byte aligned, so an unpadded "
            "append cannot be told apart from a padded one",
            "",
        )
        unpadded = work / "unpadded.cpio"
        unpadded.write_bytes(base_bytes + segment.read_bytes())
        status, output = launch(
            emulator, rootfs, engine, scratch, [f"initrd={unpadded}"]
        )
        # The kernel looks for the next header at the base's exact length,
        # finds "070701" at an unaligned offset and takes it for an unknown
        # compression; it keeps what it unpacked before that point.
        require(
            "invalid magic at start of compressed archive" in output,
            "the kernel accepted a segment appended without padding",
            output,
        )
        require_loud_failure(status, output, "UML_BOOT_BASE_ERROR:")
        print("ok: a segment appended without padding stops at the base's /init")

        invalid_env = work / "invalid.env"
        invalid_env.write_text("UML_BOOT_ENTRY=/sbin/init\nUML_BOOT_TYPO=1\n")
        status, output = launch(
            emulator,
            rootfs,
            engine,
            scratch,
            [f"--guest-file=/boot.env={invalid_env}"],
        )
        require_loud_failure(
            status, output, "UML_BOOT_ERROR:unknown /boot.env parameter: UML_BOOT_TYPO"
        )
        print("ok: an invalid /boot.env stops the per-run /init")

        workload = work / "workload"
        workload.write_text(WORKLOAD)
        workload.chmod(0o755)
        session_env = work / "uml-session.env"
        session_env.write_text(
            f"UML_SESSION_ENTRY={WORKLOAD_PATH}\n"
            "UML_SESSION_RUNTIME_MOUNTS=1\n"
        )
        workload_boot = work / "workload-boot.env"
        workload_boot.write_text(f"UML_BOOT_ENTRY={SESSION_PATH}\n")
        status, output = launch(
            emulator,
            rootfs,
            engine,
            scratch,
            [
                f"--guest-file=/boot.env={workload_boot}",
                f"--guest-file=/payload{SESSION_PATH}={SESSION}",
                f"--guest-file=/payload/etc/uml-session.env={session_env}",
                f"--guest-file=/payload{WORKLOAD_PATH}={workload}",
                f"--guest-file=/payload{TOKEN_PATH}={token_file}",
            ],
        )
        require(
            f"UML_BOOT_WORKLOAD:{token}" in output,
            "the session did not run the workload with its runtime mounts",
            output,
        )
        require(
            f"UML_SESSION_EXIT:{WORKLOAD_STATUS}" in output,
            "the session did not report the workload's status",
            output,
        )
        require("Kernel panic" not in output, "the guest panicked", output)
        require(status == 0, f"the guest exited with status {status}", output)
        print("ok: the session runs a workload entry and powers the guest off")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument(
        "--base",
        required=True,
        type=pathlib.Path,
        help="the accepted base archive the emulator embeds",
    )
    parser.add_argument("--initramfs-tool", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()
    try:
        run(arguments)
    except (CaseFailure, subprocess.SubprocessError) as error:
        print(f"boot initramfs check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
