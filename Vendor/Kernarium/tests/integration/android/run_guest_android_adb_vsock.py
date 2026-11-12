#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove the stock adb client works against the Android guest over vsock.

The runner boots the accepted Android release on a throwaway instance with
``--vsock-forward=127.0.0.1:0-5555`` beside the usual TCP forward to adbd, then
drives the unmodified ``adb`` through the vsock forward:

* ``adb connect`` and a shell;
* ``adb pull`` of a system app's APK from the image itself, ``adb install -r``
  of that APK, which makes the system app an updated one, and ``pm uninstall``,
  which restores the system version;
* ``adb bugreport``, whose zip must be well formed and over 1 MB; and
* the TCP forward still answering beside vsock, with the same bytes.

The runner is opt-in: it needs the macos-android17 executable, an installed
Android release with the kernel set that carries Virtio vsock, and an adb
client. It runs no more than one guest.

usage: run_guest_android_adb_vsock.py --emulator uml-darwin-android17
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import importlib.util
import json
import pathlib
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import zipfile


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "boot_android", REPOSITORY / "tools" / "boot_android.py")
assert SPEC is not None and SPEC.loader is not None
boot_android = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot_android)

GUEST_ADB_PORT = 5555
FORWARD = re.compile(r"vsock forward (\S+):(\d+) -> guest port (\d+)")
# System apps to try, smallest-risk first: updatable, not persistent.
PREFERRED_PACKAGES = (
    "org.lineageos.jelly",
    "org.lineageos.etar",
    "com.android.calculator2",
    "org.lineageos.recorder",
    "com.android.deskclock",
    "com.android.gallery3d",
)
COMMAND_SECONDS = 120
INSTALL_SECONDS = 300
BUGREPORT_MINIMUM_BYTES = 1 << 20
FORWARD_SECONDS = 60


def run_adb(adb: pathlib.Path, serial: str, *arguments: str,
            timeout: float = COMMAND_SECONDS,
            check: bool = True) -> subprocess.CompletedProcess:
    result = subprocess.run(
        [str(adb), "-s", serial, *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
        check=False,
    )
    if check and result.returncode != 0:
        raise RuntimeError(
            f"adb {' '.join(arguments)} on {serial} failed "
            f"({result.returncode}):\n{result.stdout}"
        )
    return result


def unused_loopback_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def vsock_forward_port(log: pathlib.Path, guest: subprocess.Popen) -> int:
    """The host port the launcher chose for the forward to adbd."""
    deadline = time.monotonic() + FORWARD_SECONDS
    while time.monotonic() < deadline:
        if guest.poll() is not None:
            raise RuntimeError(
                f"the guest exited with status {guest.returncode}:\n"
                + log.read_text(errors="replace")[-4000:])
        for _, port, guest_port in FORWARD.findall(
                log.read_text(errors="replace")):
            if int(guest_port) == GUEST_ADB_PORT:
                return int(port)
        time.sleep(0.2)
    raise RuntimeError("the launcher reported no vsock forward")


def system_packages(adb: pathlib.Path, serial: str) -> dict[str, str]:
    """Package to APK path, for the system apps that are not privileged."""
    listing = run_adb(adb, serial, "shell", "pm", "list", "packages", "-s", "-f")
    found = {}
    for line in listing.stdout.splitlines():
        match = re.match(r"package:(/\S+\.apk)=(\S+)\s*$", line.strip())
        if match and "/priv-app/" not in match[1]:
            found[match[2]] = match[1]
    return found


def candidates(found: dict[str, str]) -> list[str]:
    chosen = [name for name in PREFERRED_PACKAGES if name in found]
    chosen += [name for name in sorted(found) if name not in chosen]
    return chosen[:4]


def installed_path(adb: pathlib.Path, serial: str, package: str) -> str:
    result = run_adb(adb, serial, "shell", "pm", "path", package)
    paths = [line.removeprefix("package:").strip()
             for line in result.stdout.splitlines() if line.startswith("package:")]
    if not paths:
        raise RuntimeError(f"{package} has no installed path:\n{result.stdout}")
    return paths[0]


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def install_system_update(adb: pathlib.Path, serial: str, tcp_serial: str,
                          work: pathlib.Path) -> dict:
    """Pulls a system APK over vsock, installs it over the system app, and
    removes the update again. Returns what happened."""
    problems = []
    for package in candidates(system_packages(adb, serial)):
        original = installed_path(adb, serial, package)
        apk = work / f"{package}.apk"
        started = time.monotonic()
        run_adb(adb, serial, "pull", original, str(apk), timeout=INSTALL_SECONDS)
        pull_seconds = time.monotonic() - started
        size = apk.stat().st_size
        install = run_adb(
            adb, serial, "install", "-r", str(apk),
            timeout=INSTALL_SECONDS, check=False)
        if install.returncode != 0 or "Success" not in install.stdout:
            problems.append(f"{package}: {install.stdout.strip()}")
            continue
        updated = installed_path(adb, serial, package)
        if updated == original:
            raise RuntimeError(
                f"{package}: install -r left the system copy {original} in use")
        uninstall = run_adb(adb, serial, "shell", "pm", "uninstall", package)
        if "Success" not in uninstall.stdout:
            raise RuntimeError(f"{package}: pm uninstall said {uninstall.stdout!r}")
        restored = installed_path(adb, serial, package)
        if restored != original:
            raise RuntimeError(
                f"{package}: the system copy {original} did not come back "
                f"({restored})")
        # The same file over the TCP forward is the same bytes.
        over_tcp = work / f"{package}.tcp.apk"
        run_adb(adb, tcp_serial, "pull", original, str(over_tcp),
                timeout=INSTALL_SECONDS)
        if sha256(over_tcp) != sha256(apk):
            raise RuntimeError(f"{package}: the TCP pull differs from the vsock pull")
        return {
            "package": package,
            "bytes": size,
            "pull_seconds": round(pull_seconds, 3),
            "pull_mb_per_s": round(size / pull_seconds / 1e6, 2),
        }
    raise RuntimeError(
        "no system app took an update over vsock:\n  " + "\n  ".join(problems))


def check_bugreport(adb: pathlib.Path, serial: str, work: pathlib.Path,
                    timeout: float) -> dict:
    directory = work / "bugreport"
    directory.mkdir()
    started = time.monotonic()
    result = run_adb(adb, serial, "bugreport", str(directory),
                     timeout=timeout, check=False)
    elapsed = time.monotonic() - started
    archives = sorted(directory.glob("*.zip"))
    if result.returncode != 0 or len(archives) != 1:
        raise RuntimeError(
            f"adb bugreport ({result.returncode}) wrote {archives}:\n"
            f"{result.stdout[-4000:]}")
    size = archives[0].stat().st_size
    if size < BUGREPORT_MINIMUM_BYTES:
        raise RuntimeError(f"the bug report is only {size} bytes")
    if not zipfile.is_zipfile(archives[0]):
        raise RuntimeError("the bug report is not a zip archive")
    with zipfile.ZipFile(archives[0]) as archive:
        bad = archive.testzip()
        if bad is not None:
            raise RuntimeError(f"the bug report's member {bad} is damaged")
    return {"bytes": size, "seconds": round(elapsed, 1)}


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", required=True, type=pathlib.Path,
                        help="the macos-android17 uml-darwin-android17")
    parser.add_argument("--release", type=pathlib.Path,
                        help="installed Android release (default: accepted)")
    parser.add_argument("--adb", default="adb", help="adb client to use")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--memory", default="4G")
    parser.add_argument("--boot-timeout", type=int, default=900,
                        help="seconds to wait for adbd and for boot_completed")
    parser.add_argument("--bugreport-timeout", type=int, default=1800,
                        help="seconds adb bugreport may take")
    arguments = parser.parse_args()
    if arguments.cpus <= 0 or arguments.boot_timeout <= 0 \
            or arguments.bugreport_timeout <= 0:
        parser.error("counts and timeouts must be positive")

    adb = boot_android.resolve_executable(arguments.adb, sdk_fallback=True)
    emulator = boot_android.resolve_executable(arguments.emulator)
    # Short, so every socket path below it stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-adbv-"))
    guest = None
    log_stream = None
    connected: list[str] = []
    try:
        instance = private / "instance"
        tcp_port = unused_loopback_port()
        options = boot_android.parse_arguments([
            *(["--release", str(arguments.release)] if arguments.release else []),
            "--instance", str(instance),
            "--emulator", str(emulator),
            "--adb-port", str(tcp_port),
            "--cpus", str(arguments.cpus),
            "--memory", arguments.memory,
            "--display", "offscreen",
            "--audio-output", "null",
            "--engine", "gadgets",
        ])
        release = boot_android.release_path(options.release)
        identity = boot_android.android_instance.read_release(release)
        decision = boot_android.plan_start(options, identity, instance)
        boot_android.android_instance.apply(instance, identity, decision)
        command = [
            *boot_android.emulator_command(
                options, release, instance / boot_android.android_instance.DATA_DISK,
                emulator, boot_android.data_capacity(options, identity)),
            f"--vsock-forward=127.0.0.1:0-{GUEST_ADB_PORT}",
        ]
        log = private / "emulator.log"
        log_stream = log.open("wb")
        guest = subprocess.Popen(
            command, stdin=subprocess.DEVNULL, stdout=log_stream,
            stderr=log_stream, start_new_session=True)

        vsock_serial = f"127.0.0.1:{vsock_forward_port(log, guest)}"
        tcp_serial = f"127.0.0.1:{tcp_port}"
        for serial in (vsock_serial, tcp_serial):
            boot_android.wait_for_adb(adb, serial, guest, arguments.boot_timeout)
            connected.append(serial)
        if not boot_android.wait_for_boot_completed(
                adb, vsock_serial, guest, arguments.boot_timeout):
            raise RuntimeError("Android did not reach sys.boot_completed")

        report = {}
        shell = run_adb(adb, vsock_serial, "shell", "echo vsock-shell; id -un")
        if "vsock-shell" not in shell.stdout or "shell" not in shell.stdout:
            raise RuntimeError(f"the vsock shell answered {shell.stdout!r}")
        tcp_shell = run_adb(adb, tcp_serial, "shell", "echo tcp-shell")
        if "tcp-shell" not in tcp_shell.stdout:
            raise RuntimeError(f"the TCP shell answered {tcp_shell.stdout!r}")
        report["install"] = install_system_update(
            adb, vsock_serial, tcp_serial, private)
        report["bugreport"] = check_bugreport(
            adb, vsock_serial, private, arguments.bugreport_timeout)
        # TCP still answers after all of it.
        after = run_adb(adb, tcp_serial, "shell", "echo tcp-after")
        if "tcp-after" not in after.stdout:
            raise RuntimeError(f"the TCP shell answered {after.stdout!r}")
        print(json.dumps(report))
        print("stock adb installed, pulled and bug-reported over vsock, "
              "and TCP 5555 answered beside it")
        return 0
    finally:
        for serial in connected:
            boot_android.disconnect_adb(adb, serial)
        if guest is not None:
            boot_android.stop_guest(guest)
        if log_stream is not None:
            log_stream.close()
        shutil.rmtree(private, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
