#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""run_ios_device_android.py drives an iOS device only through its tools.

A fake ``xcrun devicectl``, ``iproxy``, ``adb`` and ``codesign`` on PATH
(fake_device_tools.py, wrapped by scripts this test writes into its scratch
directory) model the application's container, the console of Andarium and the
guest. The runner is run against them for the happy path and for the ways a
device run goes wrong: adb that never gets ready, a console that never reports
the end, an alert nobody answers, and what a run leaves in the container
(nothing).
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import io
import json
import os
import pathlib
import random
import socket
import stat
import sys
import tempfile
import unittest
from unittest import mock

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
sys.path.insert(0, str(REPOSITORY / "tests" / "perf"))
import run_ios_device_android as runner  # noqa: E402

DEVICE = "fake-device"
EXISTING = {"keep.txt": b"the owner's file", "rootfs.erofs": b"x" * 64}
DEVICECTL_ALLOWED = (
    ["device", "info", "lockState"],
    ["device", "install", "app"],
    ["device", "process", "launch"],
)
SCENARIO = {
    "app": "settings",
    "steps": [{"sleep": 0}, {"tap": [10, 20]}],
    "done": "SCENARIO DONE",
    "limit_seconds": 5,
    "values": {"score": "score=(\\d+)"},
}


def free_port() -> int:
    while True:
        port = random.randrange(30000, 40000)
        with socket.socket() as probe:
            if probe.connect_ex(("127.0.0.1", port)) != 0:
                return port


class Outcome:
    def __init__(self, status: int, out: str, err: str,
                 scratch: "Scratch", artifacts: pathlib.Path,
                 host_port: int) -> None:
        self.status = status
        self.out = out
        self.err = err
        self.scratch = scratch
        self.artifacts = artifacts
        self.host_port = host_port

    @property
    def commands(self) -> list[dict]:
        log = self.scratch.state / "commands.jsonl"
        if not log.exists():
            return []
        return [json.loads(line) for line in log.read_text().splitlines()]

    def tool(self, name: str) -> list[dict]:
        return [c for c in self.commands if c["tool"] == name]

    def devicectl(self, *prefix: str) -> list[dict]:
        return [c for c in self.tool("devicectl")
                if c["args"][:len(prefix)] == list(prefix)]

    @property
    def summary(self) -> dict:
        (path,) = list(self.artifacts.glob("*/summary.json"))
        return json.loads(path.read_text())

    @property
    def documents(self) -> dict[str, bytes]:
        return {p.name: p.read_bytes()
                for p in sorted(self.scratch.documents.iterdir())}


class Scratch:
    """The fake tools, the fake container and the files a run reads."""

    def __init__(self, root: pathlib.Path) -> None:
        self.root = root
        self.state = root / "state"
        self.documents = self.state / "documents"
        self.documents.mkdir(parents=True)
        for name, contents in EXISTING.items():
            (self.documents / name).write_bytes(contents)
        bin_dir = root / "bin"
        bin_dir.mkdir()
        for tool in ("xcrun", "iproxy", "adb", "codesign"):
            wrapper = bin_dir / tool
            wrapper.write_text(
                f"#!/bin/sh\nexec {sys.executable} -S -B "
                f"{HERE / 'fake_device_tools.py'} {tool} \"$@\"\n"
            )
            wrapper.chmod(wrapper.stat().st_mode | stat.S_IXUSR)
        self.bin = bin_dir
        self.script = root / "workload.sh"
        self.script.write_text("#!/bin/sh\necho hello\n")
        self.package = root / "Cts.apk"
        self.package.write_bytes(b"PK apk bytes")
        self.scenario = root / "scenario.json"
        self.scenario.write_text(json.dumps(SCENARIO))
        self.runs = 0

    def app(self, *, automation: bool) -> pathlib.Path:
        binary = (self.root / "Andarium.app" / "Frameworks"
                  / "KRuntime_Android_6_18_4K.framework" / "KRuntime_Android_6_18_4K")
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_bytes(
            b"x\0KERNARIUM_ANDROID_ADB_AUTH\0y" if automation
            else b"release runtime"
        )
        return binary.parents[2]

    def run(self, *arguments: str, environment: dict[str, str] | None = None,
            port: bool = True) -> Outcome:
        host_port = free_port()
        self.runs += 1
        artifacts = self.root / f"artifacts-{self.runs}"
        variables = {
            "PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}",
            "FAKE_STATE": str(self.state),
            **(environment or {}),
        }
        argv = ["--device", DEVICE, "--poll-interval", "0.05",
                "--launch-settle", "0",
                "--artifact-root", str(artifacts), *arguments]
        if port:
            variables["GUEST_SLOT_PORT_BASE"] = str(host_port)
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.dict(os.environ, variables), \
                contextlib.redirect_stdout(out), \
                contextlib.redirect_stderr(err):
            if not port:
                os.environ.pop("GUEST_SLOT_PORT_BASE", None)
            # Whatever the runner's adb sees of the server selection is the
            # runner's own doing, not the developer's shell.
            os.environ.pop("ANDROID_ADB_SERVER_PORT", None)
            status = runner.main(argv)
        return Outcome(status, out.getvalue(), err.getvalue(), self,
                       artifacts, host_port)


class AndroidRunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.scratch = Scratch(pathlib.Path(directory.name))

    def run_android(self, *arguments: str, **keywords: object) -> Outcome:
        return self.scratch.run(*arguments, **keywords)

    def assert_container_untouched(self, outcome: Outcome) -> None:
        self.assertEqual(outcome.documents, EXISTING)

    def assert_only_allowed_devicectl(self, outcome: Outcome) -> None:
        for command in outcome.tool("devicectl"):
            self.assertIn(command["args"][:3], DEVICECTL_ALLOWED)
            self.assertNotIn("--remove-existing-content", command["args"])
            self.assertNotIn("uninstall", command["args"])

    def assert_adb_follows_launch_and_tunnel(self, outcome: Outcome) -> None:
        """No adb call is recorded before the launch and the tunnel are.

        The runner starts the launch and the tunnel back to back and neither
        needs the other to have started, so their order in the log is the
        scheduler's and is not asserted. It holds adb back until the console
        shows the guest (the launch) and the tunnel listens.
        """
        order = [c["tool"] for c in outcome.commands]
        first_adb = order.index("adb")
        self.assertIn("launch", order[:first_adb])
        self.assertIn("iproxy", order[:first_adb])

    def test_a_run_boots_works_powers_off_and_leaves_nothing(self) -> None:
        scratch = self.scratch
        app = scratch.app(automation=True)
        leaked = {"DEVICECTL_CHILD_KERNARIUM_ROOTFS": "leak",
                  "DEVICECTL_CHILD_ANDARIUM_ERASE": "1"}
        outcome = self.run_android(
            "--app", str(app),
            "--script", str(scratch.script),
            "--push", f"{scratch.package}:/data/local/tmp/Cts.apk",
            "--launch-app", "settings", "--scenario", str(scratch.scenario),
            "--memory-mib", "1536", "--cpus", "2",
            environment=leaked,
        )
        self.assertEqual(outcome.status, 0, outcome.err)
        self.assert_only_allowed_devicectl(outcome)
        self.assert_container_untouched(outcome)

        (launch,) = outcome.tool("launch")
        port = outcome.host_port
        environment = launch["environment"]
        self.assertEqual(environment["ANDARIUM_START"], "normal")
        self.assertNotIn("ANDARIUM_SYSTEM_IMAGE", environment)
        self.assertEqual(environment["KERNARIUM_VCPU_COUNT"], "2")
        self.assertEqual(environment["KERNARIUM_GUEST_MEMORY_MIB"], "1536")
        self.assertEqual(environment["KERNARIUM_ANDROID_ADB_AUTH"], "0")
        self.assertEqual(environment["KERNARIUM_HOST_FORWARD"],
                         f"tcp:127.0.0.1:{port}-5555")
        self.assertNotIn("KERNARIUM_ROOTFS", environment)
        self.assertNotIn("ANDARIUM_ERASE", environment)
        self.assertEqual(launch["args"][-1], "com.imxieyi.andarium")

        (proxy,) = outcome.tool("iproxy")
        self.assertEqual(proxy["args"], ["-u", DEVICE, f"{port}:{port}"])
        adb_calls = outcome.tool("adb")
        self.assertTrue(adb_calls)
        # The shared adb server: the runner selects no server, never stops
        # one, names the guest with -s, and ends by dropping its own transport.
        serial = f"127.0.0.1:{port}"
        for call in adb_calls:
            self.assertIsNone(call["server"])
            self.assertNotIn("-P", call["args"])
            self.assertNotIn("kill-server", call["args"])
            if call["args"][0] not in ("connect", "disconnect"):
                self.assertEqual(call["args"][:2], ["-s", serial])
        self.assertEqual(adb_calls[0]["args"], ["connect", serial])
        self.assertEqual(adb_calls[-1]["args"], ["disconnect", serial])
        self.assert_adb_follows_launch_and_tunnel(outcome)
        self.assertEqual(outcome.devicectl("device", "copy"), [])
        self.assertEqual(len(outcome.devicectl("device", "install", "app")), 1)
        self.assertEqual(len(outcome.tool("codesign")), 1)
        shells = [c["args"][-1] for c in adb_calls if "shell" in c["args"]]
        self.assertEqual(shells.count("reboot -p"), 1)
        self.assertLess(
            max(i for i, s in enumerate(shells)
                if s.startswith("am start -W")),
            shells.index("reboot -p"),
        )

        summary = outcome.summary
        self.assertEqual(summary["status"], "ok")
        self.assertEqual(summary["runtime_result"], 0)
        self.assertEqual(summary["host_port"], port)
        self.assertNotIn("system_image", summary)
        self.assertNotIn("cleanup", summary)
        self.assertEqual(summary["thermal"]["start"]["state"], "nominal")
        self.assertEqual(summary["thermal"]["finish"]["state"], "fair")
        self.assertTrue(summary["thermal"]["complete"])
        self.assertEqual(summary["workload"]["script"]["exit_status"], 0)
        (pushed,) = summary["workload"]["pushes"]
        self.assertEqual(pushed["file"], "Cts.apk")
        self.assertEqual(pushed["guest"], "/data/local/tmp/Cts.apk")
        self.assertEqual(pushed["bytes"], len(b"PK apk bytes"))
        (launched,) = summary["workload"]["launches"]
        self.assertEqual(launched["total_time_ms"], 321)
        self.assertEqual(launched["seconds"], 0.8)
        (followed,) = summary["workload"]["scenarios"]
        self.assertEqual(followed["seconds"], 100.0)
        self.assertEqual(followed["values"], {"score": 42.0})
        (artifact,) = outcome.artifacts.iterdir()
        console = (artifact / "console.log").read_text()
        self.assertIn("Andarium: runtime returned 0", console)
        self.assertIn("workload ran", (artifact / "script.out").read_text())
        pushed_files = scratch.state / "pushed"
        self.assertEqual(
            (pushed_files / "andarium-harness-script.sh").read_text(),
            scratch.script.read_text())
        self.assertEqual((pushed_files / "Cts.apk").read_bytes(),
                         scratch.package.read_bytes())
        calls = [c["args"] for c in adb_calls if "push" in c["args"]]
        self.assertLess(
            next(i for i, c in enumerate(calls) if c[-1].endswith("Cts.apk")),
            next(i for i, c in enumerate(calls) if c[-1].endswith(".sh")))

    def assert_run_with_start_order(self, first: str, second: str) -> None:
        outcome = self.run_android(
            "--script", str(self.scratch.script),
            environment={"FAKE_START_AFTER": f"{second}:{first}"},
        )
        self.assertEqual(outcome.status, 0, outcome.err)
        order = [c["tool"] for c in outcome.commands]
        self.assertLess(order.index(first), order.index(second))
        self.assert_adb_follows_launch_and_tunnel(outcome)
        self.assert_container_untouched(outcome)

    def test_a_run_does_not_need_the_launch_to_start_before_the_tunnel(
        self,
    ) -> None:
        self.assert_run_with_start_order("iproxy", "launch")

    def test_a_run_does_not_need_the_tunnel_to_start_before_the_launch(
        self,
    ) -> None:
        self.assert_run_with_start_order("launch", "iproxy")

    def provisioning_commands(self, outcome: Outcome) -> list[str]:
        return [
            call["args"][-1] for call in outcome.tool("adb")
            if "shell" in call["args"]
            and call["args"][-1].startswith(
                ("settings put", "pm disable-user", "am force-stop org.lineageos.setupwizard"))
        ]

    def test_an_erased_guest_is_provisioned_before_the_workload(self) -> None:
        outcome = self.run_android(
            "--erase", "--script", str(self.scratch.script),
            "--launch-app", "settings",
            environment={"FAKE_FRESH_GUEST": "1"},
        )
        self.assertEqual(outcome.status, 0, outcome.err)
        self.assertTrue(outcome.summary["provisioned"])
        commands = self.provisioning_commands(outcome)
        self.assertEqual(
            commands[:2],
            ["settings put global device_provisioned 1",
             "settings put secure user_setup_complete 1"])
        # The disabled wizard's process holds the status bar, so the helper stops it too.
        self.assertEqual(commands[2:], ["pm disable-user --user 0 org.lineageos.setupwizard",
                                        "am force-stop org.lineageos.setupwizard"])
        shells = [c["args"][-1] for c in outcome.tool("adb") if "shell" in c["args"]]
        first_work = min(i for i, s in enumerate(shells)
                         if s.startswith(("sh /data/local/tmp/", "am start -W")))
        self.assertLess(shells.index(commands[-1]), first_work)
        # The helper changes nothing until user 0 is unlocked and the wizard has bound.
        self.assertLess(shells.index("am get-started-user-state 0"), shells.index(commands[0]))

    def test_an_erased_guest_that_stays_in_the_wizard_fails_the_run(self) -> None:
        outcome = self.run_android(
            "--erase", "--script", str(self.scratch.script),
            environment={"FAKE_FRESH_GUEST": "1", "FAKE_PROVISION": "ignored"},
        )
        self.assertEqual(outcome.status, 1)
        self.assertIn("not provisioned", outcome.err)
        self.assertIsNone(outcome.summary["workload"])

    def test_the_wizard_can_be_kept_to_measure_the_first_boot(self) -> None:
        outcome = self.run_android(
            "--erase", "--keep-setup-wizard", "--script", str(self.scratch.script),
            environment={"FAKE_FRESH_GUEST": "1"},
        )
        self.assertEqual(outcome.status, 0, outcome.err)
        self.assertNotIn("provisioned", outcome.summary)
        self.assertEqual(self.provisioning_commands(outcome), [])

    def test_a_guest_that_is_not_erased_is_not_provisioned(self) -> None:
        outcome = self.run_android(
            "--script", str(self.scratch.script),
            environment={"FAKE_FRESH_GUEST": "1"},
        )
        self.assertEqual(outcome.status, 0, outcome.err)
        self.assertNotIn("provisioned", outcome.summary)
        self.assertEqual(self.provisioning_commands(outcome), [])

    def test_a_push_that_fails_ends_the_workload_not_the_poweroff(
            self) -> None:
        outcome = self.run_android(
            "--script", str(self.scratch.script),
            "--push", f"{self.scratch.package}:/data/local/tmp/Cts.apk",
            environment={"FAKE_ADB_PUSH": "fail"},
        )
        self.assertEqual(outcome.status, 1)
        self.assertIn("could not push", outcome.err + outcome.out)
        self.assertEqual(outcome.summary["runtime_result"], 0)
        self.assertNotIn(
            "andarium-harness-script.sh",
            "".join(c["args"][-1] for c in outcome.tool("adb")
                    if "shell" in c["args"]))

    def test_adb_that_never_gets_ready_records_no_run(self) -> None:
        outcome = self.run_android(
            "--script", str(self.scratch.script), "--boot-timeout", "1",
            environment={"FAKE_ADB": "never-ready"},
        )
        self.assertEqual(outcome.status, 1)
        self.assertIn("adb never became ready", outcome.err)
        self.assertIsNone(outcome.summary["workload"])
        self.assertEqual(outcome.summary["status"], "failed")
        for call in outcome.tool("adb"):
            self.assertNotIn("push", call["args"])
        self.assertTrue((self.scratch.state / "terminated").exists())
        self.assertFalse((self.scratch.state / "poweroff").exists())
        self.assert_container_untouched(outcome)
        self.assert_only_allowed_devicectl(outcome)

    def test_a_console_that_never_reports_the_end_fails_the_run(self) -> None:
        outcome = self.run_android(
            "--script", str(self.scratch.script), "--poweroff-timeout", "1",
            environment={"FAKE_CONSOLE": "silent"},
        )
        self.assertEqual(outcome.status, 1)
        self.assertIn("did not report the runtime returning", outcome.err)
        self.assertIsNone(outcome.summary["runtime_result"])
        # The workload ran and the guest was asked to power off.
        self.assertEqual(
            outcome.summary["workload"]["script"]["exit_status"], 0)
        self.assertTrue((self.scratch.state / "poweroff").exists())
        self.assertTrue((self.scratch.state / "terminated").exists())
        self.assert_container_untouched(outcome)

    def test_the_run_waits_for_an_alert_to_be_answered(self) -> None:
        outcome = self.run_android(
            "--script", str(self.scratch.script), "--wait-active", "30",
            environment={"FAKE_FOCUS": "alert"},
        )
        self.assertEqual(outcome.status, 0)
        self.assertIn("waited_for_active_seconds", outcome.summary)
        self.assertEqual(
            outcome.summary["workload"]["script"]["exit_status"], 0)

    def test_an_alert_nobody_answers_ends_the_run_before_its_workload(
        self,
    ) -> None:
        outcome = self.run_android(
            "--script", str(self.scratch.script), "--wait-active", "2",
            environment={"FAKE_FOCUS": "stuck"},
        )
        self.assertEqual(outcome.status, 1)
        self.assertIn("still inactive after 2 seconds", outcome.err)
        self.assertIsNone(outcome.summary["workload"])
        self.assertEqual(outcome.summary["status"], "failed")
        self.assert_container_untouched(outcome)

    def test_a_guest_that_ends_on_its_own_is_not_a_run(self) -> None:
        outcome = self.run_android(
            "--script", str(self.scratch.script),
            environment={"FAKE_GUEST": "dies"},
        )
        self.assertEqual(outcome.status, 1)
        self.assertIn("the guest ended on its own", outcome.err)
        self.assertIsNone(outcome.summary["workload"])

    def test_the_forward_port_must_come_from_somewhere(self) -> None:
        outcome = self.run_android(port=False)
        self.assertEqual(outcome.status, 1)
        self.assertIn("GUEST_SLOT_PORT_BASE is not set", outcome.err)
        self.assertIn("--host-port", outcome.err)
        self.assertEqual(outcome.commands, [])
        self.assertFalse(outcome.artifacts.exists())
        explicit = free_port()
        outcome = self.run_android("--host-port", str(explicit), port=False)
        self.assertEqual(outcome.status, 0, outcome.err)
        (launch,) = outcome.tool("launch")
        self.assertEqual(launch["environment"]["KERNARIUM_HOST_FORWARD"],
                         f"tcp:127.0.0.1:{explicit}-5555")

    def test_a_release_runtime_is_refused_before_anything_boots(self) -> None:
        outcome = self.run_android(
            "--app", str(self.scratch.app(automation=False)))
        self.assertEqual(outcome.status, 1)
        self.assertIn("release runtime", outcome.err)
        self.assertEqual(outcome.devicectl("device", "install", "app"), [])
        outcome = self.run_android(
            "--launch-timeout", "1", environment={"FAKE_AUTH": "missing"})
        self.assertEqual(outcome.status, 1)
        self.assertIn("did not report ADB authorization off", outcome.err)
        # No guest is addressed and no server is stopped: the run only drops
        # its own transport from the shared server.
        self.assertEqual(
            [c["args"] for c in outcome.tool("adb")],
            [["disconnect", f"127.0.0.1:{outcome.host_port}"]])

    def test_a_locked_device_is_not_touched(self) -> None:
        outcome = self.run_android(
            "--app", str(self.scratch.app(automation=True)),
            environment={"FAKE_LOCKED": "1"})
        self.assertEqual(outcome.status, 1)
        self.assertIn("passcode-locked", outcome.err)
        self.assertEqual(outcome.devicectl("device", "install", "app"), [])
        self.assertEqual(outcome.tool("launch"), [])

    def test_a_bad_input_costs_no_boot(self) -> None:
        for arguments, message in (
            (("--launch-app", "nothing"), "no application 'nothing'"),
            (("--scenario", str(self.scratch.scenario)),
             "not a --launch-app"),
            (("--adb", "/nonexistent/adb"), "adb is unavailable"),
            (("--push", "/nonexistent/Cts.apk:/data/local/tmp/Cts.apk"),
             "no file to push"),
            (("--push", f"{self.scratch.package}:relative/Cts.apk"),
             "absolute guest path"),
            (("--push", str(self.scratch.package)), "absolute guest path"),
        ):
            outcome = self.run_android(*arguments)
            self.assertEqual(outcome.status, 1, arguments)
            self.assertIn(message, outcome.err)
            self.assertEqual(outcome.commands, [])


if __name__ == "__main__":
    unittest.main()
