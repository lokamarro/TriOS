#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify an Android release's adb debugging setting and adbd's key rule.

The guest is the release on a fresh data disk, started with the macOS launcher
(tools/boot_android.py), or any running guest named by --serial. The contract
is android/lineage-24/README.md (the p0007 and p0011 adb topics):

- Developer options and USB debugging are off on a fresh data partition:
  `adb_enabled` is 0 (or unset) and `persist.sys.usb.config` does not hold
  `adb`: it is empty or `none`. At every boot init's property service fills an
  empty value, with `none` on a user build and with `adb` on a debuggable one,
  which is why this runner qualifies the user variant;
- turning USB debugging on records `adb` in `persist.sys.usb.config` and keeps
  the host's adbd running, the same process;
- turning it off drops `adb` from the property, ends the sessions by
  restarting adbd once, and leaves adbd running for the host, which
  reconnects;
- both settings survive a guest reboot;
- `adb root` on the user variant is refused at once, whatever the guest's key policy, with
  AOSP's answer for a production build: it neither announces a restart of adbd nor waits
  for one, it does not send the user to a Settings switch the build lacks, adbd stays the
  shell user, and the connection stays;
- when the guest checks keys (`ro.boot.kernarium.adb_auth` is 1, which the
  macOS launcher never sets, so only a guest the Kernarium runtime started),
  a second throwaway key stays unauthorized with no prompt while debugging is
  off, and raises the prompt while it is on. The host's own key is the one this
  runner's adb uses throughout and is never refused.

Every wait has a deadline; the guest's launcher is stopped through its own
process on any exit.

The guest is reached through the shared adb server, always named with `-s`,
and the runner ends with `adb disconnect`. The key rule is the one exception:
a second host key needs a server that does not share the host's key, so that
step starts one private server (PrivateServer) and stops it on every exit path.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from typing import Callable

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
LAUNCHER = REPOSITORY / "tools" / "boot_android.py"
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import slot_ports  # noqa: E402

DEFAULT_ADB_PORT = 5575
SETTLE_SECONDS = 30
SHARED_SERVER_PORT = 5037
KILL_SERVER_SECONDS = 15
# `adb root` on a build that cannot run adbd as root answers in well under a second. A guest
# that announces "restarting adbd as root" makes the client wait about 45 s for a restart
# that never comes, so any answer slower than this is the defect.
ROOT_ANSWER_SECONDS = 20
ROOT_RESTART_LINE = "restarting adbd as root"
# AOSP's answer to `adb root` on a build that is not debuggable. LineageOS answers with its stored
# setting's message ("ADB Root access is disabled by system setting - enable in Settings ...")
# when its adb_root service replies; route p0011 tests the build first.
ROOT_REFUSAL_LINE = "adbd cannot run as root in production builds"
SHELL_UID = "2000"
RECONNECT_SECONDS = 90
REFUSAL_LINE = "kernarium: key refused, debugging is off"
USB_CONFIG = "persist.sys.usb.config"
PROMPT_ACTIVITY = "UsbDebuggingActivity"


class Failure(RuntimeError):
    pass


class PrivateServer:
    """The one private adb server of this runner, stopped on every exit path.

    Only the key rule needs it: the guest must see a host key it does not know, and a
    server owns its key. It never serves USB devices or mDNS (ADB_USB=0, ADB_MDNS=0), its
    port is never the shared server's (kill-server on that would end everyone's
    connections), and with GUEST_SLOT_PORT_BASE set the port lies inside the slot's range,
    where the slot wrapper stops a server that outlives this runner.
    """

    running: list["PrivateServer"] = []

    def __init__(self, adb: str, port: int, key: pathlib.Path) -> None:
        if port == SHARED_SERVER_PORT:
            raise ValueError("the private adb server must not use the shared server's port")
        self.adb = adb
        self.port = port
        self.environment = {name: value for name, value in os.environ.items()
                            if name not in ("ADB_SERVER_SOCKET", "ANDROID_ADB_SERVER_ADDRESS")}
        self.environment.update({"ANDROID_ADB_SERVER_PORT": str(port), "ADB_VENDOR_KEYS": str(key),
                                 "ADB_USB": "0", "ADB_MDNS": "0"})
        # Registered before the server can start, so that no path leaves it running.
        PrivateServer.running.append(self)

    def run(self, *arguments: str, timeout: int = 30) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, *arguments], env=self.environment, text=True,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=timeout, check=False)

    def stop(self) -> None:
        if self in PrivateServer.running:
            PrivateServer.running.remove(self)
        try:
            self.run("kill-server", timeout=KILL_SERVER_SECONDS)
        except (subprocess.TimeoutExpired, OSError):
            pass


def stop_private_servers() -> None:
    for server in list(PrivateServer.running):
        server.stop()


def private_server_port() -> int:
    """A free loopback port: in the slot's range when the runner holds a guest slot."""
    base = slot_ports.slot_base()
    if base is None:
        return free_loopback_port()
    # The slot's wrapper stops an adb server listening in the slot's range. The top of the
    # range is clear of the guest's own forward at the bottom.
    first, last = slot_ports.PRIVATE_SERVER_FLOOR, slot_ports.SLOT_PORTS - 1
    for port in range(base + last, base + first - 1, -1):
        if port_is_free(port):
            return port
    raise Failure(f"no free port in {base}+{first}..{last} for the private adb server")


TERMINATION_SIGNALS = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)


def ignore_termination() -> None:
    """Cleanup runs to its own deadlines; a second signal must not cut it short."""
    for name in TERMINATION_SIGNALS:
        signal.signal(name, signal.SIG_IGN)


def terminate_on_signal(signum: int, _frame: object) -> None:
    """End the run through its finally clauses, which stop the private server."""
    ignore_termination()
    raise SystemExit(128 + signum)


def usb_debugging_recorded(value: str) -> bool:
    """adbd's DebuggingEnabled() rule for the property: `adb` is one of its comma-separated functions.

    Off is any value without it, the empty string AdbService writes when the setting is turned off
    and the `none` that init writes in its place at the next boot of a user build.
    """
    return "adb" in value.split(",")


class Device:
    """The guest as the host's adb sees it, over one server and one key."""

    def __init__(self, adb: str, serial: str, environment: dict[str, str]) -> None:
        self.adb = adb
        self.serial = serial
        self.environment = environment

    def run(self, *arguments: str, timeout: int = 30) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, *arguments], env=self.environment, text=True,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=timeout, check=False)

    def connect(self) -> bool:
        try:
            self.run("connect", self.serial, timeout=15)
            state = self.run("-s", self.serial, "get-state", timeout=15)
        except subprocess.TimeoutExpired:
            return False
        return state.returncode == 0 and state.stdout.strip() == "device"

    def shell(self, command: str, timeout: int = 30) -> str:
        result = self.run("-s", self.serial, "shell", command, timeout=timeout)
        if result.returncode != 0:
            raise Failure(f"'{command}' exited {result.returncode}: {result.stdout.strip()}")
        return result.stdout.strip()

    def getprop(self, name: str) -> str:
        return self.shell(f"getprop {name}")

    def setting(self, name: str) -> str:
        return self.shell(f"settings get global {name}")

    def adbd_pid(self) -> str:
        return self.shell("pidof adbd")

    def turn_debugging_off(self) -> None:
        """Set adb_enabled to 0; the restart that follows may end this very session."""
        try:
            self.run("-s", self.serial, "shell", "settings put global adb_enabled 0", timeout=30)
        except subprocess.TimeoutExpired:
            pass

    def devices_line(self) -> str:
        return listed_state(self.run("devices", timeout=15).stdout, self.serial)


def listed_state(listing: str, serial: str) -> str:
    """The state `adb devices` shows for the serial, or `absent`."""
    for line in listing.splitlines():
        if line.startswith(serial):
            return line.split()[-1]
    return "absent"


def wait_for(description: str, probe: Callable[[], bool], seconds: int,
             guest: subprocess.Popen | None = None) -> None:
    """Poll the probe until it is true; a probe that raises is not true yet."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if guest is not None and guest.poll() is not None:
            raise Failure(f"the guest's launcher exited ({guest.returncode}) while waiting for {description}")
        try:
            if probe():
                return
        except (Failure, subprocess.TimeoutExpired):
            pass
        time.sleep(1)
    raise Failure(f"{description}: not within {seconds} s")


def boot_id(device: Device) -> str:
    return device.shell("cat /proc/sys/kernel/random/boot_id")


def wait_booted(device: Device, seconds: int, guest: subprocess.Popen | None,
                previous_boot: str | None = None) -> None:
    def booted() -> bool:
        if not device.connect() or device.getprop("sys.boot_completed") != "1":
            return False
        return previous_boot is None or boot_id(device) != previous_boot
    wait_for("the guest to be booted", booted, seconds, guest)


def reboot(device: Device, seconds: int, guest: subprocess.Popen | None) -> None:
    previous = boot_id(device)
    device.run("-s", device.serial, "reboot", timeout=30)
    wait_booted(device, seconds, guest, previous)


def free_loopback_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def port_is_free(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        try:
            probe.bind(("127.0.0.1", port))
        except OSError:
            return False
        return True


def check_fresh(device: Device) -> None:
    enabled = device.setting("adb_enabled")
    if enabled not in ("0", "null"):
        raise Failure(f"adb_enabled is {enabled!r} on a fresh data partition")
    recorded = device.getprop(USB_CONFIG)
    if usb_debugging_recorded(recorded):
        raise Failure(f"{USB_CONFIG} is {recorded!r} on a fresh data partition, expected no adb (empty or none)")
    developer = device.setting("development_settings_enabled")
    if developer not in ("0", "null"):
        raise Failure(f"Developer options are on ({developer!r}) on a fresh data partition")
    if device.getprop("init.svc.adbd") != "running":
        raise Failure("adbd is not running for the host")


def check_root_refused(device: Device) -> None:
    """A user build never runs adbd as root: `adb root` is refused at once and the connection stays.

    Lineage's adbd asks its adb_root service whether rooted debugging is on; a guest whose boot
    policy turned that on for a non-debuggable build made adbd say it was restarting as root,
    set service.adb.root, and leave the client waiting for a restart no init trigger performs.
    The answer is AOSP's for a production build, not the service's: a user build has no
    Settings switch for the message to point at.
    """
    if device.getprop("ro.debuggable") != "0":
        print("adb root: skipped, this is a debuggable build")
        return
    pid = device.adbd_pid()
    try:
        answer = device.run("-s", device.serial, "root", timeout=ROOT_ANSWER_SECONDS)
    except subprocess.TimeoutExpired:
        raise Failure(f"`adb root` did not answer within {ROOT_ANSWER_SECONDS} s: adbd announced a "
                      f"restart as root that a {device.getprop('ro.build.type')} build never performs")
    if ROOT_RESTART_LINE in answer.stdout:
        raise Failure(f"`adb root` said {ROOT_RESTART_LINE!r} on a non-debuggable build: {answer.stdout.strip()!r}")
    if ROOT_REFUSAL_LINE not in answer.stdout:
        raise Failure(f"`adb root` on a non-debuggable build did not answer {ROOT_REFUSAL_LINE!r}: "
                      f"{answer.stdout.strip()!r}")
    if device.devices_line() != "device":
        raise Failure(f"`adb root` cost the connection: the guest is {device.devices_line()!r}")
    if device.shell("id -u") != SHELL_UID:
        raise Failure("adbd is not the shell user after `adb root` on a non-debuggable build")
    if device.adbd_pid() != pid:
        raise Failure("`adb root` restarted adbd on a non-debuggable build")
    print(f"adb root: refused at once ({answer.stdout.strip()!r}), adbd untouched, connection kept")


def check_toggle(device: Device) -> None:
    pid = device.adbd_pid()
    device.shell("settings put global adb_enabled 1")
    wait_for("persist.sys.usb.config to record the debugging setting",
             lambda: device.getprop("persist.sys.usb.config") == "adb", SETTLE_SECONDS)
    if device.setting("adb_enabled") != "1":
        raise Failure("adb_enabled is not 1 after turning USB debugging on")
    if device.adbd_pid() != pid:
        raise Failure("turning debugging on restarted adbd")
    print("toggle on: recorded, adbd untouched")

    device.turn_debugging_off()
    # The restart ends every session, the host's included.
    wait_for("adbd to restart and the host to reconnect",
             lambda: device.connect() and device.adbd_pid() != pid, RECONNECT_SECONDS)
    wait_for("persist.sys.usb.config to stop recording adb",
             lambda: not usb_debugging_recorded(device.getprop(USB_CONFIG)), SETTLE_SECONDS)
    if device.getprop("init.svc.adbd") != "running":
        raise Failure("adbd is not running after debugging was turned off")
    if device.setting("adb_enabled") != "0":
        raise Failure("adb_enabled is not 0 after turning USB debugging off")
    print("toggle off: adb dropped, adbd restarted once and serving the host")


def check_reboot(device: Device, seconds: int, guest: subprocess.Popen | None) -> None:
    device.shell("settings put global adb_enabled 1")
    wait_for("persist.sys.usb.config to record the setting",
             lambda: device.getprop("persist.sys.usb.config") == "adb", SETTLE_SECONDS)
    reboot(device, seconds, guest)
    wait_for("adb_enabled to be 1 after the reboot", lambda: device.setting("adb_enabled") == "1",
             SETTLE_SECONDS)
    if device.getprop("persist.sys.usb.config") != "adb":
        raise Failure("persist.sys.usb.config lost 'adb' across a reboot")
    device.turn_debugging_off()
    wait_for("the host to reconnect after turning debugging off",
             lambda: device.connect() and not usb_debugging_recorded(device.getprop(USB_CONFIG)),
             RECONNECT_SECONDS)
    reboot(device, seconds, guest)
    wait_for("adb_enabled to be 0 after the reboot", lambda: device.setting("adb_enabled") == "0",
             SETTLE_SECONDS)
    if usb_debugging_recorded(device.getprop(USB_CONFIG)):
        raise Failure("persist.sys.usb.config came back with adb after a reboot")
    print("reboot: both settings persist")


def check_key_rule(device: Device, scratch: pathlib.Path) -> None:
    """A second key: refused with no prompt while debugging is off, prompted while on."""
    key = scratch / "throwaway_adbkey"
    stranger = PrivateServer(device.adb, private_server_port(), key)
    serial = device.serial

    def stranger_state() -> str:
        stranger.run("connect", serial, timeout=15)
        return listed_state(stranger.run("devices", timeout=15).stdout, serial)

    try:
        created = stranger.run("keygen", str(key), timeout=30)
        if created.returncode != 0 or not key.is_file():
            raise Failure(f"adb keygen failed: {created.stdout.strip()}")
        device.shell("logcat -c")
        wait_for("the second key to reach adbd", lambda: stranger_state() == "unauthorized", SETTLE_SECONDS)
        time.sleep(10)
        if stranger_state() != "unauthorized":
            raise Failure("the second key authenticated while debugging is off")
        if REFUSAL_LINE not in device.shell("logcat -d"):
            raise Failure(f"adbd did not log {REFUSAL_LINE!r} for the second key")
        if PROMPT_ACTIVITY in device.shell("dumpsys activity activities"):
            raise Failure("the second key raised the debugging prompt while debugging is off")
        print("key rule, debugging off: unauthorized, refused, no prompt")

        device.shell("settings put global adb_enabled 1")
        wait_for("persist.sys.usb.config to record the setting",
                 lambda: device.getprop("persist.sys.usb.config") == "adb", SETTLE_SECONDS)
        stranger.run("disconnect", serial, timeout=15)
        wait_for("the second key to be offered the prompt",
                 lambda: stranger_state() == "unauthorized"
                 and PROMPT_ACTIVITY in device.shell("dumpsys activity activities"),
                 SETTLE_SECONDS)
        print("key rule, debugging on: unauthorized until accepted, prompt raised")
    finally:
        stranger.stop()
        device.turn_debugging_off()


def stop_launcher(guest: subprocess.Popen) -> None:
    if guest.poll() is None:
        guest.send_signal(signal.SIGTERM)
        try:
            guest.wait(timeout=120)
        except subprocess.TimeoutExpired:
            guest.kill()
            guest.wait(timeout=30)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--release", type=pathlib.Path, help="release directory (default: the accepted release)")
    parser.add_argument("--serial", help="a running guest on a fresh data partition (host:port) that "
                        "accepts this adb's key; no guest is launched")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--emulator", type=pathlib.Path, help="the Android macOS executable (the launcher's default)")
    parser.add_argument("--adb-port", type=int, default=slot_ports.slot_port(slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_ADB_PORT})")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--boot-timeout", type=int, default=900)
    return parser


def main() -> int:
    options = build_parser().parse_args()

    adb = shutil.which(options.adb) or options.adb
    guest: subprocess.Popen | None = None
    for name in TERMINATION_SIGNALS:
        signal.signal(name, terminate_on_signal)
    with tempfile.TemporaryDirectory(prefix="android-adb-debugging-") as scratch_name:
        scratch = pathlib.Path(scratch_name)
        environment = dict(os.environ)
        serial = options.serial or f"127.0.0.1:{options.adb_port}"
        log_path = scratch / "launcher.log"
        try:
            if not options.serial:
                command = [sys.executable, str(LAUNCHER), "--instance", str(scratch / "instance"),
                           "--adb", adb, "--adb-port", str(options.adb_port), "--display", "offscreen",
                           "--cpus", str(options.cpus), "--boot-timeout", str(options.boot_timeout)]
                if options.release:
                    command += ["--release", str(options.release)]
                if options.emulator:
                    command += ["--emulator", str(options.emulator)]
                launcher_environment = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}
                with open(log_path, "wb") as log:
                    guest = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                             stderr=subprocess.STDOUT, env=launcher_environment)
            device = Device(adb, serial, environment)
            wait_booted(device, options.boot_timeout, guest)
            check_fresh(device)
            print("fresh data: debugging off, adbd serving the host")
            check_root_refused(device)
            check_toggle(device)
            check_reboot(device, options.boot_timeout, guest)
            if device.getprop("ro.boot.kernarium.adb_auth") == "1":
                check_key_rule(device, scratch)
            else:
                print("key rule: skipped, this guest does not check keys (ro.boot.kernarium.adb_auth is not 1)")
            print("PASS")
            return 0
        except (Failure, subprocess.TimeoutExpired, OSError) as problem:
            print(f"FAIL: {problem}", file=sys.stderr)
            if log_path.is_file():
                print("launcher log tail:\n" + "\n".join(log_path.read_text(errors="replace").splitlines()[-30:]),
                      file=sys.stderr)
            return 1
        finally:
            ignore_termination()
            stop_private_servers()
            if guest is not None:
                stop_launcher(guest)
                # The shared server stays up; only the guest this run launched is dropped.
                # A guest named by --serial is someone else's and stays connected.
                subprocess.run([adb, "disconnect", serial], env=environment, stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30, check=False)


if __name__ == "__main__":
    raise SystemExit(main())
