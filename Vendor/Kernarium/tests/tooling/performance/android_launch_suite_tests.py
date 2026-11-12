#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Contract tests for the Android launch suite, without a guest.

A launch time is only as good as its end marker and the arithmetic around it:
the checked-in application manifest must parse, a launch must be timed from
the activity manager's START line to its marker by logcat's own timestamps
and never from a line older than the launch, CPU time
must convert from every `ps` spelling, thread priorities must be read from
`ps -M` (a napped emulator is several times slower), and a failed launch must stay out of
the medians while still being counted. Checked-in scenarios must parse, and a
scenario's values come only from its completion line after its steps ended.
Powering off must kill a launcher that ignores SIGTERM and its emulator too.
The guest defaults to 4 vCPUs.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import importlib.util
import json
import os
import pathlib
import re
import signal
import subprocess
import sys
import tempfile
from unittest import mock

SUITE = pathlib.Path(__file__).resolve().parents[2] / "perf" / "android_launch_suite.py"


def load_suite():
    spec = importlib.util.spec_from_file_location("android_launch_suite", SUITE)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    # Dataclasses resolve their annotations through sys.modules.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_manifest(suite) -> None:
    apps = suite.read_manifest(suite.MANIFEST)
    check(len(apps) >= 2, "the suite needs more than one application")
    for app in apps.values():
        check(app.package and "/" in app.component, f"{app.name}: component")
        check(app.limit > 0, f"{app.name}: limit")
    with tempfile.TemporaryDirectory() as directory:
        broken = pathlib.Path(directory) / "apps.tsv"
        broken.write_text("name\tcomponent\tdone\tlimit_seconds\nx\tno-slash\ty\t1\n")
        try:
            suite.read_manifest(broken)
        except ValueError:
            pass
        else:
            raise AssertionError("a component without an activity must be rejected")


def test_first_match(suite) -> None:
    pattern = re.compile(r"Scene loaded: \(Main\)")
    logcat = "\n".join([
        "--------- beginning of main",
        "  1000.500  1234  1250 I Unity   : Scene loaded: (Main)",
        "  1002.250  1234  1250 I Unity   : Scene loaded: (Loading)",
        "  1003.125  1234  1250 I Unity   : Scene loaded: (Main)",
        "  1004.000  1234  1250 I Unity   : Scene loaded: (Main)",
    ])
    check(suite.first_match(logcat, pattern, 1001.0) == 1003.125,
          "a marker older than the launch was taken")
    check(suite.first_match(logcat, pattern, 1005.0) is None,
          "a missing marker must not match")


def test_launch_seconds(suite) -> None:
    app = suite.read_manifest(suite.MANIFEST)["settings"]
    start = ("  1790365599.999   544  1609 I ActivityTaskManager: START u0 "
             "{flg=0x10000000 xflg=0x5 cmp=com.android.settings/.Settings} "
             "with LAUNCH_MULTIPLE from uid 2000 (com.android.shell) result code=0")
    shown = ("  1790365602.570   544   569 I ActivityTaskManager: Displayed "
             "com.android.settings/.Settings for user 0: +2s633ms")
    stale = ("  1790365590.000   544   569 I ActivityTaskManager: Displayed "
             "com.android.settings/.Settings for user 0: +1s000ms")
    check(suite.launch_seconds("\n".join([stale, start, shown]), app) == 2.571,
          "a launch is timed from its START line to the next marker")
    check(suite.launch_seconds(shown, app) is None,
          "a marker without its START line must not be timed")
    check(suite.launch_seconds("\n".join([start, stale]), app) is None,
          "a marker older than its START line must not be timed")


def test_cpu_seconds(suite) -> None:
    check(suite.cpu_seconds("0:01.50\n") == 1.5, "mm:ss")
    check(suite.cpu_seconds("2:03:04.25") == 7384.25, "hh:mm:ss")
    check(suite.cpu_seconds("1-00:00:01.00") == 86401.0, "dd-hh:mm:ss")


def test_thread_priorities(suite) -> None:
    ps_threads = "\n".join([
        "USER    PID   TT   %CPU STAT PRI     STIME     UTIME COMMAND",
        "alice 31478   ??    0.0 S    47T   0:00.09   0:00.35 /x/uml-darwin-android17 --a",
        "      31478        25.7 R    31T   0:00.17   0:01.32 ",
        "      31478         0.0 S     4    1:02.01  10:00.01 ",
        "      31478         0.0 S    31T   0:00.00   0:00.00 ",
    ])
    check(suite.thread_priorities(ps_threads) == {"47": 1, "31": 2, "4": 1},
          f"unexpected priorities {suite.thread_priorities(ps_threads)}")


def records() -> list[dict]:
    return [
        {"label": "A", "app": "x", "round": 0, "seconds": 10.0, "emulator_cpu": 50.0},
        {"label": "A", "app": "x", "round": 1, "seconds": 12.0, "emulator_cpu": 54.0},
        {"label": "A", "app": "x", "round": 2, "seconds": None, "emulator_cpu": 90.0},
        {"label": "B", "app": "x", "round": 0, "seconds": None, "missing": True},
    ]


def test_summary(suite) -> None:
    summary = suite.summarize(records())
    check(summary["A"]["x"] == {"launches": 3, "completed": 2,
                                "median_seconds": 11.0,
                                "median_emulator_cpu": 52.0},
          f"unexpected summary {summary['A']['x']}")
    check(summary["B"]["x"] == {"launches": 1, "completed": 0},
          "an arm with no completed launch has no median")
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory) / "runs.jsonl"
        path.write_text("".join(json.dumps(record) + "\n" for record in records()))
        result = subprocess.run(
            [sys.executable, str(SUITE), "--summary", str(path)],
            capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=30)
        check(result.returncode == 0, result.stderr)
        check(json.loads(result.stdout) == summary, "--summary disagrees")


def test_scenarios(suite) -> None:
    directory = SUITE.parent / "android_scenarios"
    apps = suite.read_manifest(suite.MANIFEST)
    paths = sorted(directory.glob("*.json"))
    check(paths, "no checked-in scenario")
    for path in paths:
        scenario = suite.read_scenario(path)
        check(scenario.app in apps, f"{path.name}: unknown app {scenario.app}")
        check(scenario.limit > 0 and scenario.values, f"{path.name}: limits")
    wle = suite.read_scenario(directory / "3dmark-wild-life-extreme.json")
    result = ('{"events": [{"eventType": "INITIALIZATION_COMPLETED", '
              '"msSinceStart": 37426}, {"eventType": "WORKLOAD_COMPLETED", '
              '"msSinceStart": 97577}], "outputs": [{"outputType": '
              '"TYPED_RESULT", "resultType": "WILD_LIFE_GT1_X", '
              '"value": 35.44}]}')
    logcat = (
        "  100.000  1  1 D x.AttanWorkloadActivity: Result: {\"events\": []}\n"
        f"  200.500  1  1 D x.AttanWorkloadActivity: Result: {result}\n")
    found = suite.scenario_values(logcat, wle, 150.0)
    check(found == {"done_at": 200.5, "values": {
        "initialization_ms": 37426.0, "workload_completed_ms": 97577.0,
        "fps": 35.44}}, f"unexpected scenario values {found}")
    check(suite.scenario_values(logcat, wle, 300.0) is None,
          "a result older than the steps must not complete the scenario")
    with tempfile.TemporaryDirectory() as temporary:
        broken = pathlib.Path(temporary) / "broken.json"
        broken.write_text(json.dumps({"app": "x", "steps": [{"tap": [1]}],
                                      "done": "y", "limit_seconds": 1}))
        try:
            suite.read_scenario(broken)
        except ValueError:
            pass
        else:
            raise AssertionError("a tap without two coordinates must be rejected")


def test_default_cpus(suite) -> None:
    with tempfile.TemporaryDirectory() as directory:
        output = pathlib.Path(directory) / "launches.jsonl"
        arguments = [str(SUITE), "--emulator", "emulator", "--release",
                     "release", "--source-instance", "source", "--work-instance",
                     "work", "--output", str(output)]
        seen = []
        with mock.patch.object(sys, "argv", arguments), \
                mock.patch.object(suite, "run",
                                  side_effect=lambda a: seen.append(a) or 0):
            check(suite.main() == 0, "the suite did not run")
    check(seen[0].cpus == 4,
          f"the suite boots {seen[0].cpus} vCPUs by default, not 4")
    check(seen[0].engine == "gadgets",
          f"the suite boots the {seen[0].engine} engine by default, not "
          "gadgets")


def test_guest_is_provisioned_unless_the_wizard_is_kept(suite) -> None:
    """The launcher provisions the guest after its boot (--provision)."""
    for extra, provisioned in (([], True), (["--keep-setup-wizard"], False)):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "launches.jsonl"
            arguments = [str(SUITE), "--emulator", "emulator", "--release",
                         "release", "--source-instance", "source",
                         "--work-instance", "work", "--output", str(output),
                         *extra]
            seen = []
            with mock.patch.object(sys, "argv", arguments), \
                    mock.patch.object(suite, "run",
                                      side_effect=lambda a: seen.append(a) or 0):
                check(suite.main() == 0, "the suite did not run")
            console = pathlib.Path(directory) / "console.log"
            with mock.patch.object(suite.subprocess, "Popen") as popen:
                seen[0].console = console
                suite.Guest(seen[0], {})
            command = popen.call_args.args[0]
            check(("--provision" in command) == provisioned,
                  f"{command} with {extra}: --provision should be "
                  f"{'present' if provisioned else 'absent'}")


# A launcher that ignores SIGTERM, with an emulator stand-in that leads its own
# session, as boot_android.py starts the real one.
STUBBORN_LAUNCHER = """
import os, signal, subprocess, sys, time
signal.signal(signal.SIGTERM, signal.SIG_IGN)
emulator = subprocess.Popen(
    [sys.executable, "-c", "import time; time.sleep(60)"],
    start_new_session=True)
print(os.getpid(), emulator.pid, flush=True)
time.sleep(60)
"""


def test_power_off_kills_a_stubborn_launcher(suite) -> None:
    graces = (suite.POWER_OFF_SECONDS, suite.LAUNCHER_STOP_SECONDS)
    emulator = None
    launcher = subprocess.Popen(
        [sys.executable, "-c", STUBBORN_LAUNCHER], stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE)
    assert launcher.stdout is not None
    try:
        _, emulator = map(int, launcher.stdout.readline().split())
        with tempfile.TemporaryDirectory() as directory:
            guest = suite.Guest.__new__(suite.Guest)
            guest.process = launcher
            guest.console = open(pathlib.Path(directory) / "console.log", "ab")

            def unreachable_shell(command: str, timeout: int = 60) -> str:
                raise subprocess.TimeoutExpired(command, timeout)

            guest.shell = unreachable_shell
            suite.POWER_OFF_SECONDS = suite.LAUNCHER_STOP_SECONDS = 1
            status = guest.power_off()
        check(status == -signal.SIGKILL,
              f"a launcher that ignores SIGTERM must be killed, not {status}")
        check(not suite.process_exists(emulator),
              "the emulator outlived its launcher")
    finally:
        suite.POWER_OFF_SECONDS, suite.LAUNCHER_STOP_SECONDS = graces
        # While the launcher lives, the emulator's PID is still its child's.
        if launcher.poll() is None:
            if emulator is not None:
                try:
                    os.killpg(emulator, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            launcher.kill()
        launcher.wait(timeout=10)
        launcher.stdout.close()


def main() -> int:
    suite = load_suite()
    test_manifest(suite)
    test_first_match(suite)
    test_launch_seconds(suite)
    test_cpu_seconds(suite)
    test_thread_priorities(suite)
    test_summary(suite)
    test_scenarios(suite)
    test_default_cpus(suite)
    test_guest_is_provisioned_unless_the_wizard_is_kept(suite)
    test_power_off_kills_a_stubborn_launcher(suite)
    print("android launch suite contract: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
