#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A Virtio sound stream starts afresh after a system suspend, a drop, a
guest stall or a drain.

The ALSA core restarts a stream that was suspended at offset 0, and the
Virtio sound driver's prepare must then start from nothing too: no position,
no queued period and no pending message of the stream before the suspend. A
suspend the driver kept the position across counted it as played, so the
stream's period completions moved the position two or three periods each and
the application wrote twice as fast as the sound plays. A stream paused
across a suspend could neither be recovered nor, after it was closed, be
opened again, because the driver still counted the periods the device had
held and the back-end had reset it; one paused after its queue had drained
met a device that kept its stopped stream. UML runs no virtio freeze, so the
driver releases such a stream on the device from its own suspend callback,
before the rings are disabled, and marks the substream for reconfiguration.

The same state is behind other failures, and the test covers them too. A
period written before a start is queued on the device, and a drop or a
prepare before the start must release it. A half-filled period must not keep
its length across a prepare. A stream that ends inside a period must drain.
And a guest stall longer than the buffer, while the device goes on playing,
must not lose a whole buffer of completions: the stream then carries on or
xruns.

The guest boots with the null audio output. A two-vCPU guest suspends itself
with an RTC alarm two seconds ahead, and virtio_sound_suspend_probe.py checks
the stream position after a suspend, a prepare after a half-filled period, a
prepare after a period queued before a start, drains that end inside a
period, a stream paused across a suspend with and without periods on the
device, and a reopen after one, with exact assertions; its header describes
each case. Then, in a one-vCPU guest for each of the probe's two stall cases,
the runner stops the guest through the management console for 300 ms while
the probe streams: with the stop threshold at the boundary the stream must
carry on, and with the default thresholds it xruns once and plays on at the
rate of audio. Every case must pass, and the guest must print no kernel
warning, oops or host fault report. The contract is the suspend paragraph of
kernel/common/README.md.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import shutil
import sys
import tempfile
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402
# The directory of this script is first on sys.path.
from run_guest_suspend_holds import ManagementConsole, drain  # noqa: E402


SUSPEND_VCPUS = 2
STALL_VCPUS = 1
BOOT_SECONDS = 180
PROBE_SECONDS = 240
STREAM_SECONDS = 60
RESULT_SECONDS = 60
# Longer than the 170 ms the stream's buffer lasts, so the device completes
# every queued period while the guest cannot look.
STALL_MILLISECONDS = 300
PROBE = "/opt/uml-test/virtio_sound_suspend_probe.py"
SUSPEND_EXIT = "PM: suspend exit"
# The probe suspends the guest once in each of these cases.
SUSPENDING_CASES = ("position", "paused", "pauseddrained", "pausedreopen")
STALL_CASES = ("stallboundary", "stalldefault")
FAILURE = re.compile(
    r"WARNING:|\bBUG\b|Oops|Call Trace:|Kernel panic|Darwin UML panic|"
    r"Darwin POSIX|Kernel mode fault|rcu: INFO"
)
CASE_FAILURE = re.compile(r"^VSND (\w+) FAIL: (.*?)\r?$", re.M)
DRIVER_COMPLAINT = re.compile(r"^.*virtio_snd.*$", re.M)
CASE_LINE = re.compile(r"^VSND .*?$", re.M)


def guest_for(arguments, vcpus: int) -> Guest:
    probe = (REPOSITORY / "tests/integration/kernel"
             / "virtio_sound_suspend_probe.py").read_bytes()
    return Guest(
        arguments.emulator,
        arguments.rootfs,
        "1G",
        "tmpfs",
        (f"ncpus={vcpus}", "--audio-output=null"),
        overlay_shell=True,
        engine=arguments.engine,
        payloads={PROBE: GuestFile(probe, 0o755)},
    )


def show_cases(text: str) -> None:
    """The probe's own report, whatever the verdict."""
    for line in CASE_LINE.findall(text):
        print(line.rstrip("\r"))


def show_driver_log(guest: Guest) -> None:
    """What the kernel logged about the sound device, for a failed run."""
    log = guest.run_command(
        "dmesg | grep -i -E 'virtio|snd|vhost|sound' | tail -n 60", 30
    )
    print("driver log:\n" + log.output)


def complaints_of(text: str) -> list[str]:
    """What the guest printed that no run may print, with the driver's own
    messages beside it."""
    failure = FAILURE.search(text)
    if failure is None:
        return []
    lines = DRIVER_COMPLAINT.findall(text)
    if lines:
        print("driver messages:\n" + "\n".join(lines))
    return [f"the guest printed {failure[0]!r}"]


def run_cases(arguments) -> list[str]:
    """The two-vCPU guest runs the probe's cases that suspend it or need no
    help from the host."""
    with guest_for(arguments, SUSPEND_VCPUS) as guest:
        guest.boot_to_shell(BOOT_SECONDS)
        request = len(guest.output())
        command = " ".join(["python3", "-B", PROBE, *(arguments.case or [])])
        result = guest.run_command(command, PROBE_SECONDS)
        tail = guest.output()[request:]
        show_cases(result.output)
        problems = [
            f"{case}: {reason}"
            for case, reason in CASE_FAILURE.findall(result.output)
        ]
        if result.return_code != 0 and not problems:
            problems.append(f"the probe exited with status {result.return_code}")
        problems += complaints_of(tail)
        if problems:
            show_driver_log(guest)
        expected = len(
            set(SUSPENDING_CASES) & set(arguments.case or SUSPENDING_CASES)
        )
        if not problems and tail.count(SUSPEND_EXIT) < expected:
            problems.append(
                f"the guest suspended {tail.count(SUSPEND_EXIT)} times, "
                f"expected {expected}"
            )
        if not problems:
            guest.check_command("echo alive", 20)
            if not guest.shutdown(60):
                problems.append("the guest did not power off")
        return problems


def result_of(guest: Guest, case: str, request: int) -> re.Match[str] | None:
    return re.search(
        rf"^VSND {case} (ok|FAIL: .*?)\r?$", guest.output()[request:], re.M
    )


def run_stall(arguments, private: pathlib.Path, case: str) -> list[str]:
    """A one-vCPU guest streams while the management console holds it."""
    with guest_for(arguments, STALL_VCPUS) as guest:
        guest.boot_to_shell(BOOT_SECONDS)
        console = ManagementConsole(guest, private)
        request = len(guest.output())
        # The probe's output goes to the console, where the runner reads it.
        guest.check_command(f"(python3 -B {PROBE} {case} </dev/null &)", 30)
        streaming = f"VSND {case} streaming"
        deadline = time.monotonic() + STREAM_SECONDS
        while streaming not in guest.output()[request:]:
            if result_of(guest, case, request) is not None:
                break
            if time.monotonic() > deadline:
                return [f"{case}: the probe never streamed"]
            drain(guest, 0.05)
        if result_of(guest, case, request) is None:
            console.request("stop")
            drain(guest, STALL_MILLISECONDS / 1000)
            console.request("go")
        deadline = time.monotonic() + RESULT_SECONDS
        while (result := result_of(guest, case, request)) is None:
            if time.monotonic() > deadline:
                return [f"{case}: the probe reported nothing in {RESULT_SECONDS} s"]
            drain(guest, 0.05)
        show_cases(guest.output()[request:])
        problems = []
        if result[1] != "ok":
            problems.append(f"{case}: {result[1].removeprefix('FAIL: ')}")
        problems += complaints_of(guest.output()[request:])
        if problems:
            show_driver_log(guest)
        if not problems and not guest.shutdown(60):
            problems.append(f"{case}: the guest did not power off")
        return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument(
        "--case", action="append",
        help="a probe case to run (default: all); the stall cases run in "
        "a guest of their own",
    )
    arguments = parser.parse_args()
    arguments.emulator = arguments.emulator.resolve()
    arguments.rootfs = arguments.rootfs.resolve()
    stalls = [case for case in (arguments.case or STALL_CASES) if case in STALL_CASES]
    if arguments.case is not None:
        arguments.case = [c for c in arguments.case if c not in STALL_CASES]

    # Short, so the management console's directory fits sun_path inside it.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-vsnd-"))
    os.environ["TMPDIR"] = str(private)
    tempfile.tempdir = str(private)
    problems: list[str] = []
    try:
        if arguments.case != []:
            problems += run_cases(arguments)
        for case in stalls:
            problems += run_stall(arguments, private, case)
    finally:
        shutil.rmtree(private, ignore_errors=True)
    if problems:
        raise RuntimeError("; ".join(problems))
    print("every stream restarted afresh and carried on")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
