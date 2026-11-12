#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the Apple playback sink and the Apple microphone for the iOS Simulator
and run their interruption tests there.

ios_interruption_tests.mm plays through the sink, and ios_microphone_tests.mm
starts the microphone over hooked AVFAudio (no real input, no permission
prompt); each posts the audio-session notifications iOS posts when another app
interrupts the session and prints one PASS or FAIL line per check. This runner
compiles each with the code it tests, the session and the other files it uses,
and runs them in a simulator device of its own, which it creates on the newest
iOS runtime at or above the deployment target and deletes afterwards; it never
uses a device it did not create unless --device names one. A new device keeps
several cores busy for about two minutes after it boots, so the tests start
once its processes have settled. The runner passes when both tests do, and
exits 77, which CTest reports as skipped, when the host has no iOS Simulator
runtime at the deployment target.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import json
import os
import pathlib
import re
import signal
import subprocess
import sys
import tempfile
import time

SKIPPED = 77
RUNTIME = re.compile(r"com\.apple\.CoreSimulator\.SimRuntime\.iOS-(\d+)-(\d+)")
# Every step is bounded, and the steps together end before CTest's TIMEOUT (840)
# would kill the runner, so that the device it created is always deleted.
DEADLINE_SECONDS = 780
CLEANUP_SECONDS = 90
BOOT_TIMEOUT_SECONDS = 240
COMPILE_TIMEOUT_SECONDS = 300
RUN_TIMEOUT_SECONDS = 240
# The device has settled when its processes use less than one core in this many
# samples in a row, five seconds apart.
SETTLE_TIMEOUT_SECONDS = 240
SETTLE_PERCENT = 100.0
SETTLE_SAMPLES = 3


class Deadline:
    def __init__(self, seconds: int) -> None:
        self.end = time.monotonic() + seconds

    def bound(self, seconds: int, reserve: int = CLEANUP_SECONDS) -> int:
        """|seconds|, cut so that |reserve| seconds remain for cleanup."""
        remaining = int(self.end - time.monotonic()) - reserve
        if remaining <= 0:
            raise TimeoutError("the runner's deadline passed")
        return min(seconds, remaining)


def simctl(*arguments: str, timeout: int = 60) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(
            ["xcrun", "simctl", *arguments],
            check=False,
            capture_output=True,
            text=True,
            stdin=subprocess.DEVNULL,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as expired:
        # The exception carries the output read so far as bytes.
        output = expired.stdout or b""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        return subprocess.CompletedProcess(
            expired.cmd, 124, output, f"timed out after {timeout} s"
        )


def pick_runtime(floor: tuple[int, int]) -> tuple[str, str] | None:
    """The newest available iOS runtime at or above |floor|, and an iPhone device
    type it supports."""
    listing = json.loads(simctl("list", "runtimes", "available", "--json").stdout or "{}")
    best = None
    for runtime in listing.get("runtimes", []):
        match = RUNTIME.fullmatch(runtime.get("identifier", ""))
        if match is None or not runtime.get("isAvailable"):
            continue
        version = (int(match[1]), int(match[2]))
        phones = [
            device_type["identifier"]
            for device_type in runtime.get("supportedDeviceTypes", [])
            if device_type.get("productFamily") == "iPhone"
        ]
        if version >= floor and phones and (best is None or version > best[0]):
            best = (version, runtime["identifier"], phones[0])
    return None if best is None else (best[1], best[2])


def device_state(udid: str) -> str | None:
    listing = json.loads(simctl("list", "devices", "--json").stdout or "{}")
    for devices in listing.get("devices", {}).values():
        for device in devices:
            if device.get("udid") == udid:
                return device.get("state")
    return None


def device_cpu(udid: str) -> float | None:
    """The summed %CPU of a booted device's processes: the descendants of its
    launchd_sim, whose arguments name the device's directory. None when there
    is no such process."""
    listing = subprocess.run(
        ["ps", "-A", "-o", "pid=,ppid=,pcpu=,args="],
        check=False,
        capture_output=True,
        text=True,
        stdin=subprocess.DEVNULL,
        timeout=30,
        env={**os.environ, "LC_ALL": "C"},
    ).stdout
    children: dict[int, list[int]] = {}
    usage: dict[int, float] = {}
    root = None
    for line in listing.splitlines():
        fields = line.split(None, 3)
        if len(fields) < 4:
            continue
        pid, parent = int(fields[0]), int(fields[1])
        usage[pid] = float(fields[2])
        children.setdefault(parent, []).append(pid)
        program = os.path.basename(fields[3].split(" ", 1)[0])
        if program == "launchd_sim" and f"/{udid}/" in fields[3]:
            root = pid
    if root is None:
        return None
    total = 0.0
    pending = list(children.get(root, []))
    while pending:
        pid = pending.pop()
        total += usage.get(pid, 0.0)
        pending.extend(children.get(pid, []))
    return total


def settle(udid: str, deadline: Deadline) -> None:
    """Wait until the device's own processes have gone quiet, or for at most
    SETTLE_TIMEOUT_SECONDS."""
    start = time.monotonic()
    end = start + deadline.bound(SETTLE_TIMEOUT_SECONDS)
    quiet = 0
    while time.monotonic() < end:
        busy = device_cpu(udid)
        if busy is None:
            print("note: the simulator's processes were not found; not waiting for them")
            return
        quiet = quiet + 1 if busy < SETTLE_PERCENT else 0
        if quiet >= SETTLE_SAMPLES:
            print(f"simulator settled after {time.monotonic() - start:.0f} s")
            return
        time.sleep(5)
    print(f"note: the simulator was still busy after {time.monotonic() - start:.0f} s")


def raise_on_signal(number: int, _frame: object) -> None:
    raise SystemExit(128 + number)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sink-directory", required=True, type=pathlib.Path)
    parser.add_argument("--include-directory", required=True, type=pathlib.Path)
    parser.add_argument("--deployment-target", required=True)
    parser.add_argument(
        "--device",
        metavar="UDID",
        help="run in this simulator device instead of one of the runner's own; the runner "
        "boots it if it is shut down and shuts it down again afterwards",
    )
    arguments = parser.parse_args()
    floor = tuple(int(part) for part in arguments.deployment_target.split("."))[:2]
    deadline = Deadline(DEADLINE_SECONDS)
    for number in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(number, raise_on_signal)

    chosen = None
    if arguments.device is None:
        chosen = pick_runtime(floor)
        if chosen is None:
            print(f"SKIP: no iOS Simulator runtime at iOS {arguments.deployment_target} or later")
            return SKIPPED
    elif device_state(arguments.device) is None:
        print(f"FAIL: no simulator device {arguments.device}")
        return 1

    here = pathlib.Path(__file__).resolve().parent
    sink = arguments.sink_directory
    shared = [here / "ios_session_notifications.mm", sink / "audio_session.mm"]
    tests = {
        "interruption_tests": [
            here / "ios_interruption_tests.mm",
            sink / "apple_playback_sink.mm",
            sink / "period_clock.cpp",
            sink / "virtio_sound_device.cpp",
            sink / "silent_capture_source.cpp",
        ],
        "microphone_tests": [
            here / "ios_microphone_tests.mm",
            sink / "apple_microphone_input.mm",
        ],
    }
    with tempfile.TemporaryDirectory(prefix="uml-audio-interruption-") as scratch:
        created = False
        shut_down_after = False
        status = 0
        try:
            if chosen is not None:
                runtime, device_type = chosen
                label = f"uml-audio-interruption-{os.getpid()}"
                made = simctl("create", label, device_type, runtime, timeout=deadline.bound(60))
                udid = made.stdout.strip()
                if made.returncode != 0 or not udid:
                    print(f"FAIL: creating simulator {label}: {made.stderr.strip()}")
                    return 1
                created = True
                print(f"simulator {label}: {device_type} on {runtime}")
            else:
                udid = arguments.device
            if device_state(udid) != "Booted":
                shut_down_after = True
                booted = simctl("boot", udid, timeout=deadline.bound(BOOT_TIMEOUT_SECONDS))
                if booted.returncode != 0:
                    print(f"FAIL: booting the simulator: {booted.stderr.strip()}")
                    return 1

            # The tests build while the device finishes booting.
            binaries = {}
            for name, sources in tests.items():
                binary = pathlib.Path(scratch) / name
                compile_command = [
                    "xcrun", "--sdk", "iphonesimulator", "clang++",
                    "-target", f"arm64-apple-ios{arguments.deployment_target}-simulator",
                    "-x", "objective-c++", "-std=c++20", "-fobjc-arc", "-O1",
                    "-Wall", "-Wextra", "-Werror",
                    f"-I{here}", f"-I{sink}", f"-I{arguments.include_directory}",
                    f"-I{arguments.include_directory / 'device' / 'vhost_user'}",
                    *(str(source) for source in sources + shared),
                    "-framework", "AVFAudio", "-framework", "Foundation",
                    "-o", str(binary),
                ]
                try:
                    built = subprocess.run(
                        compile_command,
                        check=False,
                        stdin=subprocess.DEVNULL,
                        timeout=deadline.bound(COMPILE_TIMEOUT_SECONDS),
                    )
                except subprocess.TimeoutExpired:
                    print(f"FAIL: building {name} for the iOS Simulator timed out")
                    return 1
                if built.returncode != 0:
                    print(f"FAIL: building {name} for the iOS Simulator")
                    return 1
                binaries[name] = binary

            ready = simctl("bootstatus", udid, timeout=deadline.bound(BOOT_TIMEOUT_SECONDS))
            if ready.returncode != 0:
                print(f"FAIL: waiting for the simulator to boot: {ready.stderr.strip()}")
                return 1
            if shut_down_after:
                settle(udid, deadline)
            for name, binary in binaries.items():
                result = simctl(
                    "spawn", udid, str(binary), timeout=deadline.bound(RUN_TIMEOUT_SECONDS)
                )
                print(f"== {name}")
                sys.stdout.write(result.stdout)
                sys.stderr.write(result.stderr)
                status = status or (0 if result.returncode == 0 else 1)
        except TimeoutError as error:
            print(f"FAIL: {error}")
            status = 1
        finally:
            # Cleanup has its own reserve and runs whatever stopped the steps.
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            signal.signal(signal.SIGINT, signal.SIG_IGN)
            signal.signal(signal.SIGHUP, signal.SIG_IGN)
            if shut_down_after or created:
                simctl("shutdown", udid, timeout=CLEANUP_SECONDS // 2)
            if created:
                deleted = simctl("delete", udid, timeout=CLEANUP_SECONDS // 2)
                if deleted.returncode != 0:
                    print(f"FAIL: deleting simulator {label}: {deleted.stderr.strip()}")
                    status = 1
        return status

if __name__ == "__main__":
    raise SystemExit(main())
