#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Time cold application launches in one Android guest boot.

The guest boots from a copy-on-write clone of a source instance's data disk,
so every run starts from the same installed and optimized state and the source
is never written. Each round launches every selected application from cold (force
stopped, after an idle interval) and times it from the activity manager's
START line to the first logcat line matching the application's completion
pattern, both by the guest's own logcat timestamps. The patterns live in android_launch_apps.tsv; pick one
that marks the end of the launch's CPU work (a game's first scene, not its
splash screen). Records, one JSON object per launch, are appended to
--output; `--summary FILE...` prints each label's per-application medians.

A --scenario file continues one application's launch into a phase the
application reports itself: after the launch it replays taps and waits, then
waits for a completion line and records named values captured from it (for a
benchmark, its own loading time and score), plus the emulator CPU time spent.
Taps are in guest display pixels, so a scenario belongs to one display size.

The guest is provisioned after its boot (boot_android.py --provision), so a
source instance prepared on an erased disk, which the product starts in Setup
Wizard, is measured on the launcher; a provisioned source is left as it is, and
--keep-setup-wizard measures a source in the wizard.

The tool does not serialize guests: run one guest per measurement.
Each record carries the guest's vCPU count (--cpus, default 4) and execution
engine (--engine, default gadgets). Environment
for one arm of an A/B goes in with --env, and --guest-samples profiles the
whole boot with DARWIN_UML_GUEST_SAMPLES.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import contextlib
import dataclasses
import json
import os
import pathlib
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time

REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
BOOT = REPOSITORY / "tools" / "boot_android.py"
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import slot_ports  # noqa: E402

DEFAULT_ADB_PORT = 5601
MANIFEST = pathlib.Path(__file__).resolve().parent / "android_launch_apps.tsv"
# `logcat -v epoch`: seconds.fraction, then the rest of the line.
EPOCH_LINE = re.compile(r"^\s*(\d+\.\d+)\s+(.*)$")
TOTAL_TIME = re.compile(r"^TotalTime:\s*(\d+)", re.MULTILINE)
# `ps -M`: a thread's scheduling priority precedes its two CPU time columns.
THREAD_PRIORITY = re.compile(r"\s(\d+)[A-Z]*\s+[\d:.]+\s+[\d:.]+\s*")
# Seconds power_off waits for the guest to power itself off, for the launcher
# to stop its emulator after SIGTERM, and for SIGKILLed processes to go.
POWER_OFF_SECONDS = 300
LAUNCHER_STOP_SECONDS = 60
KILL_SECONDS = 10


@dataclasses.dataclass(frozen=True)
class App:
    name: str
    component: str
    done: re.Pattern[str]
    limit: int

    @property
    def package(self) -> str:
        return self.component.split("/", 1)[0]

    @property
    def launched(self) -> re.Pattern[str]:
        return re.compile(r"START u0 .*cmp=" + re.escape(self.package) + "/")


def read_manifest(path: pathlib.Path) -> dict[str, App]:
    lines = path.read_text().splitlines()
    if not lines or lines[0].split("\t") != [
            "name", "component", "done", "limit_seconds"]:
        raise ValueError(f"{path}: unexpected header")
    apps = {}
    for number, line in enumerate(lines[1:], 2):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 4 or "/" not in fields[1]:
            raise ValueError(f"{path}:{number}: malformed row")
        apps[fields[0]] = App(fields[0], fields[1], re.compile(fields[2]),
                              int(fields[3]))
    return apps


def first_match(logcat: str, pattern: re.Pattern[str],
                since: float) -> float | None:
    """Timestamp of the first line at or after `since` matching `pattern`."""
    for line in logcat.splitlines():
        match = EPOCH_LINE.match(line)
        if match and float(match.group(1)) >= since and \
                pattern.search(match.group(2)):
            return float(match.group(1))
    return None


def launch_seconds(logcat: str, app: App) -> float | None:
    """Seconds from the activity manager's START of `app` to its marker.

    Both ends are the guest's own logcat timestamps, so the host and guest
    clocks never mix.
    """
    started = first_match(logcat, app.launched, 0.0)
    if started is None:
        return None
    finished = first_match(logcat, app.done, started)
    return None if finished is None else round(finished - started, 3)


def cpu_seconds(text: str) -> float:
    """`ps -o time=` as seconds: [[dd-]hh:]mm:ss.ss."""
    days, _, clock = text.strip().rpartition("-")
    seconds = 0.0
    for part in clock.split(":"):
        seconds = seconds * 60 + float(part)
    return seconds + (int(days) * 86400 if days else 0)


def thread_priorities(ps_threads: str) -> dict[str, int]:
    """Threads per scheduling priority in `ps -M` output."""
    counts: dict[str, int] = {}
    for line in ps_threads.splitlines()[1:]:
        match = THREAD_PRIORITY.search(line)
        if match:
            counts[match.group(1)] = counts.get(match.group(1), 0) + 1
    return counts


def summarize(records: list[dict]) -> dict[str, dict[str, dict[str, float]]]:
    """Per label and application: completed launches and median seconds."""
    grouped: dict[str, dict[str, list[dict]]] = {}
    for record in records:
        grouped.setdefault(record["label"], {}).setdefault(
            record["app"], []).append(record)
    summary = {}
    for label, apps in grouped.items():
        summary[label] = {}
        for app, runs in apps.items():
            done = [run for run in runs if run["seconds"] is not None]
            row = {"launches": len(runs), "completed": len(done)}
            if done:
                row["median_seconds"] = statistics.median(
                    run["seconds"] for run in done)
                row["median_emulator_cpu"] = statistics.median(
                    run["emulator_cpu"] for run in done)
            summary[label][app] = row
    return summary


@dataclasses.dataclass(frozen=True)
class Scenario:
    app: str
    steps: tuple[dict, ...]
    done: re.Pattern[str]
    limit: int
    values: dict[str, re.Pattern[str]]


def read_scenario(path: pathlib.Path) -> Scenario:
    data = json.loads(path.read_text())
    steps = tuple(data["steps"])
    for step in steps:
        if set(step) not in ({"sleep"}, {"tap"}) or (
                "tap" in step and len(step["tap"]) != 2):
            raise ValueError(f"{path}: a step is one sleep or one tap: {step}")
    return Scenario(data["app"], steps, re.compile(data["done"]),
                    int(data["limit_seconds"]),
                    {name: re.compile(pattern)
                     for name, pattern in data.get("values", {}).items()})


def scenario_values(logcat: str, scenario: Scenario,
                    since: float) -> dict | None:
    """The completion line's time and captured values, once it appears."""
    for line in logcat.splitlines():
        match = EPOCH_LINE.match(line)
        if not match or float(match.group(1)) < since or \
                not scenario.done.search(match.group(2)):
            continue
        values = {}
        for name, pattern in scenario.values.items():
            found = pattern.search(match.group(2))
            values[name] = float(found.group(1)) if found else None
        return {"done_at": float(match.group(1)), "values": values}
    return None


def process_exists(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


class Guest:
    def __init__(self, arguments: argparse.Namespace, environment: dict):
        self.arguments = arguments
        self.serial = f"127.0.0.1:{arguments.adb_port}"
        self.console = open(arguments.console, "ab")
        command = [sys.executable, str(BOOT),
                   "--display", arguments.display,
                   "--vnc-port", str(arguments.vnc_port),
                   "--emulator", str(arguments.emulator),
                   "--release", str(arguments.release),
                   "--instance", str(arguments.work_instance),
                   "--adb-port", str(arguments.adb_port),
                   "--adb-timeout", "900",
                   "--cpus", str(arguments.cpus),
                   "--memory", arguments.memory,
                   "--engine", arguments.engine]
        if not arguments.keep_setup_wizard:
            command.append("--provision")
        self.process = subprocess.Popen(
            command, cwd=REPOSITORY, env=environment, stdin=subprocess.DEVNULL,
            stdout=self.console, stderr=subprocess.STDOUT)

    def adb(self, *command: str, timeout: int = 60) -> str:
        result = subprocess.run(
            ["adb", "-s", self.serial, *command], stdin=subprocess.DEVNULL,
            capture_output=True, text=True, timeout=timeout)
        return result.stdout.replace("\r", "")

    def shell(self, command: str, timeout: int = 60) -> str:
        return self.adb("shell", command, timeout=timeout)

    def emulator_pid(self) -> int | None:
        """The launcher starts the emulator as its only child."""
        result = subprocess.run(
            ["pgrep", "-P", str(self.process.pid)], capture_output=True,
            text=True, stdin=subprocess.DEVNULL)
        pids = result.stdout.split()
        return int(pids[0]) if len(pids) == 1 else None

    def wait_booted(self, deadline: float) -> bool:
        while time.monotonic() < deadline and self.process.poll() is None:
            try:
                if self.shell("getprop sys.boot_completed",
                              timeout=20).strip() == "1":
                    return True
            except subprocess.TimeoutExpired:
                pass
            time.sleep(10)
        return False

    def power_off(self) -> int:
        """Stop the guest and return the launcher's exit status.

        The launcher stops its emulator when it receives SIGTERM; one that
        does not is killed together with its children.
        """
        try:
            try:
                self.shell("reboot -p", timeout=30)
            except subprocess.TimeoutExpired:
                pass
            try:
                return self.process.wait(timeout=POWER_OFF_SECONDS)
            except subprocess.TimeoutExpired:
                pass
            self.process.terminate()
            try:
                return self.process.wait(timeout=LAUNCHER_STOP_SECONDS)
            except subprocess.TimeoutExpired:
                print(f"launcher {self.process.pid} ignored SIGTERM; "
                      "killing it and its emulator", file=sys.stderr)
                return self.kill()
        finally:
            self.console.close()

    def kill(self) -> int:
        """SIGKILL the launcher and its children; raise unless all are gone.

        The emulator leads its own session, so killing the launcher alone
        would orphan it. Stopping the launcher first keeps it from reaping
        or starting a child, so every PID found stays its child until the
        launcher dies; after that, launchd reaps them.
        """
        launcher = self.process.pid
        self.process.send_signal(signal.SIGSTOP)
        if self.process.poll() is not None:
            return self.process.returncode
        result = subprocess.run(
            ["pgrep", "-P", str(launcher)], capture_output=True, text=True,
            stdin=subprocess.DEVNULL, timeout=30)
        children = [int(pid) for pid in result.stdout.split()]
        for pid in children:
            try:
                os.killpg(pid, signal.SIGKILL)
            except ProcessLookupError:
                # A child that leads no group shares the launcher's.
                with contextlib.suppress(ProcessLookupError):
                    os.kill(pid, signal.SIGKILL)
            except PermissionError:
                # Darwin refuses a group whose leader is an unreaped zombie.
                pass
        self.process.kill()
        try:
            status = self.process.wait(timeout=KILL_SECONDS)
        except subprocess.TimeoutExpired:
            raise RuntimeError(
                f"Android launcher {launcher} survived SIGKILL") from None
        deadline = time.monotonic() + KILL_SECONDS
        while children := [pid for pid in children if process_exists(pid)]:
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    f"children {children} of Android launcher {launcher} "
                    "survived SIGKILL")
            time.sleep(0.1)
        return status


def run_scenario(guest: Guest, scenario: Scenario, logcat_path: pathlib.Path,
                 emulator_pid: int) -> dict:
    cpu0 = cpu_seconds(subprocess.run(
        ["ps", "-o", "time=", "-p", str(emulator_pid)],
        capture_output=True, text=True).stdout)
    for step in scenario.steps:
        if "sleep" in step:
            time.sleep(float(step["sleep"]))
        else:
            guest.shell(f"input tap {step['tap'][0]} {step['tap'][1]}")
    # The guest's own clock marks the end of the steps, as logcat does.
    started = float(guest.shell("date +%s.%N").strip() or 0)
    deadline = time.monotonic() + scenario.limit
    result = None
    while result is None and time.monotonic() < deadline:
        time.sleep(2)
        result = scenario_values(logcat_path.read_text(errors="replace"),
                                 scenario, started)
    cpu1 = cpu_seconds(subprocess.run(
        ["ps", "-o", "time=", "-p", str(emulator_pid)],
        capture_output=True, text=True).stdout)
    record = {"scenario_seconds": None, "scenario_values": None,
              "scenario_emulator_cpu": round(cpu1 - cpu0, 2)}
    if result is not None:
        record["scenario_seconds"] = round(result["done_at"] - started, 3)
        record["scenario_values"] = result["values"]
    return record


def launch(guest: Guest, app: App, emulator_pid: int,
           scenario: Scenario | None = None) -> dict:
    guest.shell(f"am force-stop {app.package}")
    time.sleep(3)
    guest.adb("logcat", "-c")
    logcat_path = guest.arguments.output.with_suffix(".logcat")
    with open(logcat_path, "w") as logcat_file:
        reader = subprocess.Popen(
            ["adb", "-s", guest.serial, "logcat", "-v", "epoch"],
            stdin=subprocess.DEVNULL, stdout=logcat_file,
            stderr=subprocess.DEVNULL)
        try:
            cpu0 = cpu_seconds(subprocess.run(
                ["ps", "-o", "time=", "-p", str(emulator_pid)],
                capture_output=True, text=True).stdout)
            deadline = time.monotonic() + app.limit
            try:
                start = guest.shell(f"am start -W -n {app.component}",
                                    timeout=app.limit)
            except subprocess.TimeoutExpired:
                start = ""
            seconds = None
            while seconds is None and time.monotonic() < deadline:
                time.sleep(1)
                seconds = launch_seconds(
                    logcat_path.read_text(errors="replace"), app)
            cpu1 = cpu_seconds(subprocess.run(
                ["ps", "-o", "time=", "-p", str(emulator_pid)],
                capture_output=True, text=True).stdout)
            followed = {} if scenario is None or seconds is None else \
                run_scenario(guest, scenario, logcat_path, emulator_pid)
        finally:
            reader.terminate()
            reader.wait(timeout=30)
    total = TOTAL_TIME.search(start)
    # A napped emulator runs its threads at priority 4 instead of 31, which
    # alone makes launches several times slower.
    priorities = thread_priorities(subprocess.run(
        ["ps", "-M", "-p", str(emulator_pid)], capture_output=True,
        text=True).stdout)
    time.sleep(10)
    guest.shell(f"am force-stop {app.package}")
    return {
        "seconds": seconds,
        "displayed_ms": int(total.group(1)) if total else None,
        "emulator_cpu": round(cpu1 - cpu0, 2),
        "thread_priorities": priorities,
        "load": os.getloadavg()[0],
        **followed,
    }


def run(arguments: argparse.Namespace) -> int:
    apps = read_manifest(MANIFEST)
    selected = [apps[name] for name in arguments.apps.split(",")]
    scenario = read_scenario(arguments.scenario) if arguments.scenario else None
    environment = dict(os.environ)
    for item in arguments.env:
        key, _, value = item.partition("=")
        environment[key] = value
    if arguments.guest_samples:
        environment["DARWIN_UML_GUEST_SAMPLES"] = arguments.guest_samples
    # The launcher updates an instance whose recorded release differs, which
    # would silently measure an updated install.
    identity = json.loads((arguments.release / "release-manifest.json")
                          .read_text())["release_id"]
    recorded = json.loads(
        (arguments.source_instance / "instance.json").read_text())
    if recorded["release_id"] != identity or recorded["pending_update"]:
        print(f"{arguments.source_instance} does not name release {identity} "
              "as a settled start", file=sys.stderr)
        return 1
    # A clone shares the source's blocks until the guest writes, so each run
    # starts from the source's state without copying it.
    work = arguments.work_instance
    work.mkdir(parents=True, exist_ok=True)
    for stale in [*work.glob("data.img*"), work / "instance.json"]:
        stale.unlink(missing_ok=True)
    staging = work / "data.img.new"
    subprocess.run(["cp", "-c", str(arguments.source_instance / "data.img"),
                    str(staging)], check=True, stdin=subprocess.DEVNULL)
    # The source may be read-only to stay unchanged; the guest writes its clone.
    staging.chmod(staging.stat().st_mode | 0o200)
    staging.replace(work / "data.img")
    shutil.copyfile(arguments.source_instance / "instance.json",
                    work / "instance.json")

    guest = Guest(arguments, environment)
    boot_started = time.monotonic()
    booted = guest.wait_booted(boot_started + 1200)
    boot_seconds = round(time.monotonic() - boot_started, 1)
    emulator_pid = guest.emulator_pid()
    status = None
    try:
        if not booted or emulator_pid is None:
            print(f"guest did not boot (pid {emulator_pid})", file=sys.stderr)
            return 1
        # The emulator's CPU seconds at boot completion measure the boot's
        # translation and execution work whatever the host load.
        boot_cpu = cpu_seconds(subprocess.run(
            ["ps", "-o", "time=", "-p", str(emulator_pid)],
            capture_output=True, text=True).stdout)
        time.sleep(arguments.settle)
        installed = guest.shell("pm list packages")
        for round_index in range(arguments.launches):
            for app in selected:
                record = {"label": arguments.label, "app": app.name,
                          "round": round_index, "cpus": arguments.cpus,
                          "engine": arguments.engine,
                          "boot_seconds": boot_seconds,
                          "boot_emulator_cpu": boot_cpu}
                if f"package:{app.package}\n" not in installed + "\n":
                    record.update(seconds=None, missing=True)
                else:
                    record.update(launch(
                        guest, app, emulator_pid,
                        scenario if scenario and scenario.app == app.name
                        else None))
                with arguments.output.open("a") as handle:
                    handle.write(json.dumps(record) + "\n")
                print(json.dumps(record), flush=True)
    finally:
        status = guest.power_off()
        print(f"guest exit status {status}", flush=True)
    return 0 if status == 0 else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--summary", nargs="+", type=pathlib.Path,
                        help="print the medians of these record files and exit")
    parser.add_argument("--emulator", type=pathlib.Path)
    parser.add_argument("--release", type=pathlib.Path)
    parser.add_argument("--source-instance", type=pathlib.Path,
                        help="a prepared instance directory (data.img and "
                             "instance.json) that is never written")
    parser.add_argument("--work-instance", type=pathlib.Path,
                        help="the instance directory this run boots, "
                             "recreated from the source")
    parser.add_argument("--adb-port", type=int,
                        default=slot_ports.slot_port(slot_ports.ADB_OFFSET,
                                                     DEFAULT_ADB_PORT),
                        help="the adb forward's port (default: the guest "
                             "slot's first port when GUEST_SLOT_PORT_BASE is "
                             f"set, else {DEFAULT_ADB_PORT})")
    parser.add_argument("--apps", default="settings,3dmark,ab2")
    parser.add_argument("--launches", type=int, default=2)
    parser.add_argument("--label", default="run")
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--console", type=pathlib.Path)
    parser.add_argument("--env", action="append", default=[])
    parser.add_argument("--guest-samples")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--engine", choices=("gadgets", "jit"),
                        default="gadgets")
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--keep-setup-wizard", action="store_true",
                        help="do not provision the guest after its boot "
                             "(boot_android.py --provision), so a source "
                             "instance that never left Setup Wizard is "
                             "measured in it")
    parser.add_argument("--settle", type=int, default=45)
    parser.add_argument("--display", choices=("native", "vnc"),
                        default="native")
    parser.add_argument("--vnc-port", type=int, default=5900)
    parser.add_argument("--scenario", type=pathlib.Path,
                        help="continue one application's launch into a "
                             "phase it reports (JSON, see android_scenarios/)")
    return parser


def main() -> int:
    parser = build_parser()
    arguments = parser.parse_args()
    if arguments.summary:
        records = [json.loads(line) for path in arguments.summary
                   for line in path.read_text().splitlines() if line]
        print(json.dumps(summarize(records), indent=2))
        return 0
    for required in ("emulator", "release", "source_instance", "work_instance",
                     "output"):
        if getattr(arguments, required) is None:
            parser.error(f"--{required.replace('_', '-')} is required")
    if arguments.work_instance.resolve() == arguments.source_instance.resolve():
        parser.error("--work-instance must differ from --source-instance")
    if arguments.console is None:
        arguments.console = arguments.output.with_suffix(".console.log")
    return run(arguments)


if __name__ == "__main__":
    raise SystemExit(main())
