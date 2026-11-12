#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The harness helper that provisions an erased Android guest.

A fake guest answers the shell commands the helper issues and models the two
settings, the user's unlock and the wizard's HOME activity and process, so the
test states what the helper does to a fresh guest, leaves alone in a
provisioned one, and refuses when the guest does not end up provisioned. The
fake counts a disable that lands while the wizard could still be binding, which
the helper must never issue. The real guest's reference state (what
LineageOS Setup Wizard leaves behind when a person completes it) is checked by
tests/integration/android/run_android_oobe.py.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("android_provision", ROOT / "tools/android_provision.py")
assert SPEC is not None and SPEC.loader is not None
android_provision = importlib.util.module_from_spec(SPEC)
sys.modules["android_provision"] = android_provision
SPEC.loader.exec_module(android_provision)

# `cmd package resolve-activity --brief` as the release prints it.
WIZARD_HOME = (
    "priority=9 preferredOrder=0 match=0x108000 specificIndex=-1 isDefault=true\n"
    "org.lineageos.setupwizard/.SetupWizardActivity\n"
)
# `dumpsys statusbar` as the release prints its disable records: the wizard's lock, and SystemUI's own.
LOCKED_BAR = (
    "    mDisabled1=0x3a50000\n"
    "  mDisableRecords.size=3\n"
    "    [0] userId=0 what1=0x03A50000 what2=0x00000000 pkg=org.lineageos.setupwizard token=android.os.BinderProxy@2065e8e\n"
    "    [1] userId=0 what1=0x00000000 what2=0x00000000 pkg=com.android.systemui token=android.os.BinderProxy@d59b6af\n"
)
FREE_BAR = (
    "    mDisabled1=0x0\n"
    "  mDisableRecords.size=2\n"
    "    [0] userId=0 what1=0x00000000 what2=0x00000000 pkg=com.android.systemui token=android.os.BinderProxy@d59b6af\n"
)
LAUNCHER_HOME = (
    "priority=0 preferredOrder=0 match=0x108000 specificIndex=-1 isDefault=true\n"
    "com.android.launcher3/.uioverrides.QuickstepLauncher\n"
)


class FakeGuest:
    """The parts of a guest the helper reads and writes."""

    def __init__(self, provisioned: str = "0", setup_complete: str = "0",
                 disabled: tuple[str, ...] = (), ignores_disable: bool = False,
                 adbd: list[str] | None = None, trade_in_log: list[str] | None = None,
                 wizard_running: bool = True, restarts: int = 0, survives_force_stop: bool = False,
                 locked_reads: int = 0, unbound_reads: int = 0, launcher_home: bool = False,
                 kills_on_disable: bool = False, dump_unreadable_early: int = 0, home_unreadable: int = 0) -> None:
        self.settings = {("global", "device_provisioned"): provisioned,
                         ("secure", "user_setup_complete"): setup_complete}
        self.disabled = set(disabled)
        self.ignores_disable = ignores_disable
        self.commands: list[str] = []
        # What `pidof adbd` answers on each call (the last one repeats), and what the log shows on each call.
        self.adbd = adbd if adbd is not None else ["500"]
        self.trade_in_log = trade_in_log if trade_in_log is not None else [""]
        self.polls = 0
        # The wizard's process holds the status bar lock for as long as it runs. A restart is the
        # activity manager starting it once more after a force-stop, as it did after the disabling;
        # a process that survives is one the guest cannot stop.
        self.wizard_running = wizard_running
        self.restarts = restarts
        self.survives_force_stop = survives_force_stop
        self.dump_unreadable = 0
        # The wizard starts as HOME when the user unlocks: the user reads locked for `locked_reads`
        # reads of its state, and the wizard's process then takes its status bar record only after
        # `unbound_reads` reads of the dump (it is still binding until then). With `launcher_home`
        # HOME is the launcher from the start, and the wizard never runs. A process that
        # `kills_on_disable` dies when its package is disabled, as the activity manager kills it.
        self.locked_reads = locked_reads
        self.unbound_reads = unbound_reads
        self.launcher_home = launcher_home
        self.kills_on_disable = kills_on_disable
        self.dump_unreadable_early = dump_unreadable_early
        self.home_unreadable = home_unreadable
        self.state_reads = 0
        self.unsafe_disables = 0
        # The first calls of the shell fail as they do while adbd restarts: an adb error, or an exception.
        self.unreachable = 0
        self.raises = False

    def writes(self) -> list[str]:
        return [command for command in self.commands
                if command.startswith(("settings put", "pm ", "am force-stop"))]

    def home_is_wizard(self) -> bool:
        return not self.launcher_home and android_provision.SETUP_WIZARD not in self.disabled

    def holds_record(self) -> bool:
        return self.wizard_running and self.home_is_wizard() and self.unbound_reads <= 0

    def could_be_binding(self) -> bool:
        """Whether the wizard may still be attaching or installing providers: nothing may be disabled then."""
        return self.locked_reads > 0 or (self.home_is_wizard() and not self.holds_record())

    def __call__(self, command: str) -> str:
        self.commands.append(command)
        if self.unreachable > 0:
            self.unreachable -= 1
            if self.raises:
                raise subprocess.TimeoutExpired(command, 1)
            return "adb: device offline\n"
        words = command.split()
        if words[:2] == ["settings", "get"]:
            return self.settings.get((words[2], words[3]), "null") + "\n"
        if words[:2] == ["settings", "put"]:
            self.settings[(words[2], words[3])] = words[4]
            return ""
        if words[:2] == ["pm", "disable-user"]:
            assert words[2:4] == ["--user", "0"], command
            if self.could_be_binding():
                self.unsafe_disables += 1
            if not self.ignores_disable:
                self.disabled.add(words[4])
                if self.kills_on_disable:
                    self.wizard_running = False
            return f"Package {words[4]} new state: disabled-user\n"
        if words[:2] == ["pm", "disable"]:
            # The shell may not change another package's component state.
            return "java.lang.SecurityException: Shell cannot change component state\n"
        if words[:2] == ["am", "force-stop"]:
            assert words[2] == android_provision.SETUP_WIZARD, command
            if not self.survives_force_stop:
                self.wizard_running = False
                if self.restarts > 0:
                    self.restarts -= 1
                    self.wizard_running = True
            return ""
        if command == f"pidof {android_provision.SETUP_WIZARD}":
            return "1445\n" if self.wizard_running else ""
        if command == "am get-started-user-state 0":
            self.state_reads += 1
            if self.locked_reads > 0:
                self.locked_reads -= 1
                return "RUNNING_LOCKED\n" if self.locked_reads % 2 else "RUNNING_UNLOCKING\n"
            return "RUNNING_UNLOCKED\n"
        if command == "dumpsys statusbar":
            early = android_provision.SETUP_WIZARD not in self.disabled
            if (self.dump_unreadable_early if early else self.dump_unreadable) > 0:
                if early:
                    self.dump_unreadable_early -= 1
                else:
                    self.dump_unreadable -= 1
                return "Can't find service: statusbar\n"
            record = self.holds_record() if early else self.wizard_running
            self.unbound_reads -= 1
            return LOCKED_BAR if record else FREE_BAR
        if command == "pidof adbd":
            self.polls += 1
            answer = self.adbd[min(self.polls - 1, len(self.adbd) - 1)]
            return answer + "\n"
        if command.startswith("logcat -d -b all -s TradeInModeService"):
            return self.trade_in_log[min(self.polls - 1, len(self.trade_in_log) - 1)]
        if command.startswith("cmd package resolve-activity --brief"):
            if self.home_unreadable > 0:
                self.home_unreadable -= 1
                return ""
            return WIZARD_HOME if self.home_is_wizard() else LAUNCHER_HOME
        raise AssertionError(f"unexpected command: {command}")


class ProvisionTests(unittest.TestCase):
    def test_a_fresh_guest_is_provisioned_and_its_home_is_the_launcher(self) -> None:
        guest = FakeGuest()
        self.assertTrue(android_provision.provision(guest, settle_adbd=False))
        self.assertEqual(guest.settings[("global", "device_provisioned")], "1")
        self.assertEqual(guest.settings[("secure", "user_setup_complete")], "1")
        self.assertEqual(guest.disabled, {android_provision.SETUP_WIZARD})
        self.assertEqual(android_provision.home_package(guest), "com.android.launcher3")
        # Only the settings and the wizard package's state for user 0 change.
        self.assertEqual(
            guest.writes(),
            ["settings put global device_provisioned 1",
             "settings put secure user_setup_complete 1",
             f"pm disable-user --user 0 {android_provision.SETUP_WIZARD}",
             f"am force-stop {android_provision.SETUP_WIZARD}"],
        )
        # The guest's status bar is free of the wizard's lock when the helper returns.
        self.assertFalse(guest.wizard_running)
        self.assertIs(android_provision.wizard_status_bar_record(guest), False)

    def test_a_provisioned_guest_is_left_alone(self) -> None:
        guest = FakeGuest("1", "1", (android_provision.SETUP_WIZARD,))
        self.assertFalse(android_provision.provision(guest, settle_adbd=False))
        self.assertEqual(guest.writes(), [])
        self.assertTrue(android_provision.is_provisioned(guest))

    def test_a_guest_whose_wizard_still_owns_home_is_completed(self) -> None:
        guest = FakeGuest("1", "1")
        self.assertTrue(android_provision.provision(guest, settle_adbd=False))
        self.assertEqual(guest.disabled, {android_provision.SETUP_WIZARD})

    def test_a_guest_missing_one_setting_gets_it(self) -> None:
        guest = FakeGuest("1", "0", (android_provision.SETUP_WIZARD,))
        self.assertTrue(android_provision.provision(guest, settle_adbd=False))
        self.assertEqual(guest.settings[("secure", "user_setup_complete")], "1")

    def test_a_guest_that_stays_unprovisioned_is_an_error(self) -> None:
        guest = FakeGuest(ignores_disable=True)
        with self.assertRaisesRegex(android_provision.ProvisionError, "HOME='org.lineageos.setupwizard'"):
            android_provision.provision(guest)

    def test_unset_settings_read_as_not_provisioned(self) -> None:
        guest = FakeGuest("null", "null")
        self.assertFalse(android_provision.is_provisioned(guest))

    def test_home_package_reads_the_brief_form(self) -> None:
        self.assertEqual(android_provision.home_package(lambda _: WIZARD_HOME), android_provision.SETUP_WIZARD)
        self.assertEqual(android_provision.home_package(lambda _: LAUNCHER_HOME), "com.android.launcher3")
        self.assertEqual(android_provision.home_package(lambda _: "No activity found\n"), "")
        self.assertEqual(android_provision.home_package(lambda _: ""), "")

    def test_a_provisioned_guest_is_not_waited_for(self) -> None:
        guest = FakeGuest("1", "1", (android_provision.SETUP_WIZARD,))
        android_provision.provision(guest)
        self.assertEqual(guest.polls, 0)
        self.assertEqual(guest.state_reads, 0)


class Clock:
    """Time that only `sleep` advances, so the settle loop runs without waiting."""

    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


class SettleTests(unittest.TestCase):
    def provision(self, guest: FakeGuest, **options) -> Clock:
        clock = Clock()
        android_provision.provision(guest, sleep=clock.sleep, clock=clock, **options)
        return clock

    def test_a_steady_adbd_settles_after_the_stable_period(self) -> None:
        guest = FakeGuest()
        clock = self.provision(guest)
        self.assertGreaterEqual(clock.now, android_provision.SETTLE_STABLE_SECONDS)
        self.assertLess(clock.now, android_provision.SETTLE_STABLE_SECONDS + 3)

    def test_adbd_restarts_in_flight_are_waited_out(self) -> None:
        # Two restarts a few seconds apart, with adbd absent between them: the second pid has to stay.
        absent = ["", "", "", "", ""]
        guest = FakeGuest(adbd=["500", "500", "1900", *absent, "2000"],
                          trade_in_log=["", "Enabling trade-in mode.\n", "Enabling trade-in mode.\nStopping trade-in mode.\n"])
        clock = self.provision(guest)
        # The loop saw the last process come up (poll 9) and held it for the stable period.
        self.assertGreaterEqual(clock.now, 8 + android_provision.SETTLE_STABLE_SECONDS)
        self.assertGreaterEqual(guest.polls, 8 + android_provision.SETTLE_STABLE_SECONDS)

    def test_a_pending_trade_in_mode_holds_the_wait_even_with_a_steady_adbd(self) -> None:
        entered = "Enabling trade-in mode.\n"
        guest = FakeGuest(trade_in_log=[entered] * 10 + [entered + "Stopping trade-in mode.\n"])
        clock = self.provision(guest)
        self.assertGreaterEqual(clock.now, 9 + android_provision.SETTLE_STABLE_SECONDS)

    def test_a_shell_that_cannot_reach_adbd_is_retried(self) -> None:
        for raises in (False, True):
            with self.subTest(raises=raises):
                guest = FakeGuest()
                guest.unreachable, guest.raises = 7, raises
                clock = self.provision(guest)
                self.assertEqual(guest.settings[("global", "device_provisioned")], "1")
                self.assertEqual(guest.disabled, {android_provision.SETUP_WIZARD})
                # The retries slept (2 s each) before the helper went on to settle.
                self.assertGreaterEqual(clock.now, 2 * 7)

    def test_a_restart_under_the_commands_asks_again_whether_they_took(self) -> None:
        guest = FakeGuest()
        # The connection drops right after the first write, as a restart of adbd does.
        original = guest.__call__

        def shell(command: str) -> str:
            if command == "pm disable-user --user 0 " + android_provision.SETUP_WIZARD and not guest.settings.get(("x", "dropped")):
                guest.settings[("x", "dropped")] = "1"
                guest.unreachable = 4
                return "adb: device offline\n"
            return original(command)

        clock = Clock()
        self.assertTrue(android_provision.provision(shell, sleep=clock.sleep, clock=clock))
        self.assertEqual(guest.settings[("global", "device_provisioned")], "1")

    def test_a_guest_whose_shell_never_answers_is_an_error(self) -> None:
        guest = FakeGuest()
        guest.unreachable = 10_000
        with self.assertRaisesRegex(android_provision.ProvisionError, "did not answer"):
            self.provision(guest)

    def test_an_adb_error_is_not_a_process_id(self) -> None:
        self.assertEqual(android_provision.adbd_pid(lambda _: "adb: device offline\n"), "")
        self.assertEqual(android_provision.adbd_pid(lambda _: "error: closed\n"), "")
        self.assertEqual(android_provision.adbd_pid(lambda _: "534\n"), "534")

    def test_trade_in_pending_reads_the_last_of_the_two_lines(self) -> None:
        enter, stop = "I TradeInModeService: Enabling trade-in mode.\n", "I TradeInModeService: Stopping trade-in mode.\n"
        for log, expected in (("", False), (enter, True), (enter + stop, False), (enter + stop + enter, True),
                              (stop + enter, True)):
            with self.subTest(log=log):
                self.assertEqual(android_provision.trade_in_pending(lambda _: log), expected)

    def test_a_guest_that_never_settles_is_an_error(self) -> None:
        guest = FakeGuest(adbd=[str(n) for n in range(1000, 1300)])
        with self.assertRaisesRegex(android_provision.ProvisionError, "did not settle"):
            self.provision(guest)

    def test_the_wizard_is_stopped_after_it_is_disabled_and_before_adbd_is_waited_for(self) -> None:
        guest = FakeGuest()
        self.provision(guest)
        order = [command for command in guest.commands if command.startswith(("pm disable-user", "am force-stop", "pidof adbd"))]
        self.assertEqual(order[0], f"pm disable-user --user 0 {android_provision.SETUP_WIZARD}")
        self.assertEqual(order[1], f"am force-stop {android_provision.SETUP_WIZARD}")
        self.assertEqual(order[2], "pidof adbd")

    def test_a_wizard_the_activity_manager_starts_again_is_stopped_again(self) -> None:
        guest = FakeGuest(restarts=2)
        clock = self.provision(guest, settle_adbd=False)
        self.assertFalse(guest.wizard_running)
        self.assertEqual(guest.commands.count(f"am force-stop {android_provision.SETUP_WIZARD}"), 3)
        self.assertLess(clock.now, android_provision.WIZARD_RELEASE_LIMIT_SECONDS)

    def test_a_status_bar_dump_that_cannot_be_read_is_not_a_free_bar(self) -> None:
        guest = FakeGuest(kills_on_disable=True)
        guest.dump_unreadable = 3
        clock = self.provision(guest, settle_adbd=False)
        self.assertEqual(guest.dump_unreadable, 0)
        self.assertGreaterEqual(clock.now, 3)

    def test_a_wizard_that_cannot_be_stopped_is_an_error(self) -> None:
        guest = FakeGuest(survives_force_stop=True)
        with self.assertRaisesRegex(android_provision.ProvisionError,
                                    "still holds the guest 30 s after it was disabled: process 1445, status bar record held"):
            self.provision(guest)

    def test_a_dump_that_never_reads_is_an_error(self) -> None:
        guest = FakeGuest(kills_on_disable=True)
        guest.dump_unreadable = 10_000
        with self.assertRaisesRegex(android_provision.ProvisionError, "process none, status bar record unreadable"):
            self.provision(guest)

    def test_the_wizard_is_not_stopped_on_a_guest_that_is_already_provisioned(self) -> None:
        guest = FakeGuest("1", "1", (android_provision.SETUP_WIZARD,))
        self.provision(guest)
        self.assertNotIn(f"am force-stop {android_provision.SETUP_WIZARD}", guest.commands)

    def test_wizard_processes_and_records_are_read_from_the_guest_output(self) -> None:
        for answer, expected in (("", []), ("1445\n", ["1445"]), ("1445 1500\n", ["1445", "1500"]),
                                 ("adb: device offline\n", None)):
            with self.subTest(answer=answer):
                self.assertEqual(android_provision.wizard_processes(lambda _: answer), expected)
        for dump, expected in ((LOCKED_BAR, True), (FREE_BAR, False), ("", None), ("Can't find service: statusbar", None),
                               ("  mDisableRecords.size=0\n", False)):
            with self.subTest(dump=dump):
                self.assertEqual(android_provision.wizard_status_bar_record(lambda _: dump), expected)
        # A package that only shares the wizard's prefix is not the wizard.
        other = "  mDisableRecords.size=1\n    [0] userId=0 what1=0x1 what2=0x0 pkg=org.lineageos.setupwizard.other token=x\n"
        self.assertIs(android_provision.wizard_status_bar_record(lambda _: other), False)

    def test_settling_can_be_skipped(self) -> None:
        guest = FakeGuest()
        self.provision(guest, settle_adbd=False)
        self.assertEqual(guest.polls, 0)

    def test_the_adb_shell_passes_the_command_to_the_guest_and_returns_its_output(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-provision-") as scratch:
            fake = Path(scratch) / "adb"
            fake.write_text(
                "#!/bin/sh\n"
                'printf "%s|" "$@"\n'
                'printf "\\r\\n"\n'
            )
            fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
            shell = android_provision.adb_shell(str(fake), "127.0.0.1:5555")
            self.assertEqual(shell("settings get global device_provisioned"),
                             "-s|127.0.0.1:5555|shell|settings get global device_provisioned|\n")


class WizardBoundTests(unittest.TestCase):
    """The helper changes nothing while the wizard could still be binding."""

    def provision(self, guest: FakeGuest) -> Clock:
        clock = Clock()
        android_provision.provision(guest, settle_adbd=False, sleep=clock.sleep, clock=clock)
        return clock

    def test_a_locked_user_means_no_disable_until_it_is_unlocked(self) -> None:
        # FallbackHome is HOME until the user unlocks, and the wizard replaces it then: the first states are
        # not unlocked, and RUNNING_UNLOCKING is not RUNNING_UNLOCKED.
        guest = FakeGuest(locked_reads=5)
        clock = self.provision(guest)
        self.assertEqual(guest.unsafe_disables, 0)
        self.assertEqual(guest.state_reads, 6)
        self.assertGreaterEqual(clock.now, 5 * 0.5)
        self.assertEqual(guest.disabled, {android_provision.SETUP_WIZARD})
        self.assertFalse(guest.wizard_running)

    def test_no_record_yet_means_no_disable_until_the_wizard_holds_it(self) -> None:
        guest = FakeGuest(unbound_reads=4)
        self.provision(guest)
        self.assertEqual(guest.unsafe_disables, 0)
        self.assertEqual(guest.disabled, {android_provision.SETUP_WIZARD})

    def test_a_locked_user_and_a_late_record_are_both_waited_for(self) -> None:
        guest = FakeGuest(locked_reads=3, unbound_reads=3)
        self.provision(guest)
        self.assertEqual(guest.unsafe_disables, 0)

    def test_nothing_is_written_before_the_wait_ends(self) -> None:
        guest = FakeGuest(locked_reads=2, unbound_reads=2)
        self.provision(guest)
        first_write = next(index for index, command in enumerate(guest.commands) if command.startswith("settings put"))
        last_wait = max(index for index, command in enumerate(guest.commands)
                        if command in ("am get-started-user-state 0", "dumpsys statusbar") and index < first_write)
        self.assertLess(last_wait, first_write)
        # The record that ends the wait is read after the last state read.
        self.assertEqual(guest.commands[first_write - 1], "dumpsys statusbar")

    def test_a_home_that_is_not_the_wizard_proceeds_without_its_record(self) -> None:
        # The wizard never started (or finished): there is no record to wait for.
        guest = FakeGuest(launcher_home=True, wizard_running=False, unbound_reads=10_000)
        clock = self.provision(guest)
        self.assertEqual(guest.unsafe_disables, 0)
        self.assertEqual(clock.now, 0)
        self.assertNotIn("dumpsys statusbar", guest.commands[:guest.commands.index("settings put global device_provisioned 1")])
        self.assertEqual(guest.disabled, {android_provision.SETUP_WIZARD})

    def test_a_home_that_cannot_be_read_is_not_a_home_that_is_not_the_wizard(self) -> None:
        # A failed shell reads as empty: the helper asks again before it believes HOME left the wizard.
        guest = FakeGuest(home_unreadable=3)
        self.provision(guest)
        self.assertEqual(guest.unsafe_disables, 0)
        guest = FakeGuest(home_unreadable=10_000)
        with self.assertRaisesRegex(android_provision.ProvisionError, "HOME unresolved"):
            self.provision(guest)
        self.assertEqual(guest.writes(), [])

    def test_a_status_bar_dump_that_cannot_be_read_is_not_a_record(self) -> None:
        guest = FakeGuest(dump_unreadable_early=3)
        self.provision(guest)
        self.assertEqual(guest.dump_unreadable_early, 0)
        self.assertEqual(guest.unsafe_disables, 0)

    def test_an_unlocked_guest_whose_wizard_holds_its_record_is_not_delayed(self) -> None:
        guest = FakeGuest()
        clock = self.provision(guest)
        self.assertEqual(clock.now, 0)
        self.assertEqual(guest.state_reads, 1)

    def test_a_user_that_never_unlocks_is_an_error_naming_its_state(self) -> None:
        guest = FakeGuest(locked_reads=10_000)
        with self.assertRaisesRegex(android_provision.ProvisionError,
                                    r"not bound within 120 s of the helper starting: user 0 RUNNING_(LOCKED|UNLOCKING)"):
            self.provision(guest)
        self.assertEqual(guest.writes(), [])
        self.assertEqual(guest.unsafe_disables, 0)

    def test_a_wizard_that_never_holds_its_record_is_an_error_naming_what_was_seen(self) -> None:
        guest = FakeGuest(unbound_reads=10_000)
        with self.assertRaisesRegex(android_provision.ProvisionError,
                                    "not bound within 120 s of the helper starting: HOME org.lineageos.setupwizard, "
                                    "status bar record absent"):
            self.provision(guest)
        self.assertEqual(guest.writes(), [])
        self.assertEqual(guest.settings[("global", "device_provisioned")], "0")

    def test_a_dump_that_never_reads_is_an_error_naming_it(self) -> None:
        guest = FakeGuest(dump_unreadable_early=10_000)
        with self.assertRaisesRegex(android_provision.ProvisionError, "status bar record unreadable"):
            self.provision(guest)
        self.assertEqual(guest.writes(), [])

    def test_the_wait_ends_at_its_limit(self) -> None:
        clock = Clock()
        guest = FakeGuest(locked_reads=10_000)
        with self.assertRaises(android_provision.ProvisionError):
            android_provision.await_wizard_bound(guest, limit_seconds=7, sleep=clock.sleep, clock=clock)
        self.assertGreaterEqual(clock.now, 7)
        self.assertLess(clock.now, 8)
        self.assertEqual(android_provision.WIZARD_BOUND_LIMIT_SECONDS, 120)

    def test_a_shell_that_failed_reads_as_a_user_that_is_not_unlocked(self) -> None:
        guest = FakeGuest()
        answers = iter(["", "adb: device offline\n", "RUNNING_UNLOCKED\n"])
        clock = Clock()

        def shell(command: str) -> str:
            if command == "am get-started-user-state 0":
                return next(answers)
            return guest(command)

        android_provision.await_wizard_bound(shell, sleep=clock.sleep, clock=clock)
        self.assertEqual(clock.now, 1.0)

    def test_the_release_guarantees_hold_after_a_wait(self) -> None:
        # A slow boot: the wait is long, and the helper still returns with no process and no record.
        guest = FakeGuest(locked_reads=20, unbound_reads=20, restarts=1)
        self.provision(guest)
        self.assertEqual(guest.unsafe_disables, 0)
        self.assertFalse(guest.wizard_running)
        self.assertIs(android_provision.wizard_status_bar_record(guest), False)
        self.assertEqual(android_provision.wizard_processes(guest), [])
        self.assertEqual(android_provision.home_package(guest), "com.android.launcher3")

    def test_the_wait_runs_before_the_settings_and_the_disable_in_every_pass(self) -> None:
        guest = FakeGuest(locked_reads=2)
        self.provision(guest)
        order = [command for command in guest.commands
                 if command.startswith(("am get-started-user-state", "settings put", "pm disable-user"))]
        self.assertEqual(order[-3:], ["settings put global device_provisioned 1",
                                      "settings put secure user_setup_complete 1",
                                      f"pm disable-user --user 0 {android_provision.SETUP_WIZARD}"])
        self.assertTrue(all(command == "am get-started-user-state 0" for command in order[:-3]))
        self.assertEqual(len(order), 3 + 3)


if __name__ == "__main__":
    unittest.main()
