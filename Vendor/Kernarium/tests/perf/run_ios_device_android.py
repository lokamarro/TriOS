#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot Android in Andarium on a connected iOS device and run an adb workload.

Android guests on a device run through Andarium; Kernarium keeps Linux
(``run_ios_device_workload.py``). The runner installs an automation build of
Andarium, launches the guest (the system the application carries) with
``ANDARIUM_START`` and a loopback forward,
reaches the guest's adbd over USB with ``iproxy`` and the shared adb server
(the guest is always named with ``-s``; the runner ends with ``adb disconnect``
and never stops a server),
waits for ``sys.boot_completed``, runs the workload (a guest shell script
and/or cold launches and scenarios from ``android_launch_suite.py``'s
formats, after any ``--push`` files such as a CTS APK reach the guest),
powers the guest off over adb and waits for the runtime to return.
The console, the thermal state at the start and the end, and a summary are
kept in the artifact directory. Nothing is typed on the device.

The runner never uninstalls the application, never restarts the automation
host and writes nothing into the container; it stops, recording no result,
when adb is not ready. Andarium/README.md#automation owns the application's
side, tests/perf/README.md the lane.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
sys.path.insert(0, str(REPOSITORY / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import android_launch_suite as launch_suite  # noqa: E402
import android_provision  # noqa: E402
import andarium  # noqa: E402
import ios_device  # noqa: E402

DEFAULT_ARTIFACT_ROOT = REPOSITORY / ".build" / "ios-device-android"
DEFAULT_APPS_MANIFEST = Path(__file__).resolve().parent / (
    "android_launch_apps.tsv"
)
SCRIPT_GUEST_PATH = "/data/local/tmp/andarium-harness-script.sh"
SCRIPT_EXIT = re.compile(r"ANDARIUM_SCRIPT_EXIT:([0-9]+)")
IPROXY_LISTEN_SECONDS = 10


@dataclass
class Plan:
    """Everything the options name, checked before the device is touched."""

    script: Path | None = None
    pushes: list[tuple[Path, str]] = field(default_factory=list)
    apps: list[launch_suite.App] = field(default_factory=list)
    rounds: int = 0
    scenarios: dict[str, list[launch_suite.Scenario]] = field(
        default_factory=dict
    )


def parse_arguments(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True,
                        help="the device's UDID (devicectl and iproxy "
                        "both take it)")
    parser.add_argument("--app", type=Path,
                        help="the automation .app bundle to install first")
    parser.add_argument("--start", choices=andarium.START_MODES,
                        default="normal", help="ANDARIUM_START")
    parser.add_argument("--erase", action="store_true",
                        help="start from a fresh data disk (ANDARIUM_ERASE); "
                        "the guest is then provisioned over adb after the boot, "
                        "so the workload does not land in Setup Wizard")
    parser.add_argument("--keep-setup-wizard", action="store_true",
                        help="with --erase, leave the guest in Setup Wizard "
                        "(to measure the first-boot experience itself)")
    parser.add_argument("--cpus", type=int, default=2)
    parser.add_argument("--memory-mib", type=int, default=1536)
    parser.add_argument("--engine", choices=("gadgets", "jit"),
                        default="gadgets")
    parser.add_argument("--disk-size-gib", type=int,
                        help="the guest storage capacity")
    parser.add_argument(
        "--setting", action="append", default=[], metavar="NAME=VALUE",
        help="another KERNARIUM_* or ANDARIUM_* launch setting",
    )
    parser.add_argument(
        "--host-port", type=int,
        help="the forward's port on the device and on this Mac; defaults to "
        f"{andarium.SLOT_PORT_VARIABLE}",
    )
    parser.add_argument("--adb", help="adb; defaults to the one on PATH")
    parser.add_argument("--iproxy", help="iproxy; defaults to PATH")
    parser.add_argument("--script", type=Path,
                        help="run this POSIX shell script in the guest")
    parser.add_argument(
        "--push", action="append", default=[], metavar="FILE:GUEST_PATH",
        help="adb push this file to the guest before the workload, for "
        "example a CTS APK the --script installs and instruments",
    )
    parser.add_argument(
        "--launch-app", action="append", default=[], metavar="NAME",
        help="cold-launch this application of the manifest",
    )
    parser.add_argument(
        "--cold-launches", type=int, metavar="N",
        help="rounds of cold launches of every --launch-app (default 1)",
    )
    parser.add_argument(
        "--scenario", action="append", default=[], type=Path, metavar="FILE",
        help="follow the first launch of the scenario's application with "
        "its taps and wait for its completion line",
    )
    parser.add_argument("--apps-manifest", type=Path,
                        default=DEFAULT_APPS_MANIFEST)
    parser.add_argument("--settle", type=float, default=0,
                        help="seconds between boot completion and workload")
    parser.add_argument("--launch-settle", type=float, default=3,
                        help="seconds between a force-stop and its launch")
    parser.add_argument("--poweroff-command", default="reboot -p",
                        help="the guest shell command that powers it off")
    parser.add_argument("--timeout", type=int, default=3600)
    parser.add_argument("--boot-timeout", type=int, default=900)
    parser.add_argument("--launch-timeout", type=int, default=120)
    parser.add_argument(
        "--wait-active", type=int, default=0, metavar="SECONDS",
        help="after the boot, wait this long for the application to be "
        "active again if a system alert took its focus (iOS asking for local "
        "network access waits for a person); the run fails when it is not",
    )
    parser.add_argument("--script-timeout", type=int, default=1800)
    parser.add_argument("--push-timeout", type=int, default=600)
    parser.add_argument("--poweroff-timeout", type=int, default=150)
    parser.add_argument("--install-timeout", type=int, default=600)
    parser.add_argument("--poll-interval", type=float, default=2.0)
    parser.add_argument("--artifact-root", type=Path,
                        default=DEFAULT_ARTIFACT_ROOT)
    options = parser.parse_args(argv)
    if min(options.timeout, options.boot_timeout, options.launch_timeout,
           options.script_timeout, options.push_timeout,
           options.poweroff_timeout, options.install_timeout) < 1 or options.poll_interval <= 0:
        parser.error("timeouts and the poll interval must be positive")
    if min(options.settle, options.launch_settle) < 0:
        parser.error("settle times cannot be negative")
    return options


def parse_settings(values: list[str]) -> dict[str, str]:
    settings: dict[str, str] = {}
    for value in values:
        name, separator, setting = value.partition("=")
        if not separator or name in settings:
            raise andarium.AndariumError(
                f"expected one NAME=VALUE per setting, not {value!r}"
            )
        settings[name] = setting
    return settings


def prepare(options: argparse.Namespace) -> Plan:
    """Check every input the run will read, so a mistake costs no boot."""
    plan = Plan()
    if options.script is not None:
        plan.script = options.script.resolve()
        if not plan.script.is_file():
            raise andarium.AndariumError(f"no script: {plan.script}")
    for value in options.push:
        local, separator, guest = value.rpartition(":")
        if not separator or not local or not guest.startswith("/"):
            raise andarium.AndariumError(
                f"expected FILE:GUEST_PATH with an absolute guest path, "
                f"not {value!r}"
            )
        source = Path(local).resolve()
        if not source.is_file():
            raise andarium.AndariumError(f"no file to push: {source}")
        plan.pushes.append((source, guest))
    manifest = launch_suite.read_manifest(options.apps_manifest)
    for name in options.launch_app:
        if name not in manifest:
            raise andarium.AndariumError(
                f"no application {name!r} in {options.apps_manifest}; it "
                "has " + ", ".join(sorted(manifest))
            )
        plan.apps.append(manifest[name])
    plan.rounds = (
        options.cold_launches if options.cold_launches is not None
        else int(bool(plan.apps))
    )
    if (plan.rounds > 0) != bool(plan.apps) or plan.rounds < 0:
        raise andarium.AndariumError(
            "--launch-app and a positive --cold-launches go together"
        )
    for path in options.scenario:
        try:
            scenario = launch_suite.read_scenario(path)
        except KeyError as error:
            raise andarium.AndariumError(
                f"{path} lacks {error}") from None
        if scenario.app not in {app.name for app in plan.apps}:
            raise andarium.AndariumError(
                f"{path} follows {scenario.app!r}, which is not a "
                "--launch-app"
            )
        plan.scenarios.setdefault(scenario.app, []).append(scenario)
    return plan


def find_tool(name: str, override: str | None) -> str:
    path = override or shutil.which(name)
    if path is None or not os.access(path, os.X_OK):
        raise andarium.AndariumError(
            f"{name} is unavailable; install it or pass --{name}"
        )
    return path


def port_is_open(port: int) -> bool:
    with socket.socket() as probe:
        probe.settimeout(1)
        return probe.connect_ex(("127.0.0.1", port)) == 0


def text_of(value: bytes | str | None) -> str:
    if isinstance(value, bytes):
        return value.decode(errors="replace")
    return value or ""


class Iproxy:
    """The USB tunnel from a Mac port to the guest's forward on the device."""

    def __init__(self, program: str, device: str, port: int,
                 log: Path) -> None:
        self.command = [program, "-u", device, f"{port}:{port}"]
        self.port = port
        self.log = log
        self.process: subprocess.Popen[bytes] | None = None

    def __enter__(self) -> "Iproxy":
        if port_is_open(self.port):
            raise andarium.AndariumError(
                f"port {self.port} on this Mac is already in use"
            )
        with self.log.open("wb") as output:
            self.process = subprocess.Popen(
                self.command, stdin=subprocess.DEVNULL, stdout=output,
                stderr=subprocess.STDOUT,
            )
        deadline = time.monotonic() + IPROXY_LISTEN_SECONDS
        while not port_is_open(self.port):
            if self.process.poll() is not None:
                raise andarium.AndariumError(
                    f"iproxy ended with status {self.process.returncode}: "
                    + self.log.read_text(errors="replace").strip()
                )
            if time.monotonic() >= deadline:
                raise andarium.AndariumError(
                    f"iproxy did not listen on port {self.port}"
                )
            time.sleep(0.1)
        return self

    def __exit__(self, *_exception: object) -> None:
        process = self.process
        if process is None or process.poll() is not None:
            return
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)


class Adb:
    """One guest, reached through the shared adb server and named by serial."""

    def __init__(self, program: str, port: int) -> None:
        self.program = program
        self.serial = f"127.0.0.1:{port}"

    def run(self, *arguments: str, timeout: int = 60,
            device: bool = True) -> subprocess.CompletedProcess[str]:
        command = [self.program]
        if device:
            command += ["-s", self.serial]
        command += arguments
        try:
            return subprocess.run(
                command, capture_output=True, text=True, errors="replace",
                stdin=subprocess.DEVNULL, timeout=timeout,
            )
        except subprocess.TimeoutExpired as error:
            return subprocess.CompletedProcess(
                command, 124, text_of(error.stdout), text_of(error.stderr)
            )

    def shell(self, command: str,
              timeout: int = 60) -> subprocess.CompletedProcess[str]:
        return self.run("shell", command, timeout=timeout)

    def connect(self) -> None:
        self.run("connect", self.serial, timeout=20, device=False)

    def state(self) -> str:
        result = self.run("get-state", timeout=20)
        return (result.stdout or result.stderr).strip() or "unreachable"

    def disconnect(self) -> None:
        """Drop this guest's transport; the shared server stays up."""
        self.run("disconnect", self.serial, timeout=20, device=False)


def last_line(transcript: str) -> str:
    lines = [line for line in transcript.splitlines() if line.strip()]
    return lines[-1][:200] if lines else "(empty)"


def guard_console(console: ios_device.Console) -> None:
    """Stop a run whose guest or devicectl ended before it asked them to."""
    running = console.running()
    transcript = console.transcript()
    result = andarium.runtime_result(transcript)
    if result is not None:
        raise andarium.AndariumError(
            f"the guest ended on its own (runtime returned {result})"
        )
    if not running:
        raise andarium.AndariumError(
            "devicectl ended before the guest did; its last line: "
            + last_line(transcript)
        )


def wait_for_adb_authorization_off(
    console: ios_device.Console, timeout: int, interval: float
) -> None:
    deadline = time.monotonic() + timeout
    while andarium.ADB_AUTH_OFF_LINE not in console.transcript():
        guard_console(console)
        if time.monotonic() >= deadline:
            andarium.require_adb_authorization_off(console.transcript())
        time.sleep(interval)


def wait_for_active(console: ios_device.Console, timeout: int,
                    interval: float) -> float:
    """Wait for the application to hold the focus; the seconds it took."""
    started = time.monotonic()
    while not andarium.application_is_active(console.transcript()):
        guard_console(console)
        if time.monotonic() - started >= timeout:
            raise andarium.AndariumError(
                f"the application was still inactive after {timeout} seconds; "
                "a system alert is waiting for a person to answer it"
            )
        time.sleep(interval)
    return time.monotonic() - started


def wait_for_boot(adb: Adb, console: ios_device.Console, *, timeout: int,
                  deadline: float, interval: float) -> None:
    """Return once adb is a ``device`` and ``sys.boot_completed`` is 1."""
    limit = min(time.monotonic() + timeout, deadline)
    state, value = "not polled", ""
    while True:
        guard_console(console)
        adb.connect()
        state = adb.state()
        if state == "device":
            result = adb.shell("getprop sys.boot_completed", timeout=30)
            value = result.stdout.strip()
            if result.returncode == 0 and value == "1":
                return
        if time.monotonic() >= limit:
            if state != "device":
                raise andarium.AndariumError(
                    f"adb never became ready within {timeout} seconds "
                    f"(last state: {state})"
                )
            raise andarium.AndariumError(
                f"sys.boot_completed was {value!r} after {timeout} seconds"
            )
        time.sleep(interval)


def provision_guest(adb: Adb) -> bool:
    """Provision the erased guest the way Setup Wizard's last step leaves it.

    Without it HOME returns to the wizard, which then runs during the launch
    and memory measurements (tools/android_provision.py); whether the helper
    changed anything is recorded in the summary.
    """
    try:
        return android_provision.provision(
            lambda command: adb.shell(command, timeout=60).stdout
        )
    except android_provision.ProvisionError as error:
        raise andarium.AndariumError(str(error)) from error


def require_adb_ready(adb: Adb) -> None:
    """Stop before a workload whose adb is gone: it would record nothing."""
    state = adb.state()
    echoed = adb.shell("echo ready", timeout=30)
    if state != "device" or "ready" not in echoed.stdout:
        raise andarium.AndariumError(
            f"adb is not ready before the workload (state: {state})"
        )


def push_file(adb: Adb, source: Path, guest: str, timeout: int) -> dict:
    started = time.monotonic()
    pushed = adb.run("push", str(source), guest, timeout=timeout)
    if pushed.returncode != 0:
        raise andarium.AndariumError(
            f"adb could not push {source.name} to {guest}: "
            + (pushed.stderr or pushed.stdout).strip()
        )
    return {
        "file": source.name,
        "guest": guest,
        "bytes": source.stat().st_size,
        "seconds": round(time.monotonic() - started, 1),
    }


def run_script(adb: Adb, source: Path, timeout: int, output: Path) -> dict:
    pushed = adb.run("push", str(source), SCRIPT_GUEST_PATH, timeout=120)
    if pushed.returncode != 0:
        raise andarium.AndariumError(
            "adb could not push the script: "
            + (pushed.stderr or pushed.stdout).strip()
        )
    started = time.monotonic()
    result = adb.shell(
        f"sh {SCRIPT_GUEST_PATH}; echo ANDARIUM_SCRIPT_EXIT:$?",
        timeout=timeout,
    )
    seconds = time.monotonic() - started
    output.write_text(result.stdout + result.stderr, encoding="utf-8")
    found = SCRIPT_EXIT.findall(result.stdout)
    adb.shell(f"rm -f {SCRIPT_GUEST_PATH}", timeout=30)
    return {
        "exit_status": int(found[-1]) if found else None,
        "seconds": round(seconds, 1),
        "output": output.name,
    }


def cold_launch(adb: Adb, app: launch_suite.App, number: int,
                options: argparse.Namespace) -> dict:
    """Force-stop the application, launch it, and time it two ways."""
    adb.shell(f"am force-stop {app.package}", timeout=30)
    time.sleep(options.launch_settle)
    adb.run("logcat", "-c", timeout=30)
    started = time.monotonic()
    start = adb.shell(f"am start -W -n {app.component}", timeout=app.limit)
    total = launch_suite.TOTAL_TIME.search(start.stdout)
    seconds = None
    while True:
        logcat = adb.run("logcat", "-d", "-v", "epoch", timeout=60).stdout
        seconds = launch_suite.launch_seconds(logcat, app)
        if seconds is not None or time.monotonic() - started >= app.limit:
            break
        time.sleep(options.poll_interval)
    return {
        "app": app.name,
        "round": number,
        "total_time_ms": int(total.group(1)) if total else None,
        "seconds": seconds,
    }


def run_scenario(adb: Adb, scenario: launch_suite.Scenario,
                 options: argparse.Namespace) -> dict:
    for step in scenario.steps:
        if "sleep" in step:
            time.sleep(float(step["sleep"]))
        else:
            x, y = step["tap"]
            adb.shell(f"input tap {x} {y}", timeout=30)
    # The guest's own clock marks the end of the steps, as its logcat does.
    started = float(adb.shell("date +%s.%N", timeout=30).stdout.strip() or 0)
    deadline = time.monotonic() + scenario.limit
    while True:
        logcat = adb.run("logcat", "-d", "-v", "epoch", timeout=60).stdout
        found = launch_suite.scenario_values(logcat, scenario, started)
        if found is not None:
            return {
                "app": scenario.app,
                "seconds": round(found["done_at"] - started, 3),
                "values": found["values"],
            }
        if time.monotonic() >= deadline:
            return {"app": scenario.app, "seconds": None, "values": None}
        time.sleep(options.poll_interval)


def run_workload(adb: Adb, plan: Plan, options: argparse.Namespace,
                 artifact: Path, deadline: float,
                 problems: list[str]) -> dict:
    require_adb_ready(adb)
    record: dict = {"pushes": [], "script": None, "launches": [],
                    "scenarios": []}
    for source, guest in plan.pushes:
        record["pushes"].append(
            push_file(adb, source, guest, options.push_timeout))
    if plan.script is not None:
        record["script"] = run_script(
            adb, plan.script, options.script_timeout,
            artifact / "script.out",
        )
        if record["script"]["exit_status"] != 0:
            problems.append(
                "the script exited with "
                f"{record['script']['exit_status']}; see script.out"
            )
    for number in range(1, plan.rounds + 1):
        for app in plan.apps:
            if time.monotonic() >= deadline:
                raise andarium.AndariumError(
                    f"--timeout passed before launch {number} of {app.name}"
                )
            launch = cold_launch(adb, app, number, options)
            record["launches"].append(launch)
            if launch["total_time_ms"] is None or launch["seconds"] is None:
                problems.append(
                    f"launch {number} of {app.name} gave no complete time"
                )
            elif number == 1:
                for scenario in plan.scenarios.get(app.name, []):
                    followed = run_scenario(adb, scenario, options)
                    record["scenarios"].append(followed)
                    if followed["seconds"] is None:
                        problems.append(
                            f"the scenario of {app.name} did not finish"
                        )
            adb.shell("input keyevent KEYCODE_HOME", timeout=30)
    return record


def wait_for_runtime_return(console: ios_device.Console, timeout: int,
                            interval: float) -> int:
    deadline = time.monotonic() + timeout
    while True:
        running = console.running()
        result = andarium.runtime_result(console.transcript())
        if result is not None:
            return result
        if not running:
            raise andarium.AndariumError(
                "devicectl ended without the runtime returning"
            )
        if time.monotonic() >= deadline:
            raise andarium.AndariumError(
                f"the console did not report the runtime returning within "
                f"{timeout} seconds of power off"
            )
        time.sleep(interval)


@dataclass
class Run:
    """One run's inputs, tools and record."""

    options: argparse.Namespace
    plan: Plan
    adb_program: str
    iproxy_program: str
    host_port: int
    settings: dict[str, str]
    artifact: Path
    summary: dict
    problems: list[str] = field(default_factory=list)

    def check_unlocked(self) -> None:
        if ios_device.device_lock_state(self.options.device) == "locked":
            raise andarium.AndariumError(
                "the device is passcode-locked; only its owner can unlock it"
            )

    def execute(self) -> None:
        """Install, boot, drive, power off; fills the summary."""
        options, plan = self.options, self.plan
        self.check_unlocked()
        if options.app is not None:
            app = options.app.resolve()
            andarium.require_automation_app(app)
            ios_device.install_app(
                options.device, app, options.install_timeout
            )
        self.check_unlocked()
        adb = Adb(self.adb_program, self.host_port)
        log = self.artifact / "console.log"
        self.summary["console"] = log.name
        started = time.monotonic()
        try:
            with ios_device.Console(
                options.device, andarium.APPLICATION, self.settings, log
            ) as console:
                try:
                    with Iproxy(self.iproxy_program, options.device,
                                self.host_port,
                                self.artifact / "iproxy.log"):
                        self.drive(adb, console, started)
                except (ios_device.DeviceError, subprocess.SubprocessError,
                        OSError) as error:
                    self.problems.append(str(error))
        finally:
            self.summary["seconds"] = round(time.monotonic() - started, 1)
            adb.disconnect()
            transcript = (
                log.read_text(errors="replace") if log.exists() else ""
            )
            self.summary["thermal"] = andarium.thermal_record(transcript)
            if "boot_seconds" in self.summary and not (
                self.summary["thermal"]["complete"]
            ):
                print("warning: the console lacks the thermal start or "
                      "finish line", file=sys.stderr, flush=True)

    def drive(self, adb: Adb, console: ios_device.Console,
              started: float) -> None:
        """Wait for the boot, run the workload and power the guest off."""
        options = self.options
        ready = False
        try:
            wait_for_adb_authorization_off(
                console, options.launch_timeout, options.poll_interval
            )
            wait_for_boot(
                adb, console, timeout=options.boot_timeout,
                deadline=started + options.timeout,
                interval=options.poll_interval,
            )
            # From the launch, so it includes the application's own start.
            self.summary["boot_seconds"] = round(
                time.monotonic() - started, 1)
            ready = True
            self.summary["waited_for_active_seconds"] = round(
                wait_for_active(console, options.wait_active,
                                options.poll_interval), 1)
            if options.erase and not options.keep_setup_wizard:
                self.summary["provisioned"] = provision_guest(adb)
            time.sleep(options.settle)
            self.summary["workload"] = run_workload(
                adb, self.plan, options, self.artifact,
                started + options.timeout, self.problems,
            )
        except (ios_device.DeviceError, subprocess.SubprocessError,
                OSError) as error:
            self.problems.append(str(error))
        if not ready:
            return
        adb.shell(options.poweroff_command, timeout=30)
        try:
            result = wait_for_runtime_return(
                console, options.poweroff_timeout, options.poll_interval
            )
        except andarium.AndariumError as error:
            self.problems.append(str(error))
            return
        self.summary["runtime_result"] = result
        if result != 0:
            self.problems.append(f"the runtime returned {result}")

    def write_summary(self) -> None:
        self.summary["problems"] = self.problems
        self.summary["status"] = "failed" if self.problems else "ok"
        (self.artifact / "summary.json").write_text(
            json.dumps(self.summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

def main(argv: list[str] | None = None) -> int:
    options = parse_arguments(argv)
    summary: dict = {
        "schema_version": 1,
        "device": options.device,
        "guest": {
            "start": options.start,
            "erase": options.erase,
            "cpus": options.cpus,
            "memory_mib": options.memory_mib,
            "engine": options.engine,
            "page_size": 4096,
        },
        "runtime_result": None,
        "workload": None,
    }
    artifact = options.artifact_root.resolve() / time.strftime(
        "%Y%m%d-%H%M%S"
    )
    try:
        plan = prepare(options)
        adb_program = find_tool("adb", options.adb)
        iproxy_program = find_tool("iproxy", options.iproxy)
        host_port = andarium.validate_port(
            options.host_port if options.host_port is not None
            else andarium.host_port_from_slot(os.environ)
        )
        settings = andarium.settings_for(
            start=options.start,
            erase=options.erase,
            cpus=options.cpus,
            memory_mib=options.memory_mib,
            engine=options.engine,
            host_port=host_port,
            disk_size_gib=options.disk_size_gib,
            extra=parse_settings(options.setting),
        )
        artifact.mkdir(parents=True, exist_ok=False)
    except (andarium.AndariumError, OSError, ValueError, KeyError) as error:
        print(f"run_ios_device_android.py: {error}", file=sys.stderr)
        return 1
    summary["host_port"] = host_port
    run = Run(options, plan, adb_program, iproxy_program,
              host_port, settings, artifact, summary)
    try:
        run.execute()
    except (ios_device.DeviceError, subprocess.SubprocessError,
            OSError) as error:
        run.problems.append(str(error))
    except BaseException as interruption:
        run.problems.append(f"interrupted by {type(interruption).__name__}")
        raise
    finally:
        run.write_summary()
    for problem in run.problems:
        print(f"run_ios_device_android.py: {problem}", file=sys.stderr)
    print(f"Android run {summary['status']}, runtime returned "
          f"{summary['runtime_result']}; console: {artifact / 'console.log'}",
          flush=True)
    return 1 if run.problems else 0


def stop_on_sigterm(_number: int, _frame: object) -> None:
    """Let a SIGTERM unwind the run, so the console stops and the summary
    is written."""
    raise SystemExit(143)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, stop_on_sigterm)
    raise SystemExit(main())
