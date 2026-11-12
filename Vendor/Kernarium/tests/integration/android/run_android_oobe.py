#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify an Android release's first boot and the harness helper that skips it.

The product starts a fresh guest in LineageOS Setup Wizard. This runner boots
the release twice with the macOS launcher (tools/boot_android.py), each time on
a fresh instance, and checks the contract in android/lineage-24/README.md:

- wizard: the guest is unprovisioned (`ro.setupwizard.mode` OPTIONAL,
  `device_provisioned` and `user_setup_complete` 0), HOME resolves to the
  wizard and stays there when pressed, and the wizard's first page is on
  screen. The runner then completes the wizard through the pages the product
  shows, exactly these and in this order: Welcome, Language, Date and time,
  Theme. Finish on the Theme page goes straight to the home screen: the
  wizard's closing page (the logo with its home button) is not shown, and its
  activity never starts. Setup ends in the state the product defines for a fresh guest:
  both settings 1, the wizard's HOME activity and flow manager disabled, HOME
  resolving to, and pressed back to, the launcher, three-button navigation
  (`navigation_mode` 0, the gestural overlay off and both mode overlays
  present), no screen lock, location on as it was before the wizard, Wi-Fi
  connected to VirtWifi, no status bar lock held by the wizard, and a home
  screen that is one page with an empty workspace, no widget and a dock of
  exactly Files and Settings, neither dropped while the launcher read its
  layout, while the app drawer still lists all six applications (Calendar,
  Gallery, Music and Calculator are reached from it). The
  drop check reads the guest's log, so the runner raises the log buffers as
  soon as adb answers, before the wizard runs, and a log without the
  launcher's layout load is a failure, not a skipped check. `--width`,
  `--height` and `--dpi` pick the display, so a phone and a tablet density
  each get their own grid;
- helper: the guest is started with `--provision`, the harness path of
  tools/android_provision.py, and reaches the same observable state without a
  person: both settings 1 and HOME on the launcher. The shell may not disable
  the wizard's components, so the helper disables the wizard package for user 0
  instead, once the wizard is bound, and stops its process, which holds a status
  bar lock until it dies. The helper then finds nothing left to do. Its home
  screen is checked the same.

Neither guest may have a wizard crash in its crash buffer: the helper's disable
landing while the wizard binds kills the process with a NameNotFoundException.

The flow is the contract of release r18 and later: a release that still shows
LineageOS's other pages, or the closing page, fails at the first page that is
not in the list.

Every wait has a deadline; the guest's launcher is stopped through its own
process on any exit.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ElementTree
from typing import Callable

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
LAUNCHER = REPOSITORY / "tools" / "boot_android.py"
sys.path.insert(0, str(REPOSITORY / "tools"))
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import android_provision  # noqa: E402
import slot_ports  # noqa: E402

DEFAULT_ADB_PORT = 5576

WIZARD = android_provision.SETUP_WIZARD
LAUNCHER_PACKAGE = "com.android.launcher3"
WELCOME_ACTIVITY = f"{WIZARD}/.WelcomeActivity"
LOCALE_ACTIVITY = f"{WIZARD}/.locale.LocaleActivity"
DATETIME_ACTIVITY = f"{WIZARD}/.time.DateTimeActivity"
THEME_ACTIVITY = f"{WIZARD}/.settings.ThemeSettingsActivity"
# The closing page of LineageOS's flow (the logo and the home button): the product skips it.
FINISH_ACTIVITY = f"{WIZARD}/.FinishActivity"
FINISH_LOG_TAG = "FinishActivity"
# The reviewed flow: the pages the wizard shows, in order. Finish on the last one goes home.
FLOW = (WELCOME_ACTIVITY, LOCALE_ACTIVITY, DATETIME_ACTIVITY, THEME_ACTIVITY)
NAVBAR_GESTURAL = "com.android.internal.systemui.navbar.gestural"
NAVBAR_3BUTTON = "com.android.internal.systemui.navbar.threebutton"
WIFI_NETWORK = "VirtWifi"
LAUNCHER_ACTIVITY = f"{LAUNCHER_PACKAGE}/.uioverrides.QuickstepLauncher"
# The launcher's default layout (android/lineage-24/overlay/overlay/packages/apps/Launcher3): the dock holds
# these two and the workspace nothing. The other four applications with a launcher activity stay in the app drawer.
DOCK_APPS = frozenset({
    ("com.android.settings", "com.android.settings.Settings"),
    ("com.android.documentsui", "com.android.documentsui.LauncherActivity"),
})
# What the app drawer lists: every application with a launcher activity, by the label the launcher shows.
DRAWER_LABELS = frozenset({"Calculator", "Calendar", "Files", "Gallery", "Music", "Settings"})
DRAWER_ONLY_LABELS = DRAWER_LABELS - {"Files", "Settings"}
ALL_APPS_ACTION = "android.intent.action.ALL_APPS"
DRAWER_ICON_ID = f"{LAUNCHER_PACKAGE}:id/icon"
DRAWER_SEARCH_ID = f"{LAUNCHER_PACKAGE}:id/search_container_all_apps"
LAUNCHER_LOAD_SECONDS = 60
UI_DUMP = "/data/local/tmp/andarium-oobe-ui.xml"
BOUNDS = re.compile(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]")
RESUMED = re.compile(r"ResumedActivity: ActivityRecord\{\S+ u\d+ (\S+) t\d+\}")
# A wizard process that died of an uncaught exception is a `Process:` line of the crash buffer.
WIZARD_FATAL = re.compile(r"^.*\bProcess: " + re.escape(WIZARD) + r"(?![\w.]).*$", re.MULTILINE)
ADB_LOST = re.compile(r"offline|device not found|no devices|closed|unauthorized", re.IGNORECASE)
RECONNECT_ATTEMPTS = 8
# What the wizard disables itself when its last page finishes.
WIZARD_DISABLES = ("SetupWizardActivity", "wizardmanager.WizardManager")
# The wizard finishes asynchronously: its flow manager is disabled a moment after the Finish page, and a loaded host takes longer.
WIZARD_RELEASE_SECONDS = 30
# A status bar disable record of the wizard, with the two flag words it holds: `dumpsys statusbar`'s mDisableRecords.
WIZARD_RECORD_FLAGS = re.compile(r"\bwhat1=0x([0-9a-fA-F]+)\s+what2=0x([0-9a-fA-F]+)\s+pkg=" + re.escape(WIZARD) + r"(?![\w.])")
WIZARD_STEPS = 20


class Failure(RuntimeError):
    pass


# --- the wizard's pages ---------------------------------------------------------


def resumed_activity(dumpsys: str) -> str:
    """The activity at the top of the resumed stack, `package/.Class`, or an empty string."""
    match = RESUMED.search(dumpsys)
    return match.group(1) if match else ""


def parse_nodes(hierarchy: str) -> list[dict[str, str]]:
    """The attributes of every node of a uiautomator dump."""
    start = hierarchy.find("<?xml")
    if start < 0:
        start = hierarchy.find("<hierarchy")
    try:
        root = ElementTree.fromstring(hierarchy[start:] if start >= 0 else hierarchy)
    except ElementTree.ParseError as error:
        raise Failure(f"the UI dump is not XML: {error}") from error
    return [dict(node.attrib) for node in root.iter("node")]


def center(bounds: str) -> tuple[int, int]:
    match = BOUNDS.fullmatch(bounds)
    if match is None:
        raise Failure(f"unreadable bounds {bounds!r}")
    left, top, right, bottom = map(int, match.groups())
    return (left + right) // 2, (top + bottom) // 2


# What each page of the wizard is asked: the id of its button, the text of its
# button, or its first list item.
PAGES: dict[str, tuple[str, str]] = {
    WELCOME_ACTIVITY: ("id", f"{WIZARD}:id/start"),
    LOCALE_ACTIVITY: ("item", "sud_items_title"),
    DATETIME_ACTIVITY: ("text", "Next"),
    # The last page: its button reads Finish, and it ends the wizard.
    THEME_ACTIVITY: ("text", "Finish"),
}
assert tuple(PAGES) == FLOW


def wizard_action(activity: str, nodes: list[dict[str, str]]) -> tuple[str, int, int]:
    """What to do on this page: ("tap", x, y)."""
    rule = PAGES.get(activity)
    if rule is None:
        raise Failure(f"the wizard shows a page this runner does not know: {activity!r}")
    kind, target = rule
    for node in nodes:
        if kind == "id" and node.get("resource-id") == target:
            return ("tap", *center(node["bounds"]))
        if kind == "text" and node.get("text") == target:
            return ("tap", *center(node["bounds"]))
        if kind == "item" and node.get("resource-id", "").endswith(target):
            return ("tap", *center(node["bounds"]))
    raise Failure(f"{activity} has no {kind} {target!r} to tap")


def next_page(position: int, activity: str) -> int:
    """The index in FLOW after seeing `activity` while the flow stands at `position`.

    The flow is strict: the page on screen is the one expected, or the one after
    it (the tap went through). Any other page, a hidden one or a reviewed one out
    of order, is a failure."""
    if activity == FLOW[position]:
        return position
    if position + 1 < len(FLOW) and activity == FLOW[position + 1]:
        return position + 1
    expected = FLOW[position] if position + 1 >= len(FLOW) else f"{FLOW[position]} or {FLOW[position + 1]}"
    raise Failure(f"the wizard shows {activity!r}, expected {expected}")


OVERLAY_LINE = re.compile(r"^(?:\[(?P<state>[ x])\]|(?P<missing>---)) (?P<package>\S+)$", re.MULTILINE)
WIFI_INFO = re.compile(r'mWifiInfo SSID: "(?P<ssid>[^"]*)".*Supplicant state: (?P<state>[A-Z_]+)')


def overlay_states(listing: str) -> dict[str, str]:
    """`cmd overlay list` as package -> "x" (enabled), " " (disabled) or "-" (not usable)."""
    return {match.group("package"): match.group("state") or "-" for match in OVERLAY_LINE.finditer(listing)}


def wifi_network(dumpsys_wifi: str) -> tuple[str, str] | None:
    """The connected network's name and supplicant state from `dumpsys wifi`, or None."""
    match = WIFI_INFO.search(dumpsys_wifi)
    return (match.group("ssid"), match.group("state")) if match else None


LAUNCHER_ITEM = re.compile(
    r"^ *\tItemInfo\(id=(?P<id>-?\d+) type=(?P<type>\w+) container=[^\n]*\n(?P<body>.*?)"
    r"(?=^ *\tItemInfo\(id=|^\s*---- extra items|\Z)", re.MULTILINE | re.DOTALL)
LAUNCHER_CONTAINER = re.compile(r"(?P<kind>\w+) \{(?P<fields>[^}]*)\}")
LAUNCHER_COMPONENT = re.compile(r"targetComponent=ComponentInfo\{(?P<package>[^/}]+)/(?P<class>[^}]+)\}")
LAUNCHER_LOG = re.compile(r"^\d\d-\d\d \d\d:\d\d:\d\d\.\d+ (?P<level>[VDIWEF])/(?P<tag>[^\s(]+)\s*\(\s*\d+\): (?P<message>.*)$",
                          re.MULTILINE)
LAUNCHER_LAYOUT_LOADED = "loading default workspace"
LAUNCHER_LAYOUT_TAGS = ("DefaultLayoutParser", "AutoInstalls")
# The launcher logs through android.util.Log, which lands in `main`. A guest's default buffer is
# 256 KiB and a boot fills it in seconds, so the runner raises the buffers a plain `logcat` reads
# as soon as adb answers: from then on the load line, which comes with the launcher's first
# activity, cannot rotate out before the check reads it.
LOG_BUFFERS = ("main", "system", "crash")
LOG_BUFFER_MIB = 64
LOG_BUFFER_SIZE = re.compile(r"^(?P<buffer>\w+): ring buffer is (?P<size>\d+(?:\.\d+)?) ?(?P<unit>[KMG]?)i?[Bb]\b",
                             re.MULTILINE)
LOG_UNITS = {"": 1, "K": 1 << 10, "M": 1 << 20, "G": 1 << 30}


def launcher_items(dump: str) -> list[dict]:
    """The items of the launcher's `Data Model` in a dump of its activity, without the predictions.

    The dump prints a container as protobuf text, which leaves a zero field out: an item on the
    first page has no `page_index`."""
    section = dump.partition("Data Model:")[2].partition("---- extra items")[0]
    items = []
    for match in LAUNCHER_ITEM.finditer(section):
        if int(match.group("id")) < 0:
            continue
        body = match.group("body")
        container = LAUNCHER_CONTAINER.match(body)
        page = re.search(r"page_index: (\d+)", container.group("fields")) if container else None
        rank = re.search(r"index: (\d+)", container.group("fields")) if container else None
        component = LAUNCHER_COMPONENT.search(body)
        items.append({
            "id": int(match.group("id")), "type": match.group("type"),
            "container": container.group("kind") if container else "?",
            "page": int(page.group(1)) if page else 0,
            "rank": int(rank.group(1)) if rank else 0,
            "component": (component.group("package"), component.group("class")) if component else None,
        })
    return items


def launcher_problems(items: list[dict]) -> list[str]:
    """What is wrong with the home screen's items: Files and Settings in the dock and nothing else."""
    problems = []
    if len(items) != len(DOCK_APPS):
        problems.append(f"{len(items)} items, expected {len(DOCK_APPS)}")
    for item in items:
        name = "/".join(item["component"]) if item["component"] else f"item {item['id']}"
        if item["type"] != "APP" or item["component"] not in DOCK_APPS:
            problems.append(f"{name} ({item['type']}) is not Files or Settings: the home screen holds only those two")
        if item["container"] == "workspace":
            problems.append(f"{name} is on the workspace page {item['page']}, which stays empty")
        elif item["container"] != "hotseat":
            problems.append(f"{name} is in the {item['container']}, not the dock")
    for package, class_name in sorted(DOCK_APPS - {item["component"] for item in items}):
        problems.append(f"{package}/{class_name} is missing from the dock")
    return problems


def drawer_labels(nodes: list[dict[str, str]]) -> list[str] | None:
    """The labels of the app drawer's icons in a dump of the screen, or None when the drawer is not what is shown.

    The drawer is on screen when its search container is; its icons are the nodes with the launcher's `icon`
    id, which the workspace's and the dock's icons do not carry."""
    if not any(node.get("resource-id") == DRAWER_SEARCH_ID for node in nodes):
        return None
    return sorted(node.get("text", "") for node in nodes if node.get("resource-id") == DRAWER_ICON_ID)


def drawer_problems(labels: list[str] | None) -> list[str]:
    """What is wrong with the app drawer: it lists the six applications, each once, and nothing else."""
    if labels is None:
        return ["the app drawer did not open"]
    problems = []
    for label in sorted(DRAWER_LABELS - set(labels)):
        problems.append(f"{label} is not in the app drawer" + (
            ": it is off the home screen only, not uninstalled" if label in DRAWER_ONLY_LABELS else ""))
    for label in sorted(set(labels) - DRAWER_LABELS):
        problems.append(f"{label} is in the app drawer, which lists only the six applications of the image")
    for label in sorted({label for label in labels if labels.count(label) > 1}):
        problems.append(f"{label} is in the app drawer twice")
    return problems


WORKSPACE_VIEW = re.compile(r"^(?P<indent> *)com\.android\.launcher3\.Workspace\{")


def workspace_view(dump: str) -> tuple[int, list[str]] | None:
    """The workspace's pages and widget views in the launcher's view hierarchy, or None without one.

    A page is a `CellLayout` child of the `Workspace`; a widget is an app widget host view anywhere
    below it. The hierarchy comes in the same dump as the data model, indented by depth."""
    lines = dump.splitlines()
    for start, line in enumerate(lines):
        header = WORKSPACE_VIEW.match(line)
        if header is None:
            continue
        depth = len(header.group("indent"))
        subtree = []
        for below in lines[start + 1:]:
            if below.strip() and len(below) - len(below.lstrip()) <= depth:
                break
            subtree.append(below)
        children = [len(below) - len(below.lstrip()) for below in subtree if below.strip()]
        if not children:
            return 0, []
        child_depth = min(children)
        pages = sum(1 for below in subtree
                    if len(below) - len(below.lstrip()) == child_depth
                    and below.lstrip().startswith("com.android.launcher3.CellLayout{"))
        widgets = [below.strip().split("{")[0] for below in subtree if "AppWidgetHostView{" in below]
        return pages, widgets
    return None


def workspace_view_problems(view: tuple[int, list[str]] | None) -> list[str]:
    """What is wrong with the workspace's views: one page, and no widget on it."""
    if view is None:
        return ["the dump has no workspace view, so the page count and the widgets cannot be read"]
    pages, widgets = view
    problems = []
    if pages != 1:
        problems.append(f"{pages} workspace pages, expected exactly one")
    if widgets:
        problems.append(f"{len(widgets)} app widget view(s) on the workspace: {', '.join(sorted(set(widgets)))}")
    return problems


def log_buffer_sizes(logcat_g: str) -> dict[str, int]:
    """Each buffer's capacity in bytes from `logcat -g`, whose lines read `main: ring buffer is 64 MiB (...)`."""
    return {match.group("buffer"): int(float(match.group("size")) * LOG_UNITS[match.group("unit")])
            for match in LOG_BUFFER_SIZE.finditer(logcat_g)}


def launcher_layout_drops(log: str) -> list[str] | None:
    """The layout parser's complaints from the launcher's last load of its default layout.

    None when the log holds no such load, which the caller refuses. An item the layout names that the image
    lacks is logged by the parser and dropped; the layout names no widget, so the widget lookup failing is a drop too."""
    start = log.rfind(LAUNCHER_LAYOUT_LOADED)
    if start < 0:
        return None
    drops = []
    for match in LAUNCHER_LOG.finditer(log[start:]):
        if match.group("tag") in LAUNCHER_LAYOUT_TAGS and (
                match.group("level") in "WE" or "Can't find" in match.group("message")):
            drops.append(f"{match.group('tag')}: {match.group('message')}")
    return drops


def wizard_fatals(crash_log: str) -> list[str]:
    """The crash buffer's lines that name the wizard's process as the one that died."""
    return [match.group(0).strip() for match in WIZARD_FATAL.finditer(crash_log)]


def wizard_holds_status_bar(dump: str) -> bool | None:
    """Whether the wizard's status bar disable record disables anything, None when the dump holds no record list.

    A finished wizard keeps its process, cached, and with it a record whose two flag words are zero: the record stays
    until the process dies, and a zero record hides nothing. Release r16 does the same."""
    if "mDisableRecords" not in dump:
        return None
    return any(int(first, 16) or int(second, 16) for first, second in WIZARD_RECORD_FLAGS.findall(dump))


def disabled_components(dumpsys_package: str) -> set[str]:
    """The component class names the wizard package lists as disabled for user 0."""
    lines = dumpsys_package.splitlines()
    names: set[str] = set()
    for index, line in enumerate(lines):
        if line.strip() == "disabledComponents:":
            indent = len(line) - len(line.lstrip())
            for following in lines[index + 1:]:
                if len(following) - len(following.lstrip()) <= indent or not following.strip():
                    break
                names.add(following.strip())
            break
    return names


# --- the guest ------------------------------------------------------------------


class Device:
    """The guest as the host's adb sees it: the shared server, always named by serial."""

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
        # A user build restarts adbd when its first-boot work ends, which drops
        # the session for a moment: reconnect and ask again before failing.
        for attempt in range(RECONNECT_ATTEMPTS):
            result = self.run("-s", self.serial, "shell", command, timeout=timeout)
            if result.returncode == 0:
                return result.stdout.replace("\r", "").strip()
            if attempt == RECONNECT_ATTEMPTS - 1 or ADB_LOST.search(result.stdout) is None:
                break
            time.sleep(3)
            self.connect()
        raise Failure(f"'{command}' exited {result.returncode}: {result.stdout.strip()}")

    def setting(self, namespace: str, name: str) -> str:
        return self.shell(f"settings get {namespace} {name}")

    def resumed(self) -> str:
        return resumed_activity(self.shell("dumpsys activity activities"))

    def home_package(self) -> str:
        return android_provision.home_package(lambda command: self.shell(command))

    def ui_nodes(self) -> list[dict[str, str]]:
        return parse_nodes(self.shell(
            f"rm -f {UI_DUMP}; uiautomator dump {UI_DUMP} >/dev/null 2>&1; cat {UI_DUMP}; rm -f {UI_DUMP}",
            timeout=60))

    def wizard_status_bar_record(self) -> bool | None:
        return android_provision.wizard_status_bar_record(self.shell)

    def wizard_status_bar_lock(self) -> bool | None:
        return wizard_holds_status_bar(self.shell("dumpsys statusbar"))

    def wizard_processes(self) -> list[str] | None:
        # pidof exits 1 when no process has the name; that is the answer (an empty list), not an error of the shell.
        return android_provision.wizard_processes(lambda command: self.shell(f"{command}; true"))


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


def size_log_buffers(device: Device) -> None:
    """Raise the guest's log buffers and read the sizes back: a buffer that kept its size is a failure.

    The shell user may resize the buffers (it is in the log group), and the sizes last until the
    guest stops, which is what a run needs. Nothing in the image changes."""
    selection = " ".join(f"-b {name}" for name in LOG_BUFFERS)
    device.shell(f"logcat {selection} -G {LOG_BUFFER_MIB}M")
    sizes = log_buffer_sizes(device.shell(f"logcat {selection} -g"))
    wanted = LOG_BUFFER_MIB << 20
    short = {name: sizes.get(name) for name in LOG_BUFFERS if sizes.get(name, 0) < wanted}
    if short:
        raise Failure(f"the log buffers are not {LOG_BUFFER_MIB} MiB after logcat -G: {short} "
                      "(None: the buffer is not in `logcat -g`); the launcher's layout load could rotate out")


def wait_booted(device: Device, seconds: int, guest: subprocess.Popen | None) -> None:
    """Return once the guest has booted; its log buffers are raised the moment adb first answers."""
    deadline = time.monotonic() + seconds

    def answers() -> bool:
        # sys.boot_completed is unset until the boot ends, so any answer is an answer.
        if not device.connect():
            return False
        device.shell("getprop sys.boot_completed")
        return True

    wait_for("the guest's adb to answer", answers, seconds, guest)
    size_log_buffers(device)
    wait_for("the guest to be booted", lambda: device.shell("getprop sys.boot_completed") == "1",
             max(1, int(deadline - time.monotonic())), guest)


def stop_launcher(guest: subprocess.Popen) -> None:
    if guest.poll() is None:
        guest.send_signal(signal.SIGTERM)
        try:
            guest.wait(timeout=120)
        except subprocess.TimeoutExpired:
            guest.kill()
            guest.wait(timeout=30)


# --- the checks -----------------------------------------------------------------


# What the wizard's Location page used to write and the product's flow no longer shows:
# the platform's values stay as they were before the wizard ran.
LOCATION_SETTINGS = (("secure", "location_mode"), ("global", "assisted_gps_enabled"))


def location_state(device: Device) -> dict[str, str]:
    return {f"{namespace} {name}": device.setting(namespace, name) for namespace, name in LOCATION_SETTINGS}


def check_unprovisioned(device: Device) -> dict[str, str]:
    """A fresh guest is in Setup Wizard, which HOME does not leave; returns its location settings."""
    mode = device.shell("getprop ro.setupwizard.mode")
    if mode != "OPTIONAL":
        raise Failure(f"ro.setupwizard.mode is {mode!r}, expected OPTIONAL")
    for namespace, name in (("global", "device_provisioned"), ("secure", "user_setup_complete")):
        value = device.setting(namespace, name)
        if value != "0":
            raise Failure(f"{name} is {value!r} on a fresh data partition, expected 0")
    wait_for("the wizard's welcome page", lambda: device.resumed() == WELCOME_ACTIVITY, 90)
    if device.home_package() != WIZARD:
        raise Failure(f"HOME resolves to {device.home_package()!r} on a fresh guest, expected the wizard")
    device.shell("input keyevent KEYCODE_HOME")
    time.sleep(3)
    if device.resumed() != WELCOME_ACTIVITY:
        raise Failure(f"HOME left the wizard for {device.resumed()!r} on an unprovisioned guest")
    print("fresh guest: unprovisioned, HOME stays in the wizard")
    return location_state(device)


def ui_nodes(device: Device) -> list[dict[str, str]]:
    """The page's nodes; a dump taken while the page changes is not XML, so ask again."""
    for attempt in range(3):
        try:
            return device.ui_nodes()
        except Failure:
            if attempt == 2:
                raise
            time.sleep(2)
    return []


def complete_wizard(device: Device) -> None:
    """Answer the wizard's pages until the launcher is on screen, and only these pages, in order."""
    position = 0
    unknown = 0
    answered: set[int] = set()
    for _ in range(WIZARD_STEPS):
        activity = device.resumed()
        if activity.startswith(f"{LAUNCHER_PACKAGE}/"):
            if position != len(FLOW) - 1 or position not in answered:
                raise Failure(f"the wizard ended at the launcher after {FLOW[position]}, before the whole flow "
                              f"{' > '.join(page.split('/')[-1] for page in FLOW)}")
            print(f"wizard completed through {len(FLOW)} pages, and Finish went to the home screen")
            return
        if activity == FINISH_ACTIVITY:
            raise Failure("the wizard shows its closing page, which the product skips: Finish on the "
                          f"{THEME_ACTIVITY.split('/')[-1]} page goes straight to the home screen")
        # A page that is not in the flow may be a transition: look again before
        # calling it unexpected.
        if activity not in FLOW:
            unknown += 1
            if unknown > 3:
                raise Failure(f"the wizard shows a page the product does not: {activity!r}")
            time.sleep(3)
            continue
        unknown = 0
        position = next_page(position, activity)
        action = wizard_action(activity, ui_nodes(device))
        device.shell(f"input tap {action[1]} {action[2]}")
        answered.add(position)
        # Give the page time to change before the next look.
        time.sleep(4)
    raise Failure(f"the wizard did not finish in {WIZARD_STEPS} steps; at {device.resumed()!r}")


def check_defaults(device: Device, before: dict[str, str] | None) -> None:
    """The platform's defaults of a fresh guest, which the shortened flow leaves alone."""
    mode = device.setting("secure", "navigation_mode")
    if mode != "0":
        raise Failure(f"navigation_mode is {mode!r}, expected 0 (three buttons): the wizard may not choose the mode")
    states = overlay_states(device.shell("cmd overlay list android"))
    for package in (NAVBAR_GESTURAL, NAVBAR_3BUTTON):
        if package not in states:
            raise Failure(f"the navigation mode overlay {package} is not installed: {states}")
    if states[NAVBAR_GESTURAL] == "x":
        raise Failure(f"the gestural navigation overlay is enabled: {states}")
    disabled = device.shell("locksettings get-disabled")
    if disabled != "true":
        raise Failure(f"locksettings get-disabled is {disabled!r}, expected true (no screen lock)")
    location = location_state(device)
    if location["secure location_mode"] != "3":
        raise Failure(f"location is not on: {location}")
    if before is not None and location != before:
        raise Failure(f"the wizard changed the location settings: before {before}, after {location}")
    wait_for(f"Wi-Fi connected to {WIFI_NETWORK}",
             lambda: wifi_network(device.shell("dumpsys wifi | grep 'mWifiInfo SSID'")) == (WIFI_NETWORK, "COMPLETED"), 60)
    print(f"defaults: three buttons, no screen lock, location {location}, Wi-Fi on {WIFI_NETWORK}")


def check_wizard_released(device: Device, *, by_wizard: bool) -> None:
    """The wizard holds no status bar lock: its disable record disables nothing, and for the helper there is no process.

    A wizard that ran to its last page may take a moment to let go; a lock that stays is
    the empty status bar and the single Back key of a guest nobody can use. The process of a wizard that
    finished lives on, cached, with a record of two zero flag words: that is released (release r16 ends the same way)."""
    wait_for("the wizard to hold no status bar lock", lambda: device.wizard_status_bar_lock() is False,
             WIZARD_RELEASE_SECONDS)
    if not by_wizard and device.wizard_processes() != []:
        raise Failure(f"the setup wizard still runs after the helper: {device.wizard_processes()}")
    print("the wizard holds no status bar lock")


def finish_activity_lines(log: str) -> list[str]:
    """The lines of the closing page's log tag in a `logcat -v time` dump; the page logs when it is created."""
    return [match.group(0) for match in LAUNCHER_LOG.finditer(log) if match.group("tag") == FINISH_LOG_TAG]


def check_wizard_skipped_finish(device: Device) -> None:
    """The wizard's closing page never started, in either guest: Finish on the last page went to the home screen.

    A poll of the resumed activity could miss a page that flashes by, so the proof is the page's own log line,
    which its activity writes when it is created. The main buffer was raised before the wizard ran."""
    lines = finish_activity_lines(device.shell(f"logcat -b main -d -v time -s {FINISH_LOG_TAG}:V", timeout=60))
    if lines:
        raise Failure("the wizard started its closing page, which the product skips: " + lines[0])
    print("the wizard did not start its closing page")


def check_wizard_did_not_crash(device: Device) -> None:
    """The wizard's process did not die of an exception, in either guest.

    The crash buffer was raised with the others before the wizard ran (wait_booted), so a crash
    during the boot is still there. The helper must not disable the wizard while its process binds."""
    fatals = wizard_fatals(device.shell("logcat -b crash -d", timeout=60))
    if fatals:
        raise Failure("the setup wizard crashed: " + "; ".join(fatals[:3]))
    print("the wizard did not crash")


def open_app_drawer(device: Device) -> list[str] | None:
    """The labels of the app drawer, opened with the launcher's ALL_APPS intent; None when it does not open."""
    device.shell("input keyevent KEYCODE_HOME")
    wait_for("the launcher", lambda: device.resumed().startswith(f"{LAUNCHER_PACKAGE}/"), 30)
    device.shell(f"am start -a {ALL_APPS_ACTION}")
    labels: list[str] | None = None
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        time.sleep(2)
        try:
            labels = drawer_labels(ui_nodes(device))
        except Failure:
            continue
        if labels is not None:
            break
    device.shell("input keyevent KEYCODE_HOME")
    return labels


def check_app_drawer(device: Device) -> None:
    """The four applications that are off the home screen are still installed and listed in the app drawer."""
    problems = drawer_problems(open_app_drawer(device))
    if problems:
        raise Failure("the app drawer is not the six applications: " + "; ".join(problems))
    print("app drawer: lists " + ", ".join(sorted(DRAWER_LABELS)))


def check_launcher(device: Device) -> None:
    """The home screen is one page with an empty workspace and a dock of Files and Settings, and no widget.

    The launcher reads its layout when it first starts, and a guest keeps what it built, so this is
    a fresh guest's check. A release whose layout names an application the image lacks drops it and
    the parser says so in the log. The load line is in the log of every fresh guest, because the
    buffers were raised before the wizard ran (wait_booted): a log without it is a failure."""
    deadline = time.monotonic() + LAUNCHER_LOAD_SECONDS
    while True:
        dump = device.shell(f"dumpsys activity {LAUNCHER_ACTIVITY}", timeout=60)
        items = launcher_items(dump)
        view = workspace_view(dump)
        # The model lists the items before the workspace binds them to a page.
        if (len(items) >= len(DOCK_APPS) and view is not None and view[0] >= 1) or time.monotonic() >= deadline:
            break
        time.sleep(2)
    problems = launcher_problems(items) + workspace_view_problems(view)
    if problems:
        raise Failure("the launcher's home screen is not the default layout: " + "; ".join(problems))
    log = device.shell("logcat -b main -d -v time -s ModelDbController:D "
                       + " ".join(f"{tag}:D" for tag in LAUNCHER_LAYOUT_TAGS), timeout=60)
    drops = launcher_layout_drops(log)
    if drops is None:
        raise Failure(f"the guest's log has no '{LAUNCHER_LAYOUT_LOADED}' line, though the log buffers were raised to "
                      f"{LOG_BUFFER_MIB} MiB before the wizard ran: the launcher did not load its default layout, "
                      "or the drops cannot be checked")
    if drops:
        raise Failure("the launcher dropped items of its default layout: " + "; ".join(drops[:5]))
    print("home screen: one page, an empty workspace, no widget, the dock holds Files and Settings, no item dropped")
    check_app_drawer(device)


def check_provisioned(device: Device, *, by_wizard: bool, before: dict[str, str] | None = None) -> None:
    """The end state: both settings 1, and HOME the launcher, pressed or not.

    The wizard ends by disabling its HOME activity and its flow manager, and
    leaves the package enabled. The shell may not change another package's
    component state, so the helper disables the wizard package for user 0.
    `before` is the wizard guest's location settings from before the wizard.
    """
    for namespace, name in (("global", "device_provisioned"), ("secure", "user_setup_complete")):
        value = device.setting(namespace, name)
        if value != "1":
            raise Failure(f"{name} is {value!r} after setup, expected 1")
    if device.home_package() != LAUNCHER_PACKAGE:
        raise Failure(f"HOME resolves to {device.home_package()!r} after setup, expected the launcher")
    device.shell("input keyevent KEYCODE_HOME")
    wait_for("HOME to reach the launcher", lambda: device.resumed().startswith(f"{LAUNCHER_PACKAGE}/"), 30)
    package_disabled = f"package:{WIZARD}" in device.shell("pm list packages -d").splitlines()
    if by_wizard:
        if package_disabled:
            raise Failure("the wizard package is disabled after the wizard finished, not only its activities")
        def undisabled() -> tuple[set[str], set[str]]:
            disabled = disabled_components(device.shell(f"dumpsys package {WIZARD}"))
            return {name for name in WIZARD_DISABLES if f"{WIZARD}.{name}" not in disabled}, disabled

        try:
            wait_for("the wizard to disable its HOME activity and its flow manager", lambda: not undisabled()[0],
                     WIZARD_RELEASE_SECONDS)
        except Failure:
            missing, disabled = undisabled()
            raise Failure(f"the wizard did not disable {sorted(missing)} within {WIZARD_RELEASE_SECONDS} s; "
                          f"disabled: {sorted(disabled)}") from None
    elif not package_disabled:
        raise Failure("the helper left the wizard package enabled")
    print("provisioned: settings 1, HOME is the launcher, the wizard "
          + ("disabled its HOME activity and manager" if by_wizard else "package is disabled"))
    check_wizard_released(device, by_wizard=by_wizard)
    check_wizard_did_not_crash(device)
    check_wizard_skipped_finish(device)
    check_defaults(device, before)
    check_launcher(device)


# --- the runner -----------------------------------------------------------------


def launch(options: argparse.Namespace, scratch: pathlib.Path, name: str, adb: str,
           extra: list[str]) -> tuple[subprocess.Popen, pathlib.Path]:
    serial_port = options.adb_port
    log_path = scratch / f"{name}-launcher.log"
    command = [sys.executable, str(LAUNCHER), "--instance", str(scratch / f"{name}-instance"),
               "--adb", adb, "--adb-port", str(serial_port), "--display", "offscreen",
               "--cpus", str(options.cpus), "--boot-timeout", str(options.boot_timeout), *extra]
    for name in ("width", "height", "dpi"):
        if getattr(options, name):
            command += [f"--{name}", str(getattr(options, name))]
    if options.release:
        command += ["--release", str(options.release)]
    if options.emulator:
        command += ["--emulator", str(options.emulator)]
    with open(log_path, "wb") as log:
        guest = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                 env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
    return guest, log_path


def run_guest(options: argparse.Namespace, scratch: pathlib.Path, name: str, adb: str,
              environment: dict[str, str], extra: list[str],
              exercise: Callable[[Device, pathlib.Path], None]) -> None:
    guest, log_path = launch(options, scratch, name, adb, extra)
    try:
        device = Device(adb, f"127.0.0.1:{options.adb_port}", environment)
        wait_booted(device, options.boot_timeout, guest)
        exercise(device, log_path)
    except (Failure, subprocess.TimeoutExpired, OSError):
        if log_path.is_file():
            print(f"{name} launcher log tail:\n" + "\n".join(log_path.read_text(errors="replace").splitlines()[-30:]),
                  file=sys.stderr)
        raise
    finally:
        stop_launcher(guest)


def wizard_guest(device: Device, _log: pathlib.Path) -> None:
    before = check_unprovisioned(device)
    complete_wizard(device)
    check_provisioned(device, by_wizard=True, before=before)


def helper_guest(device: Device, log: pathlib.Path) -> None:
    # The launcher provisions right after sys.boot_completed; the home screen
    # follows within seconds.
    wait_for("the launcher's provisioning", lambda: "Guest provisioned." in log.read_text(errors="replace"), 120)
    check_provisioned(device, by_wizard=False)
    if android_provision.provision(lambda command: device.shell(command)):
        raise Failure("the helper changed a guest that was already provisioned")
    print("helper: nothing left to do on a provisioned guest")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--release", type=pathlib.Path, help="release directory (default: the accepted release)")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--emulator", type=pathlib.Path, help="the Android macOS executable (the launcher's default)")
    parser.add_argument("--adb-port", type=int, default=slot_ports.slot_port(slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_ADB_PORT})")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--width", type=int, help="guest display width in pixels (the launcher's default)")
    parser.add_argument("--height", type=int, help="guest display height in pixels (the launcher's default)")
    parser.add_argument("--dpi", type=int, help="guest display density: with the size it picks the launcher's grid")
    parser.add_argument("--boot-timeout", type=int, default=900)
    parser.add_argument("--only", choices=("wizard", "helper"), help="run one of the two guests")
    return parser


def main() -> int:
    options = build_parser().parse_args()

    adb = shutil.which(options.adb) or options.adb
    with tempfile.TemporaryDirectory(prefix="android-oobe-") as scratch_name:
        scratch = pathlib.Path(scratch_name)
        environment = dict(os.environ)
        try:
            if options.only != "helper":
                run_guest(options, scratch, "wizard", adb, environment, [], wizard_guest)
            if options.only != "wizard":
                run_guest(options, scratch, "helper", adb, environment, ["--provision"], helper_guest)
            print("PASS")
            return 0
        except (Failure, subprocess.TimeoutExpired, OSError) as problem:
            print(f"FAIL: {problem}", file=sys.stderr)
            return 1
        finally:
            # The shared server stays up; only this run's guest transport goes.
            subprocess.run([adb, "disconnect", f"127.0.0.1:{options.adb_port}"], env=environment,
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=30, check=False)


if __name__ == "__main__":
    raise SystemExit(main())
