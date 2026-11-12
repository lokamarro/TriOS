#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot the accepted Android guest on macOS and manage its ADB transport.

The guest has the release's read-only system disk and one writable data disk,
which lives in an instance directory beside `instance.json`. Before each start
the launcher checks, with `tools/android_instance.py`, that the release may run
on that disk: a newer release is snapshotted and started, an older one is
refused. The moment a start reaches `sys.boot_completed` the update it began
counts as applied and its snapshot is dropped; after that start shuts down
cleanly the launcher records the completion.
"""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
from pathlib import Path
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import time
from typing import NoReturn, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
_ARTIFACTS = str(REPO_ROOT / "kernel/common/artifacts")
if _ARTIFACTS not in sys.path:
    sys.path.insert(0, _ARTIFACTS)
from contract_checks import accepted_directory_name  # noqa: E402
_TOOLS = str(REPO_ROOT / "tools")
if _TOOLS not in sys.path:
    sys.path.insert(0, _TOOLS)
import android_instance  # noqa: E402
import android_provision  # noqa: E402
_GUEST_SUPPORT = str(REPO_ROOT / "tests/integration/support")
if _GUEST_SUPPORT not in sys.path:
    sys.path.insert(0, _GUEST_SUPPORT)
from guest import remove_emulator_run_directories  # noqa: E402

ACCEPTED_RELEASE = (
    REPO_ROOT / "android/lineage-24/metadata/accepted-release.env"
)
RELEASES_ROOT = REPO_ROOT / ".build/android/releases"
INSTANCES_ROOT = REPO_ROOT / ".build/android/instances"
DEFAULT_EMULATOR = (
    REPO_ROOT / ".build/presets/macos-android17/bin/uml-darwin-android17"
)
DEFAULT_ADB_PORT = 5555
GUEST_ADB_PORT = 5555
DEFAULT_DISPLAY_WIDTH = 720
DEFAULT_DISPLAY_HEIGHT = 1280
DEFAULT_VNC_PORT = 5900
DEFAULT_INSTANCE = INSTANCES_ROOT / "android-17"
# The signals that ask the emulator to stop, each with its grace period,
# before it is killed.
GUEST_STOP_STAGES = ((signal.SIGINT, 10), (signal.SIGTERM, 5))


class LauncherError(RuntimeError):
    """A user-facing launcher failure."""


class RequestedShutdown(Exception):
    def __init__(self, signum: int) -> None:
        super().__init__(signum)
        self.signum = signum


def positive_integer(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"not an integer: {value}") from error
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def port_number(value: str) -> int:
    parsed = positive_integer(value)
    if parsed > 65535:
        raise argparse.ArgumentTypeError("must be at most 65535")
    return parsed


def display_size(value: str) -> str:
    """A WIDTHxHEIGHT pixel size, returned in the emulator's spelling."""
    width, separator, height = value.partition("x")
    if separator != "x" or not (width.isdecimal() and height.isdecimal()):
        raise argparse.ArgumentTypeError("must be WIDTHxHEIGHT")
    return f"{positive_integer(width)}x{positive_integer(height)}"


def mirror_setting(value: str) -> str:
    """on, off, or WIDTH@FPS, returned in the emulator's spelling."""
    if value in ("on", "off"):
        return value
    width, separator, rate = value.partition("@")
    if separator != "@" or not (width.isdecimal() and rate.isdecimal()):
        raise argparse.ArgumentTypeError("must be on, off or WIDTH@FPS")
    return f"{positive_integer(width)}@{positive_integer(rate)}"


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "boot the accepted Android 17 image on macOS, wait for "
            "ADB, and disconnect ADB when the guest exits"
        )
    )
    parser.add_argument(
        "--release",
        type=Path,
        help="installed Android release (default: accepted release)",
    )
    parser.add_argument(
        "--instance",
        type=Path,
        help=(
            "directory of the guest instance: its data.img, instance.json and "
            "update snapshot (default: android-17 below .build/android/instances)"
        ),
    )
    parser.add_argument(
        "--data-gib",
        type=positive_integer,
        help=(
            "capacity of the data disk in GiB: a new disk is created at it, an "
            "existing one grown to it, and one never shrinks (default: the "
            "release's default for a new disk, otherwise unchanged)"
        ),
    )
    update = parser.add_mutually_exclusive_group()
    update.add_argument(
        "--retry",
        action="store_true",
        help=(
            "boot again a release whose update did not reach boot_completed, "
            "keeping its snapshot"
        ),
    )
    update.add_argument(
        "--restore-snapshot",
        action="store_true",
        help=(
            "put back the data an unfinished update started from, then start "
            "the release afresh"
        ),
    )
    update.add_argument(
        "--reset",
        action="store_true",
        help="delete the instance (data disk, state, snapshots) and create it anew",
    )
    parser.add_argument(
        "--emulator",
        type=Path,
        default=DEFAULT_EMULATOR,
        help="Android macOS executable produced by the macos-android17 preset",
    )
    parser.add_argument(
        "--adb",
        default="adb",
        help="ADB executable name or path (default: adb from PATH or Android SDK)",
    )
    parser.add_argument("--adb-port", type=port_number, default=DEFAULT_ADB_PORT)
    parser.add_argument(
        "--adb-timeout",
        type=positive_integer,
        default=300,
        help="seconds to wait for the guest ADB daemon (default: 300)",
    )
    parser.add_argument(
        "--boot-timeout",
        type=positive_integer,
        default=900,
        help=(
            "seconds after ADB to wait for sys.boot_completed before the start "
            "no longer counts as complete (default: 900)"
        ),
    )
    parser.add_argument(
        "--provision",
        action="store_true",
        help=(
            "for harnesses that measure an erased guest: once the guest has "
            "reached sys.boot_completed, provision it over adb the way Setup "
            "Wizard's last step does (tools/android_provision.py). The product "
            "starts a fresh guest in Setup Wizard, and does so without this"
        ),
    )
    parser.add_argument("--cpus", type=positive_integer, default=10)
    parser.add_argument("--memory", default="8G")
    parser.add_argument(
        "--width",
        type=positive_integer,
        default=DEFAULT_DISPLAY_WIDTH,
        help=f"guest display width in pixels (default: {DEFAULT_DISPLAY_WIDTH})",
    )
    parser.add_argument(
        "--height",
        type=positive_integer,
        default=DEFAULT_DISPLAY_HEIGHT,
        help=f"guest display height in pixels (default: {DEFAULT_DISPLAY_HEIGHT})",
    )
    parser.add_argument(
        "--dpi",
        type=positive_integer,
        help=(
            "guest display density, which is also the Android density and, "
            "with the size, picks the launcher's grid (default: the "
            "emulator's 213)"
        ),
    )
    parser.add_argument(
        "--display",
        choices=("native", "offscreen", "vnc"),
        default="native",
        help=(
            "native Metal window; offscreen, which presents into a Metal "
            "layer that belongs to no window, for memory measurements; or a "
            "loopback VNC server that scripted runs can observe and drive "
            "(default: native)"
        ),
    )
    parser.add_argument(
        "--offscreen-size",
        type=display_size,
        metavar="WIDTHxHEIGHT",
        help=(
            "pixel size of the offscreen display's drawables, such as the "
            "view an iPad shows the guest in (default: the guest display)"
        ),
    )
    parser.add_argument(
        "--offscreen-mirror",
        type=mirror_setting,
        metavar="on|off|WIDTH@FPS",
        help=(
            "keep the frame mirror that Picture in Picture runs on an "
            "offscreen display; on is 800@15 (default: off)"
        ),
    )
    parser.add_argument(
        "--vnc-port",
        type=port_number,
        default=DEFAULT_VNC_PORT,
        help=f"loopback VNC port for --display=vnc (default: {DEFAULT_VNC_PORT})",
    )
    parser.add_argument(
        "--engine",
        choices=("gadgets", "jit"),
        default="gadgets",
    )
    parser.add_argument(
        "--audio-output",
        choices=("host", "null"),
        default="host",
        help=(
            "play guest audio through the Mac's default output, or discard "
            "it at the rate it would play, for runs that must be silent "
            "(default: host)"
        ),
    )
    parser.add_argument(
        "--audio-input",
        choices=("silent", "host"),
        default="silent",
        help=(
            "record silence in the guest, or record from the Mac's "
            "microphone; host asks macOS for microphone access on behalf of "
            "the app that runs this script (default: silent)"
        ),
    )
    boot_mode = parser.add_mutually_exclusive_group()
    boot_mode.add_argument(
        "--boot-mode",
        choices=("auto", "normal", "recovery"),
        default="auto",
        help=(
            "which ramdisk the first boot uses: auto follows the disk's "
            "bootloader message, as every restart does (default: auto)"
        ),
    )
    boot_mode.add_argument(
        "--recovery",
        action="store_const",
        const="recovery",
        dest="boot_mode",
        help="boot the accepted release into recovery once, as a first-boot override",
    )
    parser.add_argument(
        "--build",
        action="store_true",
        help="incrementally rebuild the macos-android17 executable before boot",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print build, emulator, and ADB commands without changing state",
    )
    options = parser.parse_args(arguments)
    if not options.memory or any(character.isspace() for character in options.memory):
        parser.error("--memory must be one kernel argument without whitespace")
    if options.display != "offscreen" and (
        options.offscreen_size or options.offscreen_mirror
    ):
        parser.error("--offscreen-* options require --display=offscreen")
    return options


def accepted_directory(descriptor: Path = ACCEPTED_RELEASE) -> str:
    return accepted_directory_name(
        descriptor, "LINEAGE24_ACCEPTED_DIRECTORY_NAME", LauncherError
    )


def release_path(
    requested: Path | None,
    *,
    descriptor: Path = ACCEPTED_RELEASE,
    releases_root: Path = RELEASES_ROOT,
) -> Path:
    if requested is None:
        candidate = releases_root / accepted_directory(descriptor)
    else:
        candidate = requested.expanduser()
        if not candidate.is_absolute():
            candidate = Path.cwd() / candidate
    try:
        release = candidate.resolve(strict=True)
    except OSError as error:
        raise LauncherError(f"Android release is unavailable: {candidate}: {error}") from error
    if not release.is_dir():
        raise LauncherError(f"Android release is not a directory: {release}")
    required = (
        release / "release-manifest.json",
        release / "boot/kernel-command-line.txt",
        release / "guest/system.img",
        release / "guest/partition-layout.json",
    )
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise LauncherError("Android release is incomplete: " + ", ".join(missing))
    return release


def instance_path(requested: Path | None) -> Path:
    candidate = DEFAULT_INSTANCE if requested is None else requested.expanduser()
    if not candidate.is_absolute():
        candidate = Path.cwd() / candidate
    return candidate.resolve(strict=False)


def data_capacity(options: argparse.Namespace, release: android_instance.Release) -> int | None:
    """The capacity to ask the emulator for, checked against the release's range."""
    requested = options.data_gib
    if requested is None:
        return None
    if not release.minimum_gib <= requested <= release.maximum_gib:
        raise LauncherError(
            f"--data-gib must be {release.minimum_gib} through "
            f"{release.maximum_gib} GiB for this release"
        )
    return requested


def resolve_executable(value: str | Path, *, sdk_fallback: bool = False) -> Path:
    text = os.fspath(value)
    expanded = Path(text).expanduser()
    if expanded.parent != Path(".") or expanded.is_absolute():
        candidate = expanded if expanded.is_absolute() else Path.cwd() / expanded
        resolved = candidate.resolve(strict=False)
        if resolved.is_file() and os.access(resolved, os.X_OK):
            return resolved
        raise LauncherError(f"executable is unavailable: {resolved}")
    discovered = shutil.which(text)
    if discovered is not None:
        return Path(discovered).resolve()
    if sdk_fallback:
        fallback = Path.home() / "Library/Android/sdk/platform-tools/adb"
        if fallback.is_file() and os.access(fallback, os.X_OK):
            return fallback.resolve()
    raise LauncherError(f"cannot locate executable: {text}")


def build_commands(cmake: Path) -> list[list[str]]:
    return [
        [str(cmake), "--preset", "macos-android17"],
        [str(cmake), "--build", "--preset", "macos-android17"],
    ]


def emulator_command(
    options: argparse.Namespace,
    release: Path,
    data_disk: Path,
    emulator: Path,
    data_gib: int | None = None,
) -> list[str]:
    return [
        str(emulator),
        f"--android-release={release}",
        f"--android-data-disk={data_disk}",
        *([f"--android-data-gib={data_gib}"] if data_gib is not None else []),
        f"--android-boot-mode={options.boot_mode}",
        f"--net-forward=tcp:127.0.0.1:{options.adb_port}-{GUEST_ADB_PORT}",
        "--gpu",
        f"--gpu-display={options.display}",
        *([f"--gpu-vnc-port={options.vnc_port}"]
          if options.display == "vnc" else []),
        *([f"--gpu-offscreen-size={options.offscreen_size}"]
          if options.offscreen_size else []),
        *([f"--gpu-offscreen-mirror={options.offscreen_mirror}"]
          if options.offscreen_mirror else []),
        f"--gpu-width={options.width}",
        f"--gpu-height={options.height}",
        *([f"--gpu-dpi={options.dpi}"] if options.dpi else []),
        "--video",
        "--video-encoder",
        *([f"--audio-output={options.audio_output}"]
          if options.audio_output != "host" else []),
        *([f"--audio-input={options.audio_input}"]
          if options.audio_input != "silent" else []),
        f"ncpus={options.cpus}",
        f"mem={options.memory}",
        f"--execution-engine={options.engine}",
    ]


def ensure_loopback_port_available(port: int) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            probe.bind(("127.0.0.1", port))
        except OSError as error:
            raise LauncherError(
                f"ADB forwarding port 127.0.0.1:{port} is unavailable: {error}"
            ) from error


def disconnect_adb(adb: Path, serial: str) -> None:
    try:
        subprocess.run(
            [str(adb), "disconnect", serial],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        pass


def wait_for_adb(
    adb: Path,
    serial: str,
    guest: subprocess.Popen[bytes],
    timeout: int,
) -> None:
    deadline = time.monotonic() + timeout
    last_response = ""
    while time.monotonic() < deadline:
        result = guest.poll()
        if result is not None:
            raise LauncherError(
                f"Android guest exited with status {result} before ADB became ready"
            )
        try:
            connection = subprocess.run(
                [str(adb), "connect", serial],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=10,
                check=False,
            )
            last_response = connection.stdout.strip()
            state = subprocess.run(
                [str(adb), "-s", serial, "get-state"],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=10,
                check=False,
            )
            # Recovery's adbd reports the state "recovery" instead of "device".
            if state.returncode == 0 and state.stdout.strip() in ("device", "recovery"):
                return
        except subprocess.TimeoutExpired:
            last_response = "ADB command timed out"
        time.sleep(1)
    detail = f": {last_response}" if last_response else ""
    raise LauncherError(f"ADB did not become ready within {timeout} seconds{detail}")


def start_guest(command: list[str]) -> subprocess.Popen[bytes]:
    """The emulator leads its own process group, so stopping it reaches the
    helpers it starts."""
    return subprocess.Popen(command, start_new_session=True)


def signal_guest(guest: subprocess.Popen[bytes], signum: int) -> None:
    try:
        os.killpg(guest.pid, signum)
    except ProcessLookupError:
        # No group has the emulator's ID, so it leads none. That is no
        # evidence it exited: signal the emulator itself.
        guest.send_signal(signum)


def stop_guest(guest: subprocess.Popen[bytes]) -> None:
    """Return only after the emulator has been reaped, and remove the run
    directories that one ended with SIGKILL could not."""
    try:
        for signum, timeout in GUEST_STOP_STAGES:
            if guest.poll() is not None:
                return
            signal_guest(guest, signum)
            try:
                guest.wait(timeout=timeout)
                return
            except subprocess.TimeoutExpired:
                continue
        if guest.poll() is not None:
            return
        signal_guest(guest, signal.SIGKILL)
        guest.wait()
    finally:
        remove_emulator_run_directories(guest.pid)


def install_shutdown_handlers() -> None:
    def request_shutdown(signum: int, _frame: object) -> NoReturn:
        raise RequestedShutdown(signum)

    signal.signal(signal.SIGTERM, request_shutdown)
    signal.signal(signal.SIGHUP, request_shutdown)


def wait_for_boot_completed(
    adb: Path,
    serial: str,
    guest: subprocess.Popen[bytes],
    timeout: int,
) -> bool:
    """Whether the guest published sys.boot_completed before `timeout` seconds
    passed and before it exited."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if guest.poll() is not None:
            return False
        try:
            value = subprocess.run(
                [str(adb), "-s", serial, "shell", "getprop", "sys.boot_completed"],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=10,
                check=False,
            )
            if value.returncode == 0 and value.stdout.strip() == "1":
                return True
        except subprocess.TimeoutExpired:
            pass
        time.sleep(2)
    return False


def provision_guest(adb: Path, serial: str) -> None:
    """Provision the booted guest (--provision); a failure ends the start."""
    try:
        changed = android_provision.provision(
            android_provision.adb_shell(str(adb), serial)
        )
    except (
        android_provision.ProvisionError,
        subprocess.SubprocessError,
        OSError,
    ) as error:
        raise LauncherError(f"cannot provision the guest: {error}") from error
    print(
        "Guest provisioned." if changed else "Guest was already provisioned.",
        file=sys.stderr,
    )


def plan_start(
    options: argparse.Namespace,
    release: android_instance.Release,
    instance: Path,
) -> android_instance.Decision:
    """What the instance needs before this release starts on it."""
    data = instance / android_instance.DATA_DISK
    try:
        state = android_instance.read_state(instance)
    except android_instance.InstanceError as error:
        raise LauncherError(str(error)) from error
    snapshot_exists = any(instance.glob(android_instance.SNAPSHOT_PREFIX + "*"))
    return android_instance.decide(
        state,
        data.is_file(),
        snapshot_exists,
        release,
        android_instance.Flags(
            retry=options.retry,
            restore_snapshot=options.restore_snapshot,
            reset=options.reset,
        ),
    )


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    try:
        release = release_path(options.release)
        try:
            identity = android_instance.read_release(release)
        except android_instance.InstanceError as error:
            raise LauncherError(str(error)) from error
        instance = instance_path(options.instance)
        data_disk = instance / android_instance.DATA_DISK
        capacity = data_capacity(options, identity)
        decision = plan_start(options, identity, instance)
        adb = resolve_executable(options.adb, sdk_fallback=True)
        commands: list[list[str]] = []
        if options.build:
            commands = build_commands(resolve_executable("cmake"))
        guest_command = emulator_command(
            options, release, data_disk, options.emulator, capacity
        )
        serial = f"127.0.0.1:{options.adb_port}"
        if options.dry_run:
            print(f"instance {instance}: {decision.action} ({decision.reason})")
            if decision.action.startswith("refuse"):
                return 1
            for command in commands:
                print(shlex.join(command))
            print(shlex.join(guest_command))
            print(shlex.join([str(adb), "connect", serial]))
            print(shlex.join([str(adb), "disconnect", serial]))
            return 0
        if sys.platform != "darwin":
            raise LauncherError("this launcher supports macOS only")
        for command in commands:
            subprocess.run(command, cwd=REPO_ROOT, check=True)
        emulator = resolve_executable(options.emulator)
        try:
            android_instance.apply(instance, identity, decision)
        except android_instance.InstanceError as error:
            raise LauncherError(str(error)) from error
        guest_command = emulator_command(options, release, data_disk, emulator, capacity)
        ensure_loopback_port_available(options.adb_port)
        disconnect_adb(adb, serial)
        print(f"Booting accepted Android release {release.name}.", file=sys.stderr)
        print(f"Instance {instance}: {decision.action}.", file=sys.stderr)
        if options.display == "vnc":
            print(f"VNC display on 127.0.0.1:{options.vnc_port}; press Ctrl-C "
                  "to stop the guest.", file=sys.stderr)
        elif options.display == "offscreen":
            print("Windowless display; press Ctrl-C to stop the guest.",
                  file=sys.stderr)
        else:
            print("Native Metal display; press Ctrl-C to stop the guest.",
                  file=sys.stderr)
        guest = start_guest(guest_command)
        install_shutdown_handlers()
        booted = False
        try:
            wait_for_adb(adb, serial, guest, options.adb_timeout)
            print(f"ADB connected: {adb} -s {serial} shell", file=sys.stderr)
            # Recovery never publishes sys.boot_completed.
            if options.boot_mode != "recovery":
                booted = wait_for_boot_completed(
                    adb, serial, guest, options.boot_timeout
                )
                if booted:
                    print("Android reached sys.boot_completed.", file=sys.stderr)
                    # A pending update counts as applied from this moment, so
                    # whatever ends the run after it, the next release updates
                    # from this data rather than restoring the snapshot.
                    android_instance.booted(instance, identity)
                    if options.provision:
                        provision_guest(adb, serial)
                else:
                    print("Android did not reach sys.boot_completed.",
                          file=sys.stderr)
            # A guest that restarts itself (adb reboot, a power-menu restart,
            # a tradefed reboot) boots again inside this process, on the same
            # ADB serial, so one wait covers every boot until it powers off.
            return guest.wait()
        except KeyboardInterrupt:
            return 130
        except RequestedShutdown as shutdown:
            return 128 + shutdown.signum
        finally:
            stop_guest(guest)
            disconnect_adb(adb, serial)
            print(f"ADB disconnected: {serial}", file=sys.stderr)
            # Only a start that reached boot_completed and ended with the guest
            # powering off or being stopped (status 0) is complete.
            if booted and guest.returncode == 0:
                android_instance.complete(instance, identity)
                print("Instance start recorded as complete.", file=sys.stderr)
    except (
        LauncherError,
        android_instance.InstanceError,
        OSError,
        subprocess.CalledProcessError,
    ) as error:
        print(f"boot_android.py: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
