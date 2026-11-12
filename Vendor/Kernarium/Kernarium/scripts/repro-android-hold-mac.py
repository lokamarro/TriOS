#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Drive an Android guest through the application's background edge on a Mac.

The application runs in Designed for iPad mode as the host of
KernariumTests/AndroidHoldReproTests (run-ios-mac.sh), which applies the real
background policy to the running guest when this script asks, and this script
watches the guest through adb: a swipe loop keeps a launcher drawing, the
guest is sent to the background for a while, and the guest is checked for the
tasks that wait on the held GPU and for the framework's application-not-
responding report. A Mac never revokes the GPU, so this shows the guest's side
of the edge only; the GPU hold itself is the same.

It needs adb and a guest bundle, which run-ios-mac.sh assembles once from the
accepted Android release (make-guest-bundle.py) and keeps, with its writable
disk, in the application's container as --bundle NAME. The host refuses a
disk whose partition layout is not the one it expects ("fixed GPT partition
layout differs", android_boot.cpp), so a release built before a layout change
needs a bundle of the current layout copied there first; an existing bundle
is reused. The script uses the shared adb server, names its guest with -s and
disconnects it at the end; it never stops a server.

  repro-android-hold-mac.py [--no-build] [--background-seconds N]
                            [--expect {pass,fail}] [--output DIR]

--expect fail is for a build that still has the defect: the script succeeds
when it reproduces it. Exit status: 0 as expected, 1 not as expected, 2 for a
setup failure.
"""

from __future__ import annotations

import argparse
import json
import os
import plistlib
import signal
import subprocess
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
RUN_SCRIPT = ROOT / "Kernarium/scripts/run-ios-mac.sh"
BUNDLE_ID = "com.imxieyi.Kernarium"
FORWARD_PORT = 5589
SERIAL = f"127.0.0.1:{FORWARD_PORT}"
CHANNEL = "hold-repro"
# The processes whose survival says the framework itself did not fall over.
SURVIVORS = "system_server surfaceflinger"
# A cached process the framework trims has died too, which is not a finding;
# one the user could see or depend on is. The log also holds the adb service
# line of this very search, which the pattern would match.
REPORT_PATTERN = (
    r"WATCHDOG|Watchdog.*(killing|Triggering)|ANR in|am_anr|am_crash|"
    r"FATAL EXCEPTION|Process .* has died: (top|pers|fg|vis|prcp|psvc|bfgs)"
)


def fail(message: str, status: int = 2) -> None:
    print(f"repro-android-hold-mac.py: {message}", file=sys.stderr)
    raise SystemExit(status)


def say(message: str) -> None:
    print(f"{time.strftime('%H:%M:%S')} {message}", flush=True)


def adb(*arguments: str, timeout: float = 60.0) -> tuple[int, str]:
    command = ["adb", "-s", SERIAL]
    try:
        completed = subprocess.run(
            [*command, *arguments],
            capture_output=True,
            text=True,
            timeout=timeout,
            stdin=subprocess.DEVNULL,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return 124, ""
    return completed.returncode, (completed.stdout + completed.stderr).replace(
        "\r", ""
    )


def guest(command: str, timeout: float = 60.0) -> tuple[int, str]:
    return adb("shell", command, timeout=timeout)


def connect() -> bool:
    try:
        completed = subprocess.run(
            ["adb", "connect", SERIAL],
            capture_output=True,
            text=True,
            timeout=20,
            stdin=subprocess.DEVNULL,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return False
    return "connected" in completed.stdout


def wait_until(predicate, timeout: float, interval: float = 2.0) -> bool:
    deadline = time.monotonic() + timeout
    while True:
        if predicate():
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(interval)


def data_container() -> Path | None:
    root = Path.home() / "Library/Containers"
    newest: tuple[float, Path] | None = None
    for metadata in root.glob("*/.com.apple.containermanagerd.metadata.plist"):
        try:
            with metadata.open("rb") as handle:
                payload = plistlib.load(handle)
        except (OSError, plistlib.InvalidFileException):
            continue
        if payload.get("MCMMetadataIdentifier") != BUNDLE_ID:
            continue
        data = metadata.parent / "Data"
        if not data.is_dir():
            continue
        stamp = metadata.parent.stat().st_mtime
        if newest is None or stamp > newest[0]:
            newest = (stamp, data)
    return None if newest is None else newest[1]


class Channel:
    """The files the hosted test and this script ask and answer through."""

    def __init__(self) -> None:
        self.directory: Path | None = None

    def locate(self) -> Path | None:
        container = data_container()
        if container is None:
            return None
        self.directory = container / "Documents" / CHANNEL
        return self.directory

    def present(self, name: str) -> bool:
        return self.directory is not None and (self.directory / name).exists()

    def ask(self, name: str) -> None:
        assert self.directory is not None
        (self.directory / name).write_text("1", encoding="utf-8")

    def answer(self, name: str, timeout: float) -> dict | None:
        if not wait_until(lambda: self.present(name), timeout, 0.2):
            return None
        assert self.directory is not None
        for _ in range(10):
            try:
                return json.loads((self.directory / name).read_text("utf-8"))
            except (OSError, ValueError):
                time.sleep(0.1)
        return None


def own_application_pids(derived_data: Path) -> list[int]:
    """The pids of the application this run built, by the path it runs from."""
    products = str(derived_data / "Build/Products")
    listing = subprocess.run(
        ["ps", "-axo", "pid=,command="],
        capture_output=True,
        text=True,
        stdin=subprocess.DEVNULL,
        check=False,
    ).stdout
    pids = []
    for line in listing.splitlines():
        pid, _, command = line.strip().partition(" ")
        if pid.isdigit() and command.startswith(products) and "Kernarium" in command:
            pids.append(int(pid))
    return pids


def sample(label: str, baseline_pids: str) -> dict:
    """What the guest looks like now, through adb, each command bounded."""
    result: dict = {"label": label, "time": time.strftime("%H:%M:%S")}
    rc, output = guest("echo ok", timeout=40)
    result["adb"] = output.strip() == "ok"
    if not result["adb"]:
        return result
    _, stuck = guest(
        'ps -A -o PID,STAT,WCHAN:32,NAME | grep -E "^ *[0-9]+ +D"', timeout=40
    )
    result["d_state"] = [line.strip() for line in stuck.splitlines() if line.strip()]
    _, wakefulness = guest("dumpsys power | grep mWakefulness=", timeout=40)
    result["wakefulness"] = wakefulness.strip().splitlines()[0] if wakefulness.strip() else ""
    _, pids = guest(f"pidof {SURVIVORS}", timeout=40)
    result["pids"] = pids.strip()
    result["survivors_kept"] = pids.strip() == baseline_pids
    _, reports = guest(
        f'logcat -b all -d -v threadtime | grep -E "{REPORT_PATTERN}" | '
        'grep -v " adbd "',
        timeout=60,
    )
    result["reports"] = [
        line.strip() for line in reports.splitlines() if line.strip()
    ][:12]
    return result


def blocked_in_gpu_queue() -> int | None:
    """How many blocked tasks wait for room in the guest's GPU control queue.

    The count comes from sysrq-w and the kernel log, which need root: only the
    userdebug diagnostics image has it. On a user image (ro.debuggable 0) the
    sample is skipped; the D-state sample and the framework's reports still
    judge the hold.
    """
    _, debuggable = guest("getprop ro.debuggable", timeout=30)
    if debuggable.strip() != "1":
        return None
    _, output = guest(
        'su 0 sh -c "echo 1 > /proc/sys/kernel/sysrq; echo w > '
        '/proc/sysrq-trigger; dmesg | tail -n 400"',
        timeout=60,
    )
    return output.count("virtio_gpu_queue_fenced_ctrl_buffer")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--bundle", default="hold-repro")
    parser.add_argument("--background-seconds", type=int, default=90)
    parser.add_argument("--load-seconds", type=int, default=20)
    parser.add_argument("--boot-timeout", type=int, default=900)
    parser.add_argument("--expect", choices=("pass", "fail"), default="pass")
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()

    if subprocess.run(["which", "adb"], capture_output=True).returncode != 0:
        fail("adb is not installed")
    output = (
        arguments.output
        or ROOT / ".build/repro-android-hold-mac" / time.strftime("%Y%m%d-%H%M%S")
    )
    output.mkdir(parents=True, exist_ok=True)
    derived_data = ROOT / ".build/kernarium-xcode-derived"
    total = arguments.boot_timeout + arguments.background_seconds + 900

    command = [
        str(RUN_SCRIPT),
        "--guest-bundle", arguments.bundle,
        "--android-automation",
        "--only-testing", "KernariumTests/AndroidHoldReproTests",
        "--test-env", f"KERNARIUM_HOLD_REPRO_DIR={CHANNEL}",
        "--test-env", f"KERNARIUM_HOLD_REPRO_SECONDS={total}",
        "--test-env", "KERNARIUM_ANDROID_ADB_AUTH=0",
        "--test-env", f"KERNARIUM_HOST_FORWARD=tcp:127.0.0.1:{FORWARD_PORT}-5555",
        "--test-env", "KERNARIUM_VCPU_COUNT=4",
        "--test-env", "KERNARIUM_GUEST_MEMORY_MIB=2048",
        "--test-env", "KERNARIUM_GPU_AUTO_RESIZE=1",
    ]
    if arguments.no_build:
        command.append("--no-build")

    launcher_log = (output / "launch.log").open("w")
    channel = Channel()
    verdict: dict = {"expect": arguments.expect}
    launcher = None
    try:
        say(f"launching the application: {RUN_SCRIPT.name} (log {output}/launch.log)")
        launcher = subprocess.Popen(
            command,
            stdout=launcher_log,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            start_new_session=True,
        )

        def ready() -> bool:
            if launcher.poll() is not None:
                fail(f"the launcher ended with status {launcher.returncode}")
            return channel.locate() is not None and channel.present("ready")

        if not wait_until(ready, arguments.boot_timeout, 3.0):
            fail("the guest never ran in the application", 2)
        say("the application's guest runs; waiting for Android")

        def booted() -> bool:
            if not connect():
                return False
            rc, boot = guest("getprop sys.boot_completed", timeout=30)
            return boot.strip() == "1"

        if not wait_until(booted, arguments.boot_timeout, 5.0):
            fail("Android did not finish booting, or adb did not connect", 2)
        say("Android booted")
        time.sleep(30)

        guest("logcat -b all -c")
        guest(
            "settings put system screen_off_timeout 1800000; "
            "input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard"
        )
        _, baseline_pids = guest(f"pidof {SURVIVORS}")
        baseline_pids = baseline_pids.strip()
        _, awake = guest("dumpsys power | grep mWakefulness=")
        awake = awake.strip().splitlines()[0] if awake.strip() else ""
        say(f"survivors {baseline_pids!r}, {awake}")
        verdict["baseline_pids"] = baseline_pids
        verdict["baseline_wakefulness"] = awake
        guest(
            'nohup sh -c "while :; do input swipe 560 600 560 200 250; '
            'input swipe 560 200 560 600 250; done" </dev/null >/dev/null 2>&1 &'
        )
        say(f"swipe loop running; loading for {arguments.load_seconds} s")
        time.sleep(arguments.load_seconds)

        say("asking for the background edge")
        started = time.monotonic()
        channel.ask("edge-background")
        edge = channel.answer("edge-background.done", 60)
        if edge is None:
            fail("the application never answered the background edge", 2)
        verdict["edge_background"] = edge
        say(
            f"edge handled in {edge['handler_ms']} ms, phase {edge['phase']}, "
            f"calls {edge['calls']}"
        )

        samples = []
        offsets = sorted({10, 30, 60, arguments.background_seconds})
        offsets = [offset for offset in offsets if offset <= arguments.background_seconds]
        for offset in offsets:
            remaining = started + offset - time.monotonic()
            if remaining > 0:
                time.sleep(remaining)
            taken = sample(f"+{offset} s", baseline_pids)
            samples.append(taken)
            say(
                f"sample {taken['label']}: adb {taken['adb']}, "
                f"D-state {len(taken.get('d_state', []))}, "
                f"{taken.get('wakefulness', '?')}, reports "
                f"{len(taken.get('reports', []))}"
            )
        verdict["samples"] = samples
        verdict["tasks_waiting_on_the_gpu_queue"] = (
            blocked_in_gpu_queue() if samples[-1]["adb"] else None
        )

        say("asking for the foreground edge")
        channel.ask("edge-foreground")
        back = channel.answer("edge-foreground.done", 120)
        verdict["edge_foreground"] = back
        say("waiting for adb to answer after the return")
        wait_until(lambda: connect() and guest("echo ok", timeout=30)[1].strip() == "ok", 180, 3.0)
        time.sleep(15)
        after = sample("after the return", baseline_pids)
        verdict["after"] = after
        say(
            f"after the return: adb {after['adb']}, D-state "
            f"{len(after.get('d_state', []))}, {after.get('wakefulness', '?')}, "
            f"reports {len(after.get('reports', []))}"
        )
        channel.ask("finish")
    finally:
        pids = []
        if launcher is not None:
            try:
                launcher.wait(timeout=120)
            except subprocess.TimeoutExpired:
                say("the launcher did not end; stopping the pids this run started")
                pids = own_application_pids(derived_data)
                for pid in [launcher.pid, *pids]:
                    try:
                        os.kill(pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                time.sleep(20)
                for pid in own_application_pids(derived_data):
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
        launcher_log.close()
        subprocess.run(
            ["adb", "disconnect", SERIAL],
            capture_output=True,
            timeout=20,
            stdin=subprocess.DEVNULL,
            check=False,
        )

    held = "guest GPU requests held on background" in (output / "launch.log").read_text(
        errors="replace"
    )
    problems = []
    if not held:
        problems.append("the log has no GPU hold line")
    samples = verdict["samples"]
    if any(not taken["adb"] for taken in samples):
        problems.append("adb went silent during the hold")
    if any(taken.get("d_state") for taken in samples):
        problems.append("tasks sat in D state during the hold")
    if verdict["tasks_waiting_on_the_gpu_queue"]:
        problems.append("tasks waited for room in the GPU control queue")
    if any(taken.get("reports") for taken in samples) or verdict["after"].get("reports"):
        problems.append("the framework reported a watchdog, an ANR or a crash")
    if not all(taken.get("survivors_kept", True) for taken in samples):
        problems.append("system_server or surfaceflinger changed pid")
    if "Awake" not in verdict["after"].get("wakefulness", ""):
        problems.append("the screen did not come back on after the return")
    verdict["problems"] = problems
    (output / "verdict.json").write_text(
        json.dumps(verdict, indent=2, sort_keys=True), encoding="utf-8"
    )
    say(f"problems: {problems or 'none'}")
    reproduced = bool(problems)
    if arguments.expect == "fail":
        say("REPRODUCED the defect" if reproduced else "did NOT reproduce the defect")
        return 0 if reproduced else 1
    say("PASS" if not reproduced else "FAIL")
    return 1 if reproduced else 0


if __name__ == "__main__":
    raise SystemExit(main())
