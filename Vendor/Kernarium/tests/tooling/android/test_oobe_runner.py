#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The first-boot runner's reading of the guest.

run_android_oobe.py boots a release and completes LineageOS Setup Wizard through
the pages the product shows, and checks the fresh guest's defaults. The screens,
the resumed activity, the wizard package's disabled components, the overlay list
and the Wi-Fi line below are what a release's guest printed, so the runner's
reading of them, its strict flow and its end-state checks run without a guest.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[2] / "integration/android/run_android_oobe.py"
SPEC = importlib.util.spec_from_file_location("run_android_oobe", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)

DUMPSYS_ACTIVITIES = """\
  topResumedActivity=ActivityRecord{248339971 u0 org.lineageos.setupwizard/.WelcomeActivity t8}
  ResumedActivity: ActivityRecord{248339971 u0 org.lineageos.setupwizard/.WelcomeActivity t8}
"""

DISABLED_AFTER_SETUP = """\
      overlay paths:
        /product/overlay/NavigationBarModeGestural/NavigationBarModeGesturalOverlay.apk
      disabledComponents:
        org.lineageos.setupwizard.wizardmanager.WizardManager
        org.lineageos.setupwizard.system.UpdateRecoveryActivity
        org.lineageos.setupwizard.SetupWizardActivity
        org.lineageos.setupwizard.system.DeviceSpecificActivity
      enabledComponents:
        org.lineageos.setupwizard.user.ScreenLockActivity

Queries:
"""


# `cmd overlay list android` on a fresh three-button guest, and on a release that forced gestures.
OVERLAYS_THREE_BUTTON = """\
android
[ ] com.android.internal.systemui.navbar.threebutton
[ ] com.android.internal.systemui.navbar.transparent
[ ] com.android.internal.systemui.navbar.gestural
"""
OVERLAYS_GESTURAL = OVERLAYS_THREE_BUTTON.replace("[ ] com.android.internal.systemui.navbar.gestural",
                                                  "[x] com.android.internal.systemui.navbar.gestural")
WIFI_LINE = ('mWifiInfo SSID: "VirtWifi", BSSID: c6:73:8b:52:2e:17, MAC: 02:15:b2:00:00:00, IP: /10.0.2.16, '
             'Security type: 0, Supplicant state: COMPLETED, Wi-Fi standard: unknown, RSSI: -50')


def node(text: str = "", resource_id: str = "", bounds: str = "[0,0][100,100]") -> str:
    return f'<node text="{text}" resource-id="{resource_id}" class="x" bounds="{bounds}" />'


def hierarchy(*nodes: str) -> str:
    return '<?xml version=\'1.0\' encoding=\'UTF-8\' standalone=\'yes\' ?><hierarchy rotation="0">' + "".join(nodes) + "</hierarchy>"


class ReadingTests(unittest.TestCase):
    def test_the_resumed_activity_is_the_one_the_stack_resumes(self) -> None:
        self.assertEqual(runner.resumed_activity(DUMPSYS_ACTIVITIES), runner.WELCOME_ACTIVITY)
        self.assertEqual(runner.resumed_activity("nothing resumed"), "")

    def test_disabled_components_stop_at_the_next_list(self) -> None:
        self.assertEqual(
            runner.disabled_components(DISABLED_AFTER_SETUP),
            {
                "org.lineageos.setupwizard.wizardmanager.WizardManager",
                "org.lineageos.setupwizard.system.UpdateRecoveryActivity",
                "org.lineageos.setupwizard.SetupWizardActivity",
                "org.lineageos.setupwizard.system.DeviceSpecificActivity",
            },
        )
        self.assertEqual(runner.disabled_components("      enabledComponents:\n        a.B\n"), set())

    def test_the_wizard_disables_the_components_the_runner_expects(self) -> None:
        disabled = runner.disabled_components(DISABLED_AFTER_SETUP)
        for name in runner.WIZARD_DISABLES:
            self.assertIn(f"{runner.WIZARD}.{name}", disabled)

    def test_bounds_have_a_center(self) -> None:
        self.assertEqual(runner.center("[181,1098][688,1226]"), (434, 1162))
        with self.assertRaises(runner.Failure):
            runner.center("[181,1098]")


class WizardPageTests(unittest.TestCase):
    def action(self, activity: str, *nodes: str) -> tuple:
        return runner.wizard_action(activity, runner.parse_nodes(hierarchy(*nodes)))

    def test_welcome_taps_start(self) -> None:
        self.assertEqual(
            self.action(runner.WELCOME_ACTIVITY,
                        node("Welcome to Andarium", f"{runner.WIZARD}:id/welcome_title", "[32,890][688,1077]"),
                        node("Start", f"{runner.WIZARD}:id/start", "[181,1098][688,1226]")),
            ("tap", 434, 1162))

    def test_the_language_page_takes_its_first_item(self) -> None:
        self.assertEqual(
            self.action(runner.LOCALE_ACTIVITY,
                        node("Language", f"{runner.WIZARD}:id/suc_layout_title", "[32,144][688,212]"),
                        node("English (United States)", f"{runner.WIZARD}:id/sud_items_title", "[53,255][667,286]"),
                        node("Español", f"{runner.WIZARD}:id/sud_items_title", "[53,333][667,364]")),
            ("tap", 360, 270))

    def test_the_closing_page_is_not_a_page_the_runner_answers(self) -> None:
        # The product skips it: Finish on the Theme page goes home, so there is no home icon to tap.
        with self.assertRaisesRegex(runner.Failure, "does not know"):
            self.action(runner.FINISH_ACTIVITY,
                        node("Tap the home button to go home", f"{runner.WIZARD}:id/swipe_hint_text", "[100,1000][600,1040]"),
                        node("", f"{runner.WIZARD}:id/swipe_hint_icon", "[300,900][420,1000]"))

    def test_buttons_are_found_by_text(self) -> None:
        for activity, label in ((runner.DATETIME_ACTIVITY, "Next"), (runner.THEME_ACTIVITY, "Finish")):
            with self.subTest(activity=activity):
                self.assertEqual(
                    self.action(activity, node("Title", "", "[0,0][10,10]"), node(label, "", "[32,1162][687,1237]")),
                    ("tap", 359, 1199))

    def test_the_theme_page_is_the_last_and_says_so(self) -> None:
        self.assertEqual(runner.PAGES[runner.THEME_ACTIVITY], ("text", "Finish"))
        self.assertEqual(runner.FLOW[-1], runner.THEME_ACTIVITY)
        self.assertNotIn(runner.FINISH_ACTIVITY, runner.FLOW)

    def test_the_flow_is_the_reviewed_pages_in_order(self) -> None:
        self.assertEqual([page.split("/")[-1] for page in runner.FLOW],
                         [".WelcomeActivity", ".locale.LocaleActivity", ".time.DateTimeActivity",
                          ".settings.ThemeSettingsActivity"])

    def test_the_pages_the_product_hides_are_not_known(self) -> None:
        for activity in ("com.android.settings/.network.NetworkSetupActivity",
                         f"{runner.WIZARD}/.backup.RestoreIntroActivity",
                         f"{runner.WIZARD}/.privacy.LocationSettingsActivity",
                         f"{runner.WIZARD}/.settings.LineageSettingsActivity",
                         "com.android.settings/.password.SetupChooseLockGeneric",
                         f"{runner.WIZARD}/.settings.NavigationSettingsActivity",
                         runner.FINISH_ACTIVITY):
            with self.subTest(activity=activity), self.assertRaisesRegex(runner.Failure, "does not know"):
                self.action(activity, node("Next"))

    def test_a_page_without_its_button_is_a_failure(self) -> None:
        with self.assertRaises(runner.Failure):
            self.action(runner.DATETIME_ACTIVITY, node("Title"))

    def test_a_page_the_runner_does_not_know_is_a_failure(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "does not know"):
            self.action(f"{runner.WIZARD}/.NewPageActivity", node("Next"))

    def test_a_dump_that_is_not_xml_is_a_failure(self) -> None:
        with self.assertRaises(runner.Failure):
            runner.parse_nodes("ERROR: null root node returned by UiTestAutomationBridge.")


class StrictFlowTests(unittest.TestCase):
    def test_the_page_on_screen_or_the_next_one_is_the_flow(self) -> None:
        for position in range(len(runner.FLOW)):
            self.assertEqual(runner.next_page(position, runner.FLOW[position]), position)
            if position + 1 < len(runner.FLOW):
                self.assertEqual(runner.next_page(position, runner.FLOW[position + 1]), position + 1)

    def test_a_page_out_of_order_is_a_failure(self) -> None:
        for position, activity in ((0, runner.DATETIME_ACTIVITY), (1, runner.THEME_ACTIVITY),
                                   (2, runner.LOCALE_ACTIVITY), (3, runner.WELCOME_ACTIVITY),
                                   (3, runner.FINISH_ACTIVITY)):
            with self.subTest(position=position), self.assertRaisesRegex(runner.Failure, "expected"):
                runner.next_page(position, activity)

    def test_a_hidden_page_is_a_failure_wherever_it_shows(self) -> None:
        for position in range(len(runner.FLOW)):
            with self.subTest(position=position), self.assertRaises(runner.Failure):
                runner.next_page(position, f"{runner.WIZARD}/.privacy.LocationSettingsActivity")

    def test_the_failure_names_the_page_and_what_was_expected(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "LocationSettingsActivity.*DateTimeActivity.*ThemeSettingsActivity"):
            runner.next_page(2, f"{runner.WIZARD}/.privacy.LocationSettingsActivity")

    def test_the_last_page_expects_only_itself(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "FinishActivity.*expected .*ThemeSettingsActivity"):
            runner.next_page(len(runner.FLOW) - 1, runner.FINISH_ACTIVITY)


class FakeWizard:
    """A guest that shows the given activities in turn, one per look, and answers taps."""

    def __init__(self, activities: list[str]) -> None:
        self.activities = list(activities)
        self.taps: list[str] = []

    def resumed(self) -> str:
        return self.activities.pop(0) if len(self.activities) > 1 else self.activities[0]

    def ui_nodes(self) -> list[dict[str, str]]:
        return runner.parse_nodes(hierarchy(
            node("Start", f"{runner.WIZARD}:id/start", "[0,0][100,100]"),
            node("English", f"{runner.WIZARD}:id/sud_items_title", "[0,0][100,100]"),
            node("Next", "", "[0,0][100,100]"),
            node("Finish", "", "[0,0][100,100]")))

    def shell(self, command: str) -> str:
        self.taps.append(command)
        return ""


class CompleteWizardTests(unittest.TestCase):
    LAUNCHER = f"{runner.LAUNCHER_PACKAGE}/.uioverrides.QuickstepLauncher"

    def run_flow(self, activities: list[str]) -> FakeWizard:
        device = FakeWizard(activities)
        with mock.patch.object(runner.time, "sleep"):
            runner.complete_wizard(device)
        return device

    def test_the_reviewed_flow_completes(self) -> None:
        device = self.run_flow([page for page in runner.FLOW for _ in (0, 1)] + [self.LAUNCHER])
        # One tap per look at a page, two looks per page: the page was still up after the first tap.
        self.assertEqual(len(device.taps), 2 * len(runner.FLOW))

    def test_the_flow_may_be_slow_to_change_page(self) -> None:
        device = self.run_flow([runner.WELCOME_ACTIVITY] * 3 + [""] + list(runner.FLOW[1:]) + [self.LAUNCHER])
        self.assertGreaterEqual(len(device.taps), len(runner.FLOW))

    def test_a_hidden_page_is_a_failure(self) -> None:
        for hidden in ("com.android.settings/.network.NetworkSetupActivity",
                       f"{runner.WIZARD}/.privacy.LocationSettingsActivity",
                       "com.android.settings/.password.SetupChooseLockGeneric"):
            with self.subTest(hidden=hidden), self.assertRaisesRegex(runner.Failure, "the product does not"):
                self.run_flow([runner.WELCOME_ACTIVITY, runner.LOCALE_ACTIVITY, hidden])

    def test_a_reviewed_page_out_of_order_is_a_failure(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "expected"):
            self.run_flow([runner.WELCOME_ACTIVITY, runner.LOCALE_ACTIVITY, runner.THEME_ACTIVITY])

    def test_a_reviewed_page_that_never_shows_is_a_failure(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "before the whole flow"):
            self.run_flow([runner.WELCOME_ACTIVITY, runner.LOCALE_ACTIVITY, self.LAUNCHER])

    def test_the_launcher_before_any_page_is_a_failure(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "before the whole flow"):
            self.run_flow([self.LAUNCHER])

    def test_a_wizard_that_does_not_finish_is_a_failure(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "did not finish"):
            self.run_flow([runner.WELCOME_ACTIVITY])

    def test_finish_on_the_theme_page_goes_to_the_launcher_with_no_closing_page(self) -> None:
        device = self.run_flow(list(runner.FLOW) + [self.LAUNCHER])
        # One tap per page, and none after the Theme page's Finish.
        self.assertEqual(len(device.taps), len(runner.FLOW))

    def test_the_closing_page_after_theme_is_a_failure(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "closing page, which the product skips: Finish on the "
                                                    r".*ThemeSettingsActivity page goes straight to the home screen"):
            self.run_flow(list(runner.FLOW) + [runner.FINISH_ACTIVITY, self.LAUNCHER])

    def test_the_closing_page_is_a_failure_even_when_it_is_the_only_page_seen(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "closing page"):
            self.run_flow([runner.FINISH_ACTIVITY])


class DefaultsTests(unittest.TestCase):
    def test_overlay_states_are_read_from_the_listing(self) -> None:
        self.assertEqual(
            runner.overlay_states(OVERLAYS_THREE_BUTTON),
            {runner.NAVBAR_3BUTTON: " ", "com.android.internal.systemui.navbar.transparent": " ",
             runner.NAVBAR_GESTURAL: " "})
        self.assertEqual(runner.overlay_states(OVERLAYS_GESTURAL)[runner.NAVBAR_GESTURAL], "x")
        self.assertEqual(runner.overlay_states("--- some.package\n")["some.package"], "-")
        self.assertEqual(runner.overlay_states(""), {})

    def test_the_wifi_network_is_read_from_the_dump(self) -> None:
        self.assertEqual(runner.wifi_network(WIFI_LINE), ("VirtWifi", "COMPLETED"))
        self.assertEqual(runner.wifi_network(WIFI_LINE.replace("COMPLETED", "SCANNING")), ("VirtWifi", "SCANNING"))
        self.assertIsNone(runner.wifi_network("Wifi is disabled"))


class FakeDefaults:
    """The guest's answers to the end-state checks, each of which a case can change."""

    def __init__(self, **changes: str) -> None:
        self.answers = {
            "secure navigation_mode": "0",
            "secure location_mode": "3",
            "global assisted_gps_enabled": "0",
            "overlays": OVERLAYS_THREE_BUTTON,
            "lock": "true",
            "wifi": WIFI_LINE,
        }
        self.answers.update(changes)

    def setting(self, namespace: str, name: str) -> str:
        return self.answers[f"{namespace} {name}"]

    def shell(self, command: str) -> str:
        if command == "cmd overlay list android":
            return self.answers["overlays"]
        if command == "locksettings get-disabled":
            return self.answers["lock"]
        if command.startswith("dumpsys wifi"):
            return self.answers["wifi"]
        raise AssertionError(command)


class CheckDefaultsTests(unittest.TestCase):
    BEFORE = {"secure location_mode": "3", "global assisted_gps_enabled": "0"}

    def check(self, before: dict[str, str] | None = BEFORE, **changes: str) -> None:
        def immediately(description: str, probe, seconds: int, guest=None) -> None:
            if not probe():
                raise runner.Failure(f"{description}: not within {seconds} s")

        with mock.patch.object(runner, "wait_for", immediately):
            runner.check_defaults(FakeDefaults(**changes), before)

    def test_a_fresh_guest_has_the_platform_defaults(self) -> None:
        self.check()
        self.check(before=None)

    def test_navigation_is_three_buttons(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "navigation_mode is '2', expected 0"):
            self.check(**{"secure navigation_mode": "2"})
        with self.assertRaisesRegex(runner.Failure, "navigation_mode is 'null'"):
            self.check(**{"secure navigation_mode": "null"})

    def test_the_gestural_overlay_is_off_and_both_overlays_are_installed(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "gestural navigation overlay is enabled"):
            self.check(overlays=OVERLAYS_GESTURAL)
        with self.assertRaisesRegex(runner.Failure, f"{runner.NAVBAR_3BUTTON} is not installed"):
            self.check(overlays=OVERLAYS_THREE_BUTTON.replace(runner.NAVBAR_3BUTTON, "x.other"))
        with self.assertRaisesRegex(runner.Failure, f"{runner.NAVBAR_GESTURAL} is not installed"):
            self.check(overlays=OVERLAYS_THREE_BUTTON.replace(runner.NAVBAR_GESTURAL, "x.other"))

    def test_there_is_no_screen_lock(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "expected true"):
            self.check(lock="false")

    def test_location_stays_on_as_it_was(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "location is not on"):
            self.check(**{"secure location_mode": "0"})
        with self.assertRaisesRegex(runner.Failure, "changed the location settings"):
            self.check(**{"global assisted_gps_enabled": "1"})
        # A guest with no before-state (the helper's) is held to location on only.
        self.check(before=None, **{"global assisted_gps_enabled": "1"})

    def test_wi_fi_is_connected_to_the_virtual_network(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "Wi-Fi connected to VirtWifi"):
            self.check(wifi="Wifi is disabled")
        with self.assertRaisesRegex(runner.Failure, "Wi-Fi connected to VirtWifi"):
            self.check(wifi=WIFI_LINE.replace("VirtWifi", "Other"))
        with self.assertRaisesRegex(runner.Failure, "Wi-Fi connected to VirtWifi"):
            self.check(wifi=WIFI_LINE.replace("COMPLETED", "ASSOCIATING"))


class WizardReleasedTests(unittest.TestCase):
    class Guest:
        def __init__(self, record, processes) -> None:
            self.record, self.processes = record, processes

        def wizard_status_bar_lock(self):
            return self.record

        def wizard_processes(self):
            return self.processes

    def check(self, guest, *, by_wizard: bool) -> None:
        def immediately(description: str, probe, seconds: int, guest=None) -> None:
            if not probe():
                raise runner.Failure(f"{description}: not within {seconds} s")

        with mock.patch.object(runner, "wait_for", immediately):
            runner.check_wizard_released(guest, by_wizard=by_wizard)

    def test_a_wizard_that_holds_the_status_bar_fails_both_guests(self) -> None:
        for by_wizard in (True, False):
            with self.subTest(by_wizard=by_wizard), self.assertRaisesRegex(runner.Failure, "no status bar lock"):
                self.check(self.Guest(True, []), by_wizard=by_wizard)
            with self.subTest(by_wizard=by_wizard, unreadable=True), self.assertRaises(runner.Failure):
                self.check(self.Guest(None, []), by_wizard=by_wizard)

    def test_the_helper_leaves_no_wizard_process_and_the_wizard_may_linger(self) -> None:
        self.check(self.Guest(False, ["1445"]), by_wizard=True)
        self.check(self.Guest(False, []), by_wizard=False)
        with self.assertRaisesRegex(runner.Failure, "still runs after the helper"):
            self.check(self.Guest(False, ["1445"]), by_wizard=False)


# `dumpsys statusbar` of a guest whose wizard finished (r17 and r16 end the same way): the wizard's process lives on, cached, with
# a record of two zero flag words beside SystemUI's own.
STATUS_BAR_AFTER_WIZARD = """\
displayId=0
    mDisabled1=0x0
    mDisabled2=0x0
  mDisableRecords.size=3
    [0] userId=0 what1=0x00000000 what2=0x00000000 pkg=com.android.systemui token=android.os.BinderProxy@e47a561
    [1] userId=0 what1=0x00000000 what2=0x00000000 pkg=com.android.systemui token=android.os.BinderProxy@2eb2886
    [2] userId=0 what1=0x00000000 what2=0x00000000 pkg=org.lineageos.setupwizard token=android.os.BinderProxy@21d4547
  mCurrentUserId=0
"""


class WizardProcessesTests(unittest.TestCase):
    def test_a_missing_process_is_an_empty_answer_not_a_shell_error(self) -> None:
        commands: list[str] = []

        class Recording(runner.Device):
            def shell(self, command: str, timeout: int = 30) -> str:
                commands.append(command)
                return self.answer

        device = Recording("adb", "serial", {})
        device.answer = ""
        self.assertEqual(device.wizard_processes(), [])
        device.answer = "1445 1450"
        self.assertEqual(device.wizard_processes(), ["1445", "1450"])
        # `pidof` exits 1 for no process, which the strict shell would raise.
        self.assertTrue(all(command.endswith("; true") for command in commands), commands)


class StatusBarLockTests(unittest.TestCase):
    def test_a_finished_wizards_zero_record_is_not_a_lock(self) -> None:
        self.assertIs(runner.wizard_holds_status_bar(STATUS_BAR_AFTER_WIZARD), False)

    def test_a_record_with_a_flag_is_a_lock(self) -> None:
        for first, second in (("0x00010000", "0x00000000"), ("0x00000000", "0x00000002"), ("0x7fffffff", "0x1")):
            with self.subTest(first=first, second=second):
                held = STATUS_BAR_AFTER_WIZARD.replace(
                    "what1=0x00000000 what2=0x00000000 pkg=org.lineageos.setupwizard",
                    f"what1={first} what2={second} pkg=org.lineageos.setupwizard")
                self.assertIs(runner.wizard_holds_status_bar(held), True)

    def test_only_the_wizards_own_record_counts(self) -> None:
        other = STATUS_BAR_AFTER_WIZARD.replace("what1=0x00000000 what2=0x00000000 pkg=com.android.systemui",
                                                "what1=0x00010000 what2=0x00000000 pkg=com.android.systemui", 1)
        self.assertIs(runner.wizard_holds_status_bar(other), False)
        prefix = STATUS_BAR_AFTER_WIZARD.replace("pkg=org.lineageos.setupwizard ", "pkg=org.lineageos.setupwizard.other ") \
            .replace("what1=0x00000000 what2=0x00000000 pkg=org.lineageos.setupwizard.other",
                     "what1=0x00010000 what2=0x00000000 pkg=org.lineageos.setupwizard.other")
        self.assertIs(runner.wizard_holds_status_bar(prefix), False)

    def test_a_dump_without_the_record_list_is_unreadable(self) -> None:
        self.assertIsNone(runner.wizard_holds_status_bar("Can't find service: statusbar"))
        self.assertIs(runner.wizard_holds_status_bar("mDisableRecords.size=0\n"), False)


class WizardDisablesWaitTests(unittest.TestCase):
    """The wizard disables its flow manager a moment after its Finish page: the runner waits for it."""

    def run_check(self, answers: list[set[str]]):
        guest = mock.Mock()
        guest.setting.return_value = "1"
        guest.home_package.return_value = runner.LAUNCHER_PACKAGE
        guest.resumed.return_value = f"{runner.LAUNCHER_PACKAGE}/.Launcher"
        guest.shell.return_value = "package:com.other"
        polls = []

        def polling(description: str, probe, seconds: int, guest=None) -> None:
            for _ in range(len(answers)):
                polls.append(description)
                if probe():
                    return
            raise runner.Failure(f"{description}: not within {seconds} s")

        sequence = iter(answers + [answers[-1]] * 4)
        with mock.patch.object(runner, "wait_for", polling), \
                mock.patch.object(runner, "disabled_components", side_effect=lambda _dump: next(sequence)), \
                mock.patch.object(runner, "check_wizard_released"), \
                mock.patch.object(runner, "check_wizard_did_not_crash"), \
                mock.patch.object(runner, "check_defaults"), \
                mock.patch.object(runner, "check_launcher"):
            runner.check_provisioned(guest, by_wizard=True)
        return polls

    def test_a_manager_disabled_a_moment_later_passes(self) -> None:
        only_activity = {f"{runner.WIZARD}.SetupWizardActivity"}
        both = {f"{runner.WIZARD}.{name}" for name in runner.WIZARD_DISABLES}
        polls = self.run_check([only_activity, both])
        self.assertIn("the wizard to disable its HOME activity and its flow manager", polls)

    def test_a_manager_that_stays_enabled_fails_and_names_it(self) -> None:
        only_activity = {f"{runner.WIZARD}.SetupWizardActivity"}
        with self.assertRaisesRegex(runner.Failure, r"did not disable \['wizardmanager\.WizardManager'\] within 30 s"):
            self.run_check([only_activity, only_activity])


# `logcat -b crash -d` after the wizard's process died while the package was disabled under it (r16, trimmed).
WIZARD_CRASH = """\
--------- beginning of crash
11-07 09:40:55.611  1777  1777 E AndroidRuntime: FATAL EXCEPTION: main
11-07 09:40:55.611  1777  1777 E AndroidRuntime: Process: org.lineageos.setupwizard, PID: 1777
11-07 09:40:55.611  1777  1777 E AndroidRuntime: java.lang.RuntimeException: Unable to get provider androidx.startup.InitializationProvider
11-07 09:40:55.611  1777  1777 E AndroidRuntime: Caused by: android.content.pm.PackageManager$NameNotFoundException: ComponentInfo{org.lineageos.setupwizard/androidx.startup.InitializationProvider}
"""
OTHER_CRASH = """\
--------- beginning of crash
11-07 09:50:12.105  2210  2210 E AndroidRuntime: FATAL EXCEPTION: main
11-07 09:50:12.105  2210  2210 E AndroidRuntime: Process: com.android.documentsui, PID: 2210
11-07 09:50:12.105  2210  2210 E AndroidRuntime: Process: org.lineageos.setupwizard.other, PID: 2211
"""


FINISH_LOG = """\
--------- beginning of main
11-08 04:10:22.310 I/FinishActivity( 1450): onCreate: finishState=NONE
"""


class WizardSkippedFinishTests(unittest.TestCase):
    class Guest:
        def __init__(self, log: str) -> None:
            self.log = log
            self.commands: list[str] = []

        def shell(self, command: str, timeout: int = 30) -> str:
            self.commands.append(command)
            return self.log

    def test_the_closing_pages_creation_is_its_log_tag(self) -> None:
        self.assertEqual(runner.finish_activity_lines(FINISH_LOG),
                         ["11-08 04:10:22.310 I/FinishActivity( 1450): onCreate: finishState=NONE"])
        self.assertEqual(runner.finish_activity_lines("--------- beginning of main\n"), [])

    def test_other_tags_that_name_the_page_are_not_it(self) -> None:
        self.assertEqual(runner.finish_activity_lines(
            "11-08 04:10:22.310 I/ActivityTaskManager( 900): START u0 {cmp=org.lineageos.setupwizard/.FinishActivity}\n"), [])

    def test_a_wizard_that_never_started_the_page_passes(self) -> None:
        guest = self.Guest("--------- beginning of main\n")
        runner.check_wizard_skipped_finish(guest)
        self.assertEqual(len(guest.commands), 1)
        self.assertIn("-b main", guest.commands[0])
        self.assertIn("FinishActivity:V", guest.commands[0])

    def test_a_wizard_that_started_the_page_fails_and_shows_the_line(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "started its closing page, which the product skips: .*onCreate: finishState=NONE"):
            runner.check_wizard_skipped_finish(self.Guest(FINISH_LOG))


class WizardCrashTests(unittest.TestCase):
    class Guest:
        def __init__(self, crash_log: str) -> None:
            self.crash_log, self.commands = crash_log, []

        def shell(self, command: str, timeout: int = 30) -> str:
            self.commands.append(command)
            return self.crash_log

    def test_the_wizards_process_line_is_a_crash(self) -> None:
        self.assertEqual(
            runner.wizard_fatals(WIZARD_CRASH),
            ["11-07 09:40:55.611  1777  1777 E AndroidRuntime: Process: org.lineageos.setupwizard, PID: 1777"])

    def test_other_processes_and_an_empty_buffer_are_not(self) -> None:
        self.assertEqual(runner.wizard_fatals(OTHER_CRASH), [])
        self.assertEqual(runner.wizard_fatals("--------- beginning of crash\n"), [])
        self.assertEqual(runner.wizard_fatals(""), [])
        # A package that only shares the wizard's prefix is not the wizard.
        self.assertEqual(runner.wizard_fatals("E AndroidRuntime: Process: org.lineageos.setupwizard.remote, PID: 9"), [])

    def test_a_wizard_crash_fails_the_check_and_names_the_process(self) -> None:
        guest = self.Guest(WIZARD_CRASH)
        with self.assertRaisesRegex(runner.Failure, r"the setup wizard crashed: .*Process: org\.lineageos\.setupwizard, PID: 1777"):
            runner.check_wizard_did_not_crash(guest)
        self.assertEqual(guest.commands, ["logcat -b crash -d"])

    def test_a_clean_crash_buffer_passes(self) -> None:
        runner.check_wizard_did_not_crash(self.Guest(OTHER_CRASH))

    def test_both_guests_are_checked_after_the_wizard_is_released(self) -> None:
        calls: list[str] = []
        guest = mock.Mock()
        guest.setting.return_value = "1"
        guest.home_package.return_value = runner.LAUNCHER_PACKAGE
        guest.resumed.return_value = f"{runner.LAUNCHER_PACKAGE}/.Launcher"
        for by_wizard in (True, False):
            calls.clear()
            with self.subTest(by_wizard=by_wizard), \
                    mock.patch.object(runner, "wait_for"), \
                    mock.patch.object(runner, "disabled_components",
                                      return_value={f"{runner.WIZARD}.{name}" for name in runner.WIZARD_DISABLES}), \
                    mock.patch.object(runner, "check_wizard_released", side_effect=lambda *a, **k: calls.append("released")), \
                    mock.patch.object(runner, "check_wizard_did_not_crash", side_effect=lambda *a, **k: calls.append("crash")), \
                    mock.patch.object(runner, "check_wizard_skipped_finish", side_effect=lambda *a, **k: calls.append("finish")), \
                    mock.patch.object(runner, "check_defaults", side_effect=lambda *a, **k: calls.append("defaults")), \
                    mock.patch.object(runner, "check_launcher", side_effect=lambda *a, **k: calls.append("launcher")):
                # `pm list packages -d`: the wizard ends enabled, and the helper disables its package.
                guest.shell.return_value = "package:com.other" if by_wizard else f"package:{runner.WIZARD}"
                runner.check_provisioned(guest, by_wizard=by_wizard)
            self.assertEqual(calls, ["released", "crash", "finish", "defaults", "launcher"])


# The launcher's `Data Model` as `dumpsys activity com.android.launcher3/.uioverrides.QuickstepLauncher`
# prints it: protobuf text for the container, which leaves a zero field out.
DUMP_HEAD = """\
    ContextTracker:
    \tmCurrentContext=com.android.launcher3.uioverrides.QuickstepLauncher@c03d938
    Data Model:
     ---- items id map
    \tItemInfo(id=-103 type=APP container=# com.android.launcher3.logger.LauncherAtom$ContainerInfo@19f79dd
predicted_hotseat_container {
  index: -1
} targetComponent=null ResolvedTargetInfo=ResolvedTargetInfo(targetActivityComponentName=null, componentName=null, user=UserHandle{0}) screen=-1 cell(-1,-1) span(1,1) title=null, items: [])
    \tItemInfo(id=-102 type=APP container=# com.android.launcher3.logger.LauncherAtom$ContainerInfo@1a1bf6a
prediction_container {
} targetComponent=null ResolvedTargetInfo=ResolvedTargetInfo(targetActivityComponentName=null, componentName=null, user=UserHandle{0}) screen=-1 cell(-1,-1) span(1,1) title=null, items: [])
"""
DUMP_TAIL = """\
     ---- extra items
    Default MSDL player implementation.
"""


def dump_item(item_id: int, kind: str, package: str, class_name: str, *, page: int = 0, x: int = 0, y: int = 0,
              type_name: str = "APP", index: int | None = None) -> str:
    fields = []
    if kind == "hotseat":
        fields = [f"index: {item_id}"] if index is None else [f"index: {index}"]
    else:
        fields += [f"grid_x: {x}"] if x else []
        fields += [f"grid_y: {y}"] if y else []
        fields += [f"page_index: {page}"] if page else []
    body = "".join(f"  {field}\n" for field in fields)
    return (f"    \tItemInfo(id={item_id} type={type_name} container=# com.android.launcher3.logger.LauncherAtom"
            f"$ContainerInfo@21c970c\n{kind} {{\n{body}}} targetComponent=ComponentInfo{{{package}/{class_name}}} "
            f"ResolvedTargetInfo=ResolvedTargetInfo(componentName=ComponentInfo{{{package}/{class_name}}}) "
            f"screen={page} cell({x},{y}) span(1,1) title=null, items: [])\n")


def workspace_tree(*, pages: int = 1, widgets: int = 0) -> str:
    """The launcher's view hierarchy as the activity dump prints it: a page is a CellLayout below the Workspace."""
    lines = ["                com.android.launcher3.Workspace{b549cbe V.ED..... ........ 0,0-720,1280 #7f0903fa app:id/workspace}"]
    for page in range(pages):
        lines.append(f"                  com.android.launcher3.CellLayout{{8b2995{page} V.ED..... ........ 0,37-720,1140}}")
        lines.append("                    com.android.launcher3.ShortcutAndWidgetContainer{d47add7 V.E...... ........ 13,14-707,1089}")
        if page == pages - 1:
            for widget in range(widgets):
                lines.append("                      com.android.launcher3.widget.LauncherAppWidgetHostView{ed1f61%d VFE....L. "
                             "R.....ID 0,0-695,215 #%d}" % (widget, widget + 2))
                lines.append("                        android.widget.LinearLayout{56f4e2 VFE...C.. ......ID 0,0-695,215}")
            lines.append("                      com.android.launcher3.views.DoubleShadowBubbleTextView{1e5369 VFED..CL. ......ID 139,860-278,1075 #3}")
    lines.append("                com.android.launcher3.Hotseat{3bcd11f V.ED..... ........ 0,1158-720,1280 #7f0901c2 app:id/hotseat}")
    return "\n".join(lines) + "\n"


def home_dump(*, pages: int = 1, widgets: int = 0, dock_ranks: tuple[int, int] = (1, 3)) -> str:
    """The home screen of release r18: Files and Settings in the dock, an empty workspace."""
    files, settings = sorted(runner.DOCK_APPS, key=lambda app: app[0] != "com.android.documentsui")
    items = [dump_item(2, "hotseat", *files, index=dock_ranks[0], x=dock_ranks[0]),
             dump_item(3, "hotseat", *settings, index=dock_ranks[1], x=dock_ranks[1])]
    return workspace_tree(pages=pages, widgets=widgets) + DUMP_HEAD + "".join(items) + DUMP_TAIL


class LauncherWorkspaceTests(unittest.TestCase):
    def test_files_and_settings_in_the_dock_are_the_default_layout(self) -> None:
        items = runner.launcher_items(home_dump())
        self.assertEqual(len(items), 2)
        self.assertEqual({item["component"] for item in items}, runner.DOCK_APPS)
        self.assertEqual({item["container"] for item in items}, {"hotseat"})
        self.assertEqual(sorted(item["rank"] for item in items), [1, 3])
        self.assertEqual(runner.launcher_problems(items), [])

    def test_the_dock_ranks_are_not_the_runners_business(self) -> None:
        # The grid decides the slots (the verifier pins them per grid); the runner reads which items are in the dock.
        for ranks in ((0, 1), (1, 3), (2, 3), (1, 2)):
            with self.subTest(ranks=ranks):
                self.assertEqual(runner.launcher_problems(runner.launcher_items(home_dump(dock_ranks=ranks))), [])

    def test_the_prediction_items_are_not_items(self) -> None:
        items = runner.launcher_items(DUMP_HEAD + DUMP_TAIL)
        self.assertEqual(items, [])

    def test_a_dump_without_a_data_model_has_no_items(self) -> None:
        self.assertEqual(runner.launcher_items("nothing here"), [])
        self.assertEqual(len(runner.launcher_problems([])), 3)

    def test_release_r17s_six_applications_on_the_workspace_are_a_failure(self) -> None:
        old = DUMP_HEAD
        for index, (package, class_name) in enumerate(sorted(runner.DOCK_APPS) + [
                ("org.lineageos.etar", "com.android.calendar.AllInOneActivity"),
                ("org.lineageos.glimpse", "org.lineageos.glimpse.MainActivity"),
                ("org.lineageos.twelve", "org.lineageos.twelve.MainActivity"),
                ("com.android.calculator2", "com.android.calculator2.Calculator")]):
            old += dump_item(index + 2, "workspace", package, class_name, x=index % 5, y=index // 5)
        problems = runner.launcher_problems(runner.launcher_items(old + DUMP_TAIL))
        text = "; ".join(problems)
        self.assertIn("6 items, expected 2", text)
        self.assertIn("com.android.settings/com.android.settings.Settings is on the workspace page 0, which stays empty", text)
        self.assertIn("org.lineageos.etar/com.android.calendar.AllInOneActivity (APP) is not Files or Settings", text)
        self.assertEqual(text.count("is on the workspace page"), 6)

    def test_release_r16s_widget_and_second_page_are_a_failure(self) -> None:
        old = (DUMP_HEAD
               + dump_item(2, "workspace", "org.lineageos.twelve", "org.lineageos.twelve.ui.widgets.NowPlayingAppWidgetProvider",
                           page=1, type_name="WIDGET")
               + dump_item(3, "workspace", "org.lineageos.glimpse", "org.lineageos.glimpse.MainActivity", page=1, x=1, y=4)
               + DUMP_TAIL)
        text = "; ".join(runner.launcher_problems(runner.launcher_items(old)))
        self.assertIn("(WIDGET) is not Files or Settings", text)
        self.assertIn("org.lineageos.glimpse/org.lineageos.glimpse.MainActivity is on the workspace page 1", text)
        self.assertIn("com.android.documentsui/com.android.documentsui.LauncherActivity is missing from the dock", text)
        self.assertIn("com.android.settings/com.android.settings.Settings is missing from the dock", text)

    def test_files_on_the_workspace_is_a_failure(self) -> None:
        dump = home_dump().replace(
            "    \tItemInfo(id=2 type=APP container=# com.android.launcher3.logger.LauncherAtom$ContainerInfo@21c970c\nhotseat {",
            "    \tItemInfo(id=2 type=APP container=# com.android.launcher3.logger.LauncherAtom$ContainerInfo@21c970c\nworkspace {",
            1)
        problems = runner.launcher_problems(runner.launcher_items(dump))
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("is on the workspace page 0, which stays empty", problems[0])

    def test_a_third_item_is_a_failure(self) -> None:
        dump = home_dump().replace(DUMP_TAIL, dump_item(9, "hotseat", "org.lineageos.etar", "com.android.calendar.AllInOneActivity",
                                                      index=0) + DUMP_TAIL)
        problems = runner.launcher_problems(runner.launcher_items(dump))
        self.assertTrue(any("3 items, expected 2" in problem for problem in problems), problems)
        self.assertTrue(any("com.android.calendar.AllInOneActivity" in problem for problem in problems), problems)

    def test_a_missing_dock_application_is_named(self) -> None:
        dump = home_dump()
        start = dump.index("    \tItemInfo(id=3 ")
        end = dump.index(DUMP_TAIL)
        problems = runner.launcher_problems(runner.launcher_items(dump[:start] + dump[end:]))
        self.assertEqual(len(problems), 2, problems)
        self.assertIn("1 items, expected 2", problems[0])
        self.assertEqual(problems[1], "com.android.settings/com.android.settings.Settings is missing from the dock")


DROPS = """\
--------- beginning of main
11-07 03:59:00.310 D/ModelDbController( 1080): loading default workspace
11-07 03:59:00.496 W/DefaultLayoutParser( 1080): No preference or single system activity found for Intent { act=android.intent.action.DIAL }
11-07 03:59:00.878 D/DefaultLayoutParser( 1080): Can't find widget provider: com.google.android.googlequicksearchbox.SearchWidgetProvider
11-07 03:59:01.110 E/AutoInstalls( 1080): Favorite not found: com.google.android.gm/com.google.android.gm.ConversationListActivityGmail
"""
CLEAN = """\
--------- beginning of main
11-07 03:40:00.100 W/DefaultLayoutParser( 900): left over from an older load
11-07 03:59:00.310 D/ModelDbController( 1080): loading default workspace
11-07 03:59:00.500 D/DefaultLayoutParser( 1080): parsed favorites
"""


class LauncherLayoutLogTests(unittest.TestCase):
    def test_the_parsers_complaints_after_the_layout_load_are_the_drops(self) -> None:
        drops = runner.launcher_layout_drops(DROPS)
        self.assertEqual(len(drops), 3)
        self.assertTrue(drops[0].startswith("DefaultLayoutParser: No preference"))
        self.assertIn("Can't find widget provider", drops[1])
        self.assertIn("AutoInstalls: Favorite not found", drops[2])

    def test_a_clean_load_has_no_drops_and_older_lines_do_not_count(self) -> None:
        self.assertEqual(runner.launcher_layout_drops(CLEAN), [])

    def test_a_log_without_the_load_has_no_answer(self) -> None:
        # None is not "no drops": the caller refuses it.
        self.assertIsNone(runner.launcher_layout_drops("11-07 03:59:01.110 E/AutoInstalls( 1080): Favorite not found: x/y\n"))

    def test_the_last_load_is_the_one_that_counts(self) -> None:
        self.assertEqual(runner.launcher_layout_drops(DROPS + CLEAN), [])


class WorkspaceViewTests(unittest.TestCase):
    def test_one_page_and_no_widget_is_the_home_screen(self) -> None:
        self.assertEqual(runner.workspace_view(home_dump()), (1, []))
        self.assertEqual(runner.workspace_view_problems(runner.workspace_view(home_dump())), [])

    def test_a_second_page_is_a_problem(self) -> None:
        # Release r16 showed an empty first page and the items on a second one.
        self.assertEqual(runner.workspace_view(home_dump(pages=2)), (2, []))
        self.assertEqual(runner.workspace_view_problems(runner.workspace_view(home_dump(pages=2))),
                         ["2 workspace pages, expected exactly one"])

    def test_no_page_is_a_problem(self) -> None:
        self.assertEqual(runner.workspace_view_problems(runner.workspace_view(home_dump(pages=0))),
                         ["0 workspace pages, expected exactly one"])

    def test_a_widget_is_a_problem_wherever_it_sits(self) -> None:
        # The music widget of release r16 was a LauncherAppWidgetHostView on the second page.
        problems = runner.workspace_view_problems(runner.workspace_view(home_dump(widgets=1)))
        self.assertEqual(len(problems), 1)
        self.assertIn("1 app widget view(s)", problems[0])
        self.assertIn("com.android.launcher3.widget.LauncherAppWidgetHostView", problems[0])
        both = runner.workspace_view_problems(runner.workspace_view(home_dump(pages=2, widgets=2)))
        self.assertEqual(len(both), 2)

    def test_a_dump_without_the_workspace_view_cannot_pass(self) -> None:
        for dump in ("", DUMP_HEAD + DUMP_TAIL):
            self.assertIsNone(runner.workspace_view(dump))
            self.assertIn("no workspace view", runner.workspace_view_problems(runner.workspace_view(dump))[0])

    def test_the_dock_and_other_views_are_not_pages(self) -> None:
        dump = home_dump() + "                  com.android.launcher3.CellLayout{99 V.E 0,0-1,1}\n"
        # Outside the workspace's subtree (the same depth as the Hotseat, not below the Workspace).
        self.assertEqual(runner.workspace_view(dump)[0], 1)


# The app drawer as uiautomator dumps it on a guest of release r17 (opened with the launcher's ALL_APPS intent):
# the search container and one node per application, whose id is the launcher's `icon`.
def drawer_hierarchy(*labels: str, search: bool = True) -> str:
    nodes = [node("Search apps", runner.DRAWER_SEARCH_ID, "[23,186][1155,343]")] if search else []
    for index, label in enumerate(labels):
        nodes.append(node(label, runner.DRAWER_ICON_ID, f"[{24 + 226 * (index % 5)},373][{250 + 226 * (index % 5)},733]"))
    nodes.append(node("", "com.android.launcher3:id/accessibility_action_view", "[0,84][1179,2256]"))
    return hierarchy(*nodes)


def workspace_hierarchy() -> str:
    # The workspace's and the dock's icons carry no resource id, so they are not the drawer's.
    return hierarchy(node("Files", "", "[25,2000][251,2200]"), node("Settings", "", "[701,2000][927,2200]"))


SIX = ("Calculator", "Calendar", "Files", "Gallery", "Music", "Settings")


class FakeLauncher:
    """A guest whose launcher shows a home screen, and its app drawer once the ALL_APPS intent has been sent."""

    def __init__(self, dump: str, log: str, drawer: tuple[str, ...] | None = SIX) -> None:
        self.dump = dump
        self.log = log
        self.dumps = 0
        self.drawer = drawer
        self.commands: list[str] = []
        self.opened = False

    def resumed(self) -> str:
        return f"{runner.LAUNCHER_PACKAGE}/.uioverrides.QuickstepLauncher"

    def ui_nodes(self) -> list[dict[str, str]]:
        if self.opened and self.drawer is not None:
            return runner.parse_nodes(drawer_hierarchy(*self.drawer))
        return runner.parse_nodes(workspace_hierarchy())

    def shell(self, command: str, timeout: int = 30) -> str:
        self.commands.append(command)
        if command.startswith("dumpsys activity"):
            self.dumps += 1
            return self.dump
        if command.startswith("logcat"):
            return self.log
        if command == f"am start -a {runner.ALL_APPS_ACTION}":
            self.opened = True
            return ""
        if command == "input keyevent KEYCODE_HOME":
            self.opened = False
            return ""
        raise AssertionError(command)


class AppDrawerTests(unittest.TestCase):
    def test_the_drawer_is_read_from_the_icons_with_the_launchers_icon_id(self) -> None:
        nodes = runner.parse_nodes(drawer_hierarchy(*SIX))
        self.assertEqual(runner.drawer_labels(nodes), list(SIX))
        self.assertEqual(runner.drawer_problems(runner.drawer_labels(nodes)), [])

    def test_the_home_screen_is_not_the_drawer(self) -> None:
        # Files and Settings sit in the dock with the same labels, but there is no search container.
        self.assertIsNone(runner.drawer_labels(runner.parse_nodes(workspace_hierarchy())))
        self.assertEqual(runner.drawer_problems(None), ["the app drawer did not open"])

    def test_an_application_missing_from_the_drawer_is_named(self) -> None:
        labels = runner.drawer_labels(runner.parse_nodes(drawer_hierarchy("Calendar", "Files", "Gallery", "Settings")))
        problems = runner.drawer_problems(labels)
        self.assertEqual(problems, ["Calculator is not in the app drawer: it is off the home screen only, not uninstalled",
                                    "Music is not in the app drawer: it is off the home screen only, not uninstalled"])

    def test_a_dock_application_missing_from_the_drawer_is_named_without_the_excuse(self) -> None:
        labels = runner.drawer_labels(runner.parse_nodes(drawer_hierarchy(*[label for label in SIX if label != "Files"])))
        self.assertEqual(runner.drawer_problems(labels), ["Files is not in the app drawer"])

    def test_an_extra_or_repeated_application_is_a_problem(self) -> None:
        labels = runner.drawer_labels(runner.parse_nodes(drawer_hierarchy(*SIX, "Camera", "Music")))
        self.assertEqual(runner.drawer_problems(labels),
                         ["Camera is in the app drawer, which lists only the six applications of the image",
                          "Music is in the app drawer twice"])

    def test_the_four_applications_are_the_drawer_only_ones(self) -> None:
        self.assertEqual(runner.DRAWER_ONLY_LABELS, {"Calendar", "Gallery", "Music", "Calculator"})
        self.assertEqual(runner.DRAWER_LABELS, set(SIX))

    def test_the_drawer_is_opened_with_the_intent_and_the_home_screen_is_restored(self) -> None:
        guest = FakeLauncher(home_dump(), CLEAN)
        with mock.patch.object(runner.time, "sleep"):
            labels = runner.open_app_drawer(guest)
        self.assertEqual(labels, list(SIX))
        self.assertEqual(guest.commands, ["input keyevent KEYCODE_HOME", f"am start -a {runner.ALL_APPS_ACTION}",
                                          "input keyevent KEYCODE_HOME"])

    def test_a_drawer_that_never_opens_is_none(self) -> None:
        guest = FakeLauncher(home_dump(), CLEAN, drawer=None)
        clock = iter(range(0, 1000, 10))
        with mock.patch.object(runner.time, "sleep"), mock.patch.object(runner.time, "monotonic", side_effect=lambda: next(clock)):
            self.assertIsNone(runner.open_app_drawer(guest))

    def test_a_dump_that_fails_while_the_drawer_opens_is_tried_again(self) -> None:
        class Flaky(FakeLauncher):
            calls = 0

            def ui_nodes(self) -> list[dict[str, str]]:
                Flaky.calls += 1
                if Flaky.calls < 4:
                    raise runner.Failure("the UI dump is not XML")
                return super().ui_nodes()

        guest = Flaky(home_dump(), CLEAN)
        with mock.patch.object(runner.time, "sleep"):
            self.assertEqual(runner.open_app_drawer(guest), list(SIX))


class CheckLauncherTests(unittest.TestCase):
    def check(self, guest: FakeLauncher) -> None:
        with mock.patch.object(runner, "LAUNCHER_LOAD_SECONDS", 0), mock.patch.object(runner.time, "sleep"):
            runner.check_launcher(guest)

    def test_the_default_layout_passes(self) -> None:
        guest = FakeLauncher(home_dump(), CLEAN)
        self.check(guest)
        self.assertEqual(guest.dumps, 1)

    def test_the_drawer_is_checked_after_the_home_screen(self) -> None:
        guest = FakeLauncher(home_dump(), CLEAN)
        self.check(guest)
        self.assertIn(f"am start -a {runner.ALL_APPS_ACTION}", guest.commands)

    def test_an_uninstalled_application_is_a_failure(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "app drawer is not the six applications: Music is not in the app drawer: "
                                                    "it is off the home screen only, not uninstalled"):
            self.check(FakeLauncher(home_dump(), CLEAN, drawer=("Calculator", "Calendar", "Files", "Gallery", "Settings")))

    def test_a_second_page_is_a_failure(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "2 workspace pages, expected exactly one"):
            self.check(FakeLauncher(home_dump(pages=2), CLEAN))

    def test_a_widget_is_a_failure(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "app widget view"):
            self.check(FakeLauncher(home_dump(widgets=1), CLEAN))

    def test_the_workspace_may_bind_after_the_model_lists_its_items(self) -> None:
        class Binding(FakeLauncher):
            def shell(self, command: str, timeout: int = 30) -> str:
                if command.startswith("dumpsys activity") and self.dumps < 1:
                    self.dumps += 1
                    return home_dump(pages=0)
                return super().shell(command, timeout)

        guest = Binding(home_dump(), CLEAN)
        with mock.patch.object(runner, "LAUNCHER_LOAD_SECONDS", 60), mock.patch.object(runner.time, "sleep"):
            runner.check_launcher(guest)
        self.assertEqual(guest.dumps, 2)

    def test_a_log_without_the_layout_load_is_a_failure(self) -> None:
        # The check must not skip itself: it fails when the line is missing, rotated out or never logged.
        for log in ("--------- beginning of main\n", "",
                    "11-07 03:59:01.110 E/AutoInstalls( 1080): Favorite not found: x/y\n"):
            with self.subTest(log=log), self.assertRaisesRegex(
                    runner.Failure, "no 'loading default workspace' line, though the log buffers were raised to 64 MiB"):
                self.check(FakeLauncher(home_dump(), log))

    def test_the_log_is_read_from_the_main_buffer(self) -> None:
        commands: list[str] = []

        class Recording(FakeLauncher):
            def shell(self, command: str, timeout: int = 30) -> str:
                commands.append(command)
                return super().shell(command, timeout)

        self.check(Recording(home_dump(), CLEAN))
        log_commands = [command for command in commands if command.startswith("logcat")]
        self.assertEqual(len(log_commands), 1)
        self.assertIn("-b main", log_commands[0])

    def test_the_wrong_workspace_is_a_failure_naming_the_problem(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "not the default layout: 0 items, expected 2"):
            self.check(FakeLauncher(DUMP_HEAD + DUMP_TAIL, CLEAN))

    def test_a_dropped_item_is_a_failure_even_when_the_workspace_looks_right(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "dropped items of its default layout: DefaultLayoutParser"):
            self.check(FakeLauncher(home_dump(), DROPS))

    def test_the_launcher_may_load_after_the_check_starts(self) -> None:
        class Slow(FakeLauncher):
            def shell(self, command: str, timeout: int = 30) -> str:
                if command.startswith("dumpsys activity") and self.dumps < 2:
                    self.dumps += 1
                    return DUMP_HEAD + DUMP_TAIL
                return super().shell(command, timeout)

        guest = Slow(home_dump(), CLEAN)
        with mock.patch.object(runner, "LAUNCHER_LOAD_SECONDS", 60), mock.patch.object(runner.time, "sleep"):
            runner.check_launcher(guest)
        self.assertEqual(guest.dumps, 3)


# `logcat -b main -b system -b crash -g` on a guest whose buffers were raised.
LOG_SIZES_RAISED = """\
main: ring buffer is 64 MiB (3 MiB consumed), max entry is 5120 B, max payload is 4068 B
system: ring buffer is 64 MiB (1 MiB consumed), max entry is 5120 B, max payload is 4068 B
crash: ring buffer is 64 MiB (0 B consumed), max entry is 5120 B, max payload is 4068 B
"""
LOG_SIZES_DEFAULT = LOG_SIZES_RAISED.replace("64 MiB", "256 KiB")


class FakeLogDevice:
    """A guest's shell for the log buffer commands; `-G` takes effect when `accepts` is true."""

    def __init__(self, accepts: bool = True, failure: str | None = None) -> None:
        self.sizes = LOG_SIZES_DEFAULT
        self.accepts = accepts
        self.failure = failure
        self.commands: list[str] = []

    def shell(self, command: str, timeout: int = 30) -> str:
        self.commands.append(command)
        if " -G " in command:
            if self.failure:
                raise runner.Failure(f"'{command}' exited 1: {self.failure}")
            if self.accepts:
                self.sizes = LOG_SIZES_RAISED
            return ""
        if command.endswith(" -g"):
            return self.sizes
        raise AssertionError(command)


class LogBufferTests(unittest.TestCase):
    def test_sizes_are_read_in_bytes_whatever_the_unit(self) -> None:
        self.assertEqual(
            runner.log_buffer_sizes(LOG_SIZES_RAISED), {"main": 64 << 20, "system": 64 << 20, "crash": 64 << 20})
        self.assertEqual(runner.log_buffer_sizes(LOG_SIZES_DEFAULT)["main"], 256 << 10)
        self.assertEqual(runner.log_buffer_sizes("main: ring buffer is 256Kb (4Kb consumed)")["main"], 256 << 10)
        self.assertEqual(runner.log_buffer_sizes("events: ring buffer is 1.5 MiB (0 B consumed)")["events"], 3 << 19)
        self.assertEqual(runner.log_buffer_sizes("crash: ring buffer is 512 B (0 B consumed)")["crash"], 512)
        self.assertEqual(runner.log_buffer_sizes("logcat: unable to open the log"), {})

    def test_the_buffers_a_plain_logcat_reads_are_raised_and_read_back(self) -> None:
        guest = FakeLogDevice()
        runner.size_log_buffers(guest)
        self.assertEqual(guest.commands, [
            "logcat -b main -b system -b crash -G 64M", "logcat -b main -b system -b crash -g"])
        self.assertIn("main", runner.LOG_BUFFERS)
        self.assertGreaterEqual(runner.LOG_BUFFER_MIB, 64)

    def test_a_buffer_that_kept_its_size_is_a_failure_naming_it(self) -> None:
        guest = FakeLogDevice(accepts=False)
        with self.assertRaisesRegex(runner.Failure, r"not 64 MiB after logcat -G: \{'main': 262144, 'system': 262144, "):
            runner.size_log_buffers(guest)

    def test_a_buffer_the_guest_does_not_report_is_a_failure(self) -> None:
        guest = FakeLogDevice()
        guest.shell = lambda command, timeout=30: (  # type: ignore[method-assign]
            "" if " -G " in command else LOG_SIZES_RAISED.replace("crash:", "kernel:"))
        with self.assertRaisesRegex(runner.Failure, r"'crash': None"):
            runner.size_log_buffers(guest)

    def test_a_refused_resize_is_a_failure_with_the_guests_answer(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "failed to set the buffer size"):
            runner.size_log_buffers(FakeLogDevice(failure="failed to set the buffer size to 64M"))


class FakeBootingGuest:
    """A guest whose adb answers after `silent` connects and whose boot completes after `polls` reads."""

    def __init__(self, silent: int = 0, polls: int = 0, sizing: FakeLogDevice | None = None) -> None:
        self.silent = silent
        self.polls = polls
        self.log = sizing or FakeLogDevice()
        self.events: list[str] = []
        self.reads = 0

    def connect(self) -> bool:
        self.events.append("connect")
        if self.silent > 0:
            self.silent -= 1
            return False
        return True

    def shell(self, command: str, timeout: int = 30) -> str:
        if command == "getprop sys.boot_completed":
            self.reads += 1
            # The property is unset, so empty, until the boot ends.
            value = "1" if self.reads > self.polls else ""
            self.events.append(f"boot={value}")
            return value
        self.events.append("resize" if " -G " in command else "read-sizes")
        return self.log.shell(command, timeout)


class WaitBootedTests(unittest.TestCase):
    def wait(self, guest: FakeBootingGuest, seconds: int = 100) -> None:
        with mock.patch.object(runner.time, "sleep"):
            runner.wait_booted(guest, seconds, None)

    def test_the_buffers_are_raised_when_adb_first_answers_and_before_the_boot_completes(self) -> None:
        guest = FakeBootingGuest(silent=2, polls=3)
        self.wait(guest)
        self.assertEqual(guest.events[:3], ["connect", "connect", "connect"])
        resize = guest.events.index("resize")
        self.assertEqual(guest.events[3], "boot=")  # the empty answer that told adb was up
        self.assertEqual(guest.events[resize:resize + 2], ["resize", "read-sizes"])
        self.assertLess(resize, guest.events.index("boot=1"))
        self.assertEqual(guest.events.count("resize"), 1)

    def test_a_guest_that_booted_before_adb_answered_is_still_raised_once(self) -> None:
        guest = FakeBootingGuest(silent=1, polls=0)
        self.wait(guest)
        self.assertEqual(guest.events.count("resize"), 1)
        self.assertLess(guest.events.index("resize"), guest.events.index("boot=1", guest.events.index("resize")))

    def test_a_resize_that_did_not_take_ends_the_wait_with_its_own_failure(self) -> None:
        guest = FakeBootingGuest(polls=3, sizing=FakeLogDevice(accepts=False))
        with self.assertRaisesRegex(runner.Failure, "not 64 MiB after logcat -G"):
            self.wait(guest)
        self.assertNotIn("boot=1", guest.events)

    def test_adb_that_never_answers_is_a_failure_at_the_deadline(self) -> None:
        guest = FakeBootingGuest(silent=10**6)
        with self.assertRaisesRegex(runner.Failure, "the guest's adb to answer: not within 0 s"):
            self.wait(guest, seconds=0)
        self.assertNotIn("resize", guest.events)

    def test_the_guest_is_sized_before_the_exercise_starts(self) -> None:
        order: list[str] = []
        guest = mock.Mock()
        guest.poll.return_value = 0
        options = mock.Mock(adb_port=5576, boot_timeout=100)
        with mock.patch.object(runner, "launch", return_value=(guest, Path("launcher.log"))), \
                mock.patch.object(runner, "stop_launcher"), \
                mock.patch.object(runner, "wait_booted", side_effect=lambda *args: order.append("sized and booted")):
            runner.run_guest(options, Path("."), "wizard", "adb", {}, [], lambda *args: order.append("exercise"))
        self.assertEqual(order, ["sized and booted", "exercise"])


class LaunchCommandTests(unittest.TestCase):
    def command(self, **display) -> list[str]:
        options = argparse.Namespace(adb_port=5576, cpus=4, boot_timeout=100, release=None, emulator=None,
                                     **{"width": None, "height": None, "dpi": None, **display})
        with mock.patch.object(runner.subprocess, "Popen") as popen, tempfile.TemporaryDirectory() as scratch:
            runner.launch(options, Path(scratch), "wizard", "adb", [])
        return popen.call_args.args[0]

    def test_the_display_is_the_launchers_default_unless_stated(self) -> None:
        command = self.command()
        self.assertFalse(any(value in command for value in ("--width", "--height", "--dpi")))

    def test_a_display_profile_reaches_the_launcher(self) -> None:
        command = self.command(width=1179, height=2400, dpi=480)
        for name, value in (("--width", "1179"), ("--height", "2400"), ("--dpi", "480")):
            self.assertEqual(command[command.index(name) + 1], value)


if __name__ == "__main__":
    unittest.main()
