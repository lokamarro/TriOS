#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Capture every screen and state of Andarium from the Simulator preview.

AndariumPreview opens any screen with mock data from a launch argument
(`-screen <name>`, see Preview/AppEntry.swift). This script launches each
screen in the build already installed on a Simulator and saves a labelled PNG
for it, plus a manifest.json that says, per file, the device, orientation,
appearance, screen and what to look at. It installs nothing and rotates
nothing: build and install AndariumPreview first, rotate an iPad (Simulator:
Device > Rotate, or a UI driver) between runs, and the orientation is read
from each image.

    capture-screens.py --out DIR --device <udid> [--device <udid> ...]
    capture-screens.py --list

One run at a time captures into a directory: the runs would share the
screenshots' working files and read and write manifest.json. A second run into
the same --out refuses at once and names the first; the lock is released when
the run ends, however it ends. Name several --device options for one run, or
give each run its own --out.

Capture on a Simulator made for the run (`simctl create`, deleted afterwards),
never one another session or test uses: any other app that comes to the front
spoils a picture. The script keeps a picture only when the Preview reports
(`capture-state.json`, written by `Preview/CaptureState.swift`) that it is the
launched process, on the shot's screen, and active, both before and after the
screenshot, and the frame is neither blank nor black; a shot that fails is
reported and not recorded.

A shot with `-systemMicrophone YES` runs the Microphone switch against the
Simulator's own permission: the script sets it first with `simctl privacy`
(grant, revoke or reset), and the revoked-while-backgrounded shot takes the
permission away while the app is in the background. The Simulator keeps the
system's permission alert up, over whatever launches next, until SpringBoard is
restarted, and a restart turns an iPad upright. A launch under such an alert
leaves the app inactive, which the check above refuses, so the one shot that
raises the alert runs last, and SpringBoard is restarted once after it: rotate
the iPad again before the next run.

Light mode is captured for every screen; dark mode only for the screens flagged
in the table, where the design adapts to it. Every Simulator command has a hard
time bound. A rerun replaces the files and manifest entries it captures and
keeps the rest.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import contextlib
import fcntl
import json
import os
import re
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

BUNDLE = "com.imxieyi.AndariumPreview"
COMMAND_SECONDS = 60
SETTLE_SECONDS = 15
SETTLED_FRACTION = 0.003
# How long the Preview has, after a launch, to report that it is the app in front.
FRONT_SECONDS = 10
STATE_FILE = "capture-state.json"
LOCK_FILE = ".capture.lock"


@dataclass(frozen=True)
class Shot:
    id: str
    group: str
    screen: str
    description: str
    args: tuple[str, ...] = ()
    dark: bool = False
    wait: float = 3.0
    # The Simulator's microphone permission before the launch: "grant", "revoke" or "reset" (not asked).
    privacy: str | None = None
    # Steps after the launch, for a state that needs the app in the background: "background", "grant", "revoke",
    # "foreground".
    flow: tuple[str, ...] = ()
    # The shot raises the system's own alert, which stays up over whatever launches next: it runs last, and
    # SpringBoard is restarted after it.
    alert: bool = False

    def launch_arguments(self) -> list[str]:
        return ["-screen", self.screen, *self.args]


def s(id: str, group: str, screen: str, description: str, *args: str, dark: bool = False, wait: float = 3.0) -> Shot:
    return Shot(id, group, screen, description, tuple(args), dark, wait)


SYSTEM = ("-systemMicrophone", "YES")


def system_mic(id: str, group: str, screen: str, description: str, privacy: str, *flow: str) -> Shot:
    """A shot with the Microphone switch on the Simulator's own permission, set to `privacy` first."""
    return Shot(id, group, screen, description, SYSTEM, privacy=privacy, flow=tuple(flow), alert=privacy == "reset")


def boot_stages(prefix: str, screen: str, names: list[str], what: str, dark_step: int | None = None) -> list[Shot]:
    return [
        s(
            f"{prefix}-{index}-{name.lower().replace(' ', '-')}",
            "Boot",
            screen,
            f"{what}, step {index + 1} of {len(names)} ({name}) under way.",
            "-bootStage",
            str(index),
            dark=index == dark_step,
        )
        for index, name in enumerate(names)
    ]


SHOTS: list[Shot] = [
    # Home
    s("home", "Home", "home", "Home with a configured guest: Guest system (two matched rows: CPU and memory with Edit, then Storage with its usage bar and Resize), Display, Capabilities, Backup, Recovery and erase, and the Start bar.", dark=True),
    s("home-welcome", "Home", "welcome", "Home before any guest exists: the welcome and Set up guest. First screen of the initial setup."),
    s("home-first-start", "Home", "home-new", "A guest just created and not yet started: Home as it is before the first start."),
    s("home-display", "Home", "home-display", "Home scrolled to Display: the Retina mode toggle.", dark=True),
    s("home-capabilities", "Home", "home-capabilities", "Home scrolled to Capabilities: Microphone off and Gyroscope on (its default), no notes.", dark=True),
    s("home-capabilities-mic-on", "Home", "home-mic-on", "Capabilities with Microphone on, iOS having allowed it."),
    s("home-capabilities-mic-denied", "Home", "home-mic-denied", "Capabilities after iOS refused the microphone or its app permission was removed: the row is dimmed and says it is not allowed in Settings.", dark=True),
    s("home-capabilities-mic-alert", "Home", "home-mic-alert", "Turning Microphone on while iOS refuses it: the alert that offers Settings."),
    s("home-capabilities-gyroscope-unavailable", "Home", "home-gyroscope-unavailable", "Capabilities on a device without a gyroscope or accelerometer: Gyroscope is disabled and the footnote says why."),
    s("home-backup", "Home", "home-backup", "Home scrolled to Backup: the guest-data backup toggle and a one-line note."),
    s("home-first-start-recovery", "Home", "home-new-recovery", "Home scrolled to Recovery and erase on a guest not yet started: Start in recovery is disabled until the first start.", dark=True),
    s("home-recovery-erase", "Home", "home-recovery-erase", "Home scrolled to Recovery and erase: Start in recovery and Erase guest."),
    s("home-storage-grow", "Home", "home-grow", "Guest system with a larger storage size chosen: the Storage row's bar outlines the new size and the note under it says the growth takes effect at the next start.", dark=True),
    s("home-storage-max", "Home", "home-max", "Guest system with storage at its largest size (128 GiB, in 4 GiB steps): the note under the Storage bar says so and Resize is dimmed.", dark=True),
    s("home-update", "Home", "home-update", "A system update waiting: the card above the Start bar."),
    s("home-update-unfinished", "Home", "home-update-failed", "An update that did not finish: the card offers retry or restoring the saved data."),
    s("home-update-unfinished-restore-only", "Home", "home-update-failed-restore", "An unfinished update that can only be restored."),
    s("home-blocked", "Home", "home-blocked", "Start blocked because the guest is newer than the system in this app."),
    s("home-notice-closed", "Home", "home-closed", "The notice shown after iOS closed Andarium while it was paused."),
    s("home-notice-ended", "Home", "home-ended", "The notice shown at the launch after Android's previous session ended badly, here in a kernel panic."),
    s("home-notice-low-space", "Home", "home-low-space", "The low free space notice on Home."),
    s("home-notice-memory-high", "Home", "home-memory-warning", "A guest with more memory than a new one gets (the most the device offers): the notice above Guest system says iOS is likely to close Andarium and what is safer.", dark=True),
    s("busy-create", "Operations", "busy-create", "The overlay while the guest is created: title, detail and progress."),
    s("busy-erase", "Operations", "busy-erase", "The overlay while the guest is erased."),
    s("alert-error", "Alerts", "alert-error", "An error alert (an operation that failed with the system's message)."),
    # Sheets and dialogs
    s("setup-new-guest", "Setup", "create", "The initial setup sheet: Storage and Backup only; vCPUs and memory are no longer asked.", dark=True),
    s("setup-new-guest-low-space", "Setup", "create-low-space", "The setup sheet when the device lacks the free space the first start needs: the notice and a disabled Create."),
    s("start-low-space-alert", "Setup", "start-low-space", "Start pressed without enough free space: the alert."),
    s("storage-resize", "Sheets", "resize", "The Resize sheet for the guest's storage."),
    s("system-edit", "Sheets", "system-edit", "The System sheet, opened from Edit: vCPUs and memory with the device's limits, and beneath them the estimated VRAM (graphics memory) with the note that it is separate from the guest's RAM. A guest at the default memory shows no warning.", dark=True),
    s("system-edit-memory-high", "Sheets", "system-memory-warning", "The System sheet with memory above what a new guest gets (the most the device offers): the ticks past the default are orange, the line beneath the ruler says iOS is likely to close Andarium above it and the VRAM estimate is orange.", dark=True),
    s("system-edit-vram-warning", "Sheets", "system-edit", "The System sheet on a device whose default is held below the line where iOS is likely to close Andarium (an iPhone 18 Pro Max mock) with memory between the two: the ticks past the default are orange and beyond the closing size red, the line beneath says games may run out of graphics memory and the VRAM estimate is orange. Home shows no notice at this size.", "-device", "iphone-18-pro-max", "-memoryMiB", "3584", dark=True),
    s("system-edit-ipad-mini", "Sheets", "system-edit", "The System sheet on an iPad mini 6 mock at its default memory (1.5 GiB): the two sizes coincide, so no graphics warning shows and the VRAM estimate is plain.", "-device", "ipad-mini-6"),
    s("erase-alert", "Alerts", "erase", "Erase guest: the standard destructive confirmation over Home. Confirming returns to the first-launch screen (home-welcome).", dark=True),
    s("info", "Info", "info", "Info: the app and version, System info (Build, Android version, Security patch, Kernel, Architecture, Supported architectures), Project on GitHub and Acknowledgements.", dark=True),
    s("info-acknowledgements", "Info", "info-acknowledgements", "Acknowledgements: projects by group; the names and marks line is at the bottom.", dark=True),
    s("info-license", "Info", "info-license", "A project's page: copyright holders, the license name and the license text (Mesa, MIT).", dark=True),
    s("info-license-long", "Info", "info-license-gpl", "A page with a long license text (Linux: syscall exception and GPL v2)."),
    # Boot
    *boot_stages("boot", "boot", ["System image", "Linux kernel", "Storage", "Startup"], "Normal boot over the dimmed guest display, no progress bar"),
    s("boot-single-vcpu", "Boot", "boot-single-vcpu", "Normal boot of a guest with a single vCPU: the Linux kernel line says \"1 vCPU\"."),
    s("boot-first-start", "Boot", "boot-first", "First start: Storage prepares the disk and Startup sets up for the first time."),
    s("boot-update", "Boot", "boot-upgrade", "The first start after an update: Startup updates the apps."),
    s("boot-restart", "Boot", "boot-restart", "The guest restarting."),
    *boot_stages("boot-recovery", "boot-recovery", ["System image", "Linux kernel", "Storage", "Recovery"], "Recovery boot (opaque, own stage list, no progress bar)", dark_step=3),
    # Running guest
    s("session", "Guest", "session", "The running guest with the side keys closed."),
    s("session-tip", "Guest", "session-tip", "The side-keys tip over the running guest.", "-resetTips", "YES"),
    s("session-side-keys", "Guest", "session-open", "The side keys open: Power, volume, Back, Home and Settings."),
    s("session-left-edge", "Guest", "session-left", "The side keys on the left edge."),
    s("session-volume", "Guest", "session-volume", "The volume panel after a volume key press."),
    s("session-power-menu", "Guest", "session-power", "Android's power menu."),
    s("session-keyboard", "Guest", "session-keyboard", "The guest's own keyboard up over its search."),
    s("session-stats", "Guest", "session-stats", "The CPU, memory, VRAM and frame rate readout drawn over the guest: memory and VRAM in use over bars of their share.", dark=True),
    s("session-stats-fresh", "Guest", "session-stats", "The readout before the session's first reading: every figure a dash, the CPU bars all dimmed floor bars and the memory bars empty tracks.", "-statsReadings", "0"),
    s("session-stats-vram-high", "Guest", "session-stats", "The readout with VRAM at 93% of its budget: its bar is orange.", "-statsReadings", "10", "-statsVramMiB", "498"),
    s("session-stats-short", "Guest", "session-stats", "The readout after three CPU readings: the bars fill from the right, the older slots stay dimmed floor bars.", "-statsReadings", "3"),
    s("session-graphics-notice", "Guest", "session-graphics-notice", "The notice over the running guest that Android ran out of graphics memory."),
    s("session-setup", "Guest", "session-setup", "The guest's own first-run setup screen."),
    s("session-off", "Guest", "session-off", "The guest's screen after it powered off."),
    s("space-pause", "Guest", "space-pause", "The running guest paused because the device is nearly out of space: the screen over it with Resume and Shut down.", dark=True),
    s("space-pause-short", "Guest", "space-pause-short", "The same screen after Resume found the space still short: it states what is free now."),
    s("space-pause-pausing", "Guest", "space-pause-pausing", "The same screen while the guest is being paused: it offers nothing until the guest holds still."),
    s("space-pause-running", "Guest", "space-pause-running", "The guest did not hold still: the screen says it still runs and offers Shut down and Check again."),
    s("space-pause-shutting-down", "Guest", "space-pause-shutting-down", "The same screen once Shut down was chosen: the buttons give way to Shutting down."),
    s("recovery", "Guest", "recovery", "Recovery running."),
    s("recovery-side-keys", "Guest", "recovery-open", "Recovery with the side keys open."),
    # Settings during a session
    s("settings", "Settings", "settings", "Settings at full height: Stats, Apps, Developer and Diagnostics.", dark=True),
    s("settings-stats", "Settings", "settings-stats", "Settings scrolled to Stats: CPU, Memory and VRAM with bars of their share, and Frame rate.", dark=True),
    s("settings-stats-short", "Settings", "settings-stats", "Settings scrolled to Stats after three CPU readings: the bars fill from the right.", "-statsReadings", "3"),
    system_mic("home-mic-system-prompt", "Capabilities", "home-mic-alert", "The system's own question when Microphone is turned on for the first time (the Simulator's alert).", "reset"),
    system_mic("home-mic-system-allowed", "Capabilities", "home-mic-on", "Microphone on after iOS allowed it.", "grant"),
    system_mic("home-mic-system-refused", "Capabilities", "home-capabilities", "Microphone after iOS refused it: the row is dimmed and says it is not allowed in Settings.", "revoke"),
    system_mic("home-mic-system-settings-prompt", "Capabilities", "home-mic-alert", "Turning Microphone on after a refusal: Andarium explains and offers Settings.", "revoke"),
    system_mic("home-mic-system-revoked", "Capabilities", "home-mic-on", "Microphone on, access taken away in Settings while Andarium was in the background, Andarium active again: the switch is off and dimmed.", "grant", "background", "revoke", "foreground"),
    s("settings-adb", "Settings", "settings-adb", "Developer: ADB over network on, the address a computer connects to."),
    s("settings-adb-no-address", "Settings", "settings-adb-no-address", "Developer: ADB over network on with no network address."),
    s("settings-adb-off", "Settings", "settings-adb-off", "Developer: ADB over network unavailable because USB debugging is off."),
    # Operations on the guest
    s("overlay-bug-report", "Operations", "bug-report", "The full-screen progress while a bug report is made.", wait=2.0),
    s("overlay-install-apk", "Operations", "install-apk", "The full-screen progress while an app installs from Files.", wait=2.0),
    s("overlay-install-done", "Operations", "install-done", "The install overlay at its end: App installed with Done. It stays until closed."),
    s("overlay-install-failed", "Operations", "install-failed", "The install overlay at its end: App not installed, the guest's reason and OK."),
    s("overlay-install-not-apk", "Operations", "install-not-apk", "A file that is not an app, refused at once on the overlay."),
    s("overlay-install-bundle", "Operations", "install-bundle", "An app in several files, refused at once on the overlay."),
    # End: a clean power-off returns to Home; only a failed run keeps an end screen
    s("end-failed", "End", "failed", "The screen after the guest stopped unexpectedly."),
]


def run(command: list[str], *, timeout: float = COMMAND_SECONDS) -> subprocess.CompletedProcess[str]:
    """Run a command with a hard time bound; a command that outlives it fails."""
    try:
        return subprocess.run(
            command, check=False, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=timeout
        )
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(command, 124, "", f"timed out after {timeout:.0f} s")


def simctl(*arguments: str, timeout: float = COMMAND_SECONDS) -> subprocess.CompletedProcess[str]:
    return run(["xcrun", "simctl", *arguments], timeout=timeout)


def device_names() -> dict[str, str]:
    result = simctl("list", "devices", "--json")
    if result.returncode != 0:
        sys.exit(f"capture-screens.py: simctl list failed: {result.stderr.strip()}")
    names: dict[str, str] = {}
    for devices in json.loads(result.stdout)["devices"].values():
        for device in devices:
            names[device["udid"]] = device["name"]
    return names


def png_size(path: Path) -> tuple[int, int]:
    header = path.read_bytes()[:24]
    return struct.unpack(">II", header[16:24])


def slug(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-")


def differing_pixels(first: Path, second: Path) -> float:
    """The fraction of pixels that differ between two screenshots (1.0 if unknown)."""
    try:
        result = run(["magick", "compare", "-metric", "AE", "-fuzz", "5%", str(first), str(second), "null:"], timeout=30)
        width, height = png_size(first)
        return float(result.stderr.split()[0]) / (width * height)
    except (OSError, ValueError, IndexError, subprocess.TimeoutExpired):
        return 1.0


def empty_frame(path: Path) -> str | None:
    """"white" for a plain white frame (an app that has not drawn yet), "black" for a plain black one (a display
    that is off or an app that went away), None for anything with content."""
    try:
        result = run(["magick", str(path), "-format", "%[fx:mean] %[fx:standard_deviation]", "info:"], timeout=30)
        mean, deviation = (float(value) for value in result.stdout.split())
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None
    if deviation >= 0.02:
        return None
    if mean > 0.97:
        return "white"
    return "black" if mean < 0.03 else None


def app_state(udid: str) -> dict | None:
    """What the Preview last wrote to its capture-state.json: its process, screen and whether it is active."""
    container = simctl("get_app_container", udid, BUNDLE, "data")
    if container.returncode != 0:
        return None
    try:
        return json.loads((Path(container.stdout.strip()) / "Library" / "Caches" / STATE_FILE).read_text())
    except (OSError, ValueError):
        return None


def alive(pid: int) -> bool:
    """A Simulator app is a process of the host's own."""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def not_in_front(udid: str, pid: int, shot: Shot, *, after: bool = False) -> str | None:
    """Why the app is not showing `shot`: None when it is the launched process, on the shot's screen and active;
    otherwise the reason. An app launched while a system alert is up stays inactive, so a stale alert shows here.
    The alert a shot raises itself leaves the app active; if it does not, `after` the screenshot it is no reason."""
    if not alive(pid):
        return f"the app (process {pid}) is not running"
    state = app_state(udid)
    if state is None:
        return "the app has not reported its state"
    if state.get("pid") != pid:
        return f"the app that reported is process {state.get('pid')}, not the launched {pid}"
    if state.get("screen") != shot.screen:
        return f"the app reported screen {state.get('screen')!r}, not {shot.screen!r}"
    if state.get("state") != "active" and not (after and shot.alert):
        return f"the app is {state.get('state')}, not active: a system alert, another app or the Home screen is in front"
    return None


def wait_in_front(udid: str, pid: int, shot: Shot, seconds: float = FRONT_SECONDS) -> str | None:
    """`not_in_front`, given a few seconds after a launch for the app to report."""
    deadline = time.monotonic() + seconds
    while True:
        reason = not_in_front(udid, pid, shot)
        if reason is None or time.monotonic() >= deadline:
            return reason
        time.sleep(0.5)


def settled_screenshot(udid: str, shot_id: str, out: Path) -> tuple[Path | None, bool]:
    """Screenshot once the screen has stopped changing (a launch animation or a
    scroll is over): two looks a second apart that differ in under 0.3% of
    their pixels. After SETTLE_SECONDS the last look is taken as it is."""
    temporary = out / f".{shot_id}.png"
    previous = out / f".{shot_id}.previous.png"
    deadline = time.monotonic() + SETTLE_SECONDS
    settled = False
    while True:
        taken = simctl("io", udid, "screenshot", str(temporary))
        if taken.returncode != 0 or not temporary.is_file():
            print(f"  {shot_id}: screenshot failed: {taken.stderr.strip()}", file=sys.stderr)
            return None, False
        if previous.is_file() and differing_pixels(previous, temporary) < SETTLED_FRACTION and empty_frame(temporary) is None:
            settled = True
            break
        if time.monotonic() >= deadline:
            break
        temporary.replace(previous)
        time.sleep(1.0)
    previous.unlink(missing_ok=True)
    return temporary, settled


def play_flow(udid: str, shot: Shot, pid: int) -> int | None:
    """The steps that follow the launch of a shot whose state needs the app in the background; the process that
    ends them, or None. The Simulator ends an app whose permission changes, as iOS does, so the last step is a
    relaunch there; an app still running just comes forward, which is the activation."""
    for step in shot.flow:
        if step == "background":
            result = simctl("launch", udid, "com.apple.Preferences")
        elif step in ("grant", "revoke"):
            result = simctl("privacy", udid, step, "microphone", BUNDLE)
        elif step == "foreground":
            result = simctl("launch", udid, BUNDLE, *shot.launch_arguments())
            if result.returncode == 0:
                again = launched_pid(result)
                print(f"  {shot.id}: {'the app was still running' if again == pid else 'the permission change ended the app'}")
                pid = again if again is not None else pid
        else:
            sys.exit(f"capture-screens.py: unknown flow step {step!r}")
        if result.returncode != 0:
            print(f"  {shot.id}: {step} failed: {result.stderr.strip()}", file=sys.stderr)
            return None
        time.sleep(2.0)
    return pid


def launched_pid(launched: subprocess.CompletedProcess[str]) -> int | None:
    """The process `simctl launch` started: its output is "<bundle>: <pid>"."""
    try:
        return int(launched.stdout.strip().rsplit(" ", 1)[-1])
    except ValueError:
        return None


def clear_system_alert(udid: str) -> None:
    """The Simulator keeps its permission alert up, over whatever launches next, until someone answers it.
    Restarting SpringBoard takes the alert and the app with it, and turns an iPad upright."""
    simctl("spawn", udid, "launchctl", "kill", "SIGKILL", "user/foreground/com.apple.SpringBoard")
    time.sleep(15.0)


def capture(udid: str, name: str, shot: Shot, appearance: str, out: Path, wait: float, orientation: str | None) -> dict | None:
    """One shot, or None (with the reason on stderr) when the picture cannot be Andarium's: the app is not the
    launched process on the shot's screen in front, the frame is empty, or the device turned since the run began
    (`orientation`)."""
    simctl("terminate", udid, BUNDLE)
    if shot.privacy is not None:
        privacy = simctl("privacy", udid, shot.privacy, "microphone", BUNDLE)
        if privacy.returncode != 0:
            print(f"  {shot.id}: privacy {shot.privacy} failed: {privacy.stderr.strip()}", file=sys.stderr)
            return None
    launched = simctl("launch", udid, BUNDLE, *shot.launch_arguments())
    pid = launched_pid(launched)
    if launched.returncode != 0 or pid is None:
        print(f"  {shot.id}: launch failed: {launched.stderr.strip()}", file=sys.stderr)
        return None
    time.sleep(max(wait, shot.wait))
    if shot.flow:
        pid = play_flow(udid, shot, pid)
        if pid is None:
            return None
    reason = wait_in_front(udid, pid, shot)
    if reason is not None:
        print(f"  {shot.id}: not recorded: {reason}", file=sys.stderr)
        return None
    temporary, settled = settled_screenshot(udid, shot.id, out)
    if temporary is None:
        return None
    reason = not_in_front(udid, pid, shot, after=True) or (f"the frame is {empty_frame(temporary)}" if empty_frame(temporary) else None)
    if reason is None:
        width, height = png_size(temporary)
        now = "landscape" if width > height else "portrait"
        if orientation is not None and now != orientation:
            reason = f"the device is {now}, not {orientation} as at the start of the run"
    if reason is not None:
        temporary.unlink(missing_ok=True)
        print(f"  {shot.id}: not recorded: {reason}", file=sys.stderr)
        return None
    if not settled:
        print(f"  {shot.id}: the screen kept changing; kept the last look", file=sys.stderr)
    idiom = "iPad" if "iPad" in name else "iPhone"
    file = f"{idiom.lower()}-{now}-{appearance}-{shot.id}.png"
    temporary.replace(out / file)
    return {
        "file": file,
        "device": name,
        "idiom": idiom,
        "orientation": now,
        "appearance": appearance,
        "group": shot.group,
        "id": shot.id,
        "screen": shot.screen,
        "launch_arguments": " ".join(shot.launch_arguments()),
        "pixels": [width, height],
        "settled": settled,
        "description": shot.description,
    }


def preflight(udid: str, name: str, out: Path) -> str | None:
    """Before the first shot: the Preview must come to the front on a plain screen. A permission alert left up by
    an earlier use of the Simulator is cleared (which turns an iPad upright, as is said). The result is the
    orientation the run keeps, or None when the Preview cannot be brought to the front."""
    probe = Shot("preflight", "", "home", "")
    for attempt in range(2):
        simctl("terminate", udid, BUNDLE)
        launched = simctl("launch", udid, BUNDLE, *probe.launch_arguments())
        pid = launched_pid(launched)
        if pid is not None and wait_in_front(udid, pid, probe) is None:
            temporary = out / ".preflight.png"
            if simctl("io", udid, "screenshot", str(temporary)).returncode == 0 and temporary.is_file():
                width, height = png_size(temporary)
                temporary.unlink()
                return "landscape" if width > height else "portrait"
        if attempt == 0:
            print(f"{name}: the Preview did not come to the front; restarting SpringBoard to clear an alert", file=sys.stderr)
            clear_system_alert(udid)
    return None


class OutputBusy(Exception):
    """Another run holds the output directory."""


@contextlib.contextmanager
def output_lock(out: Path):
    """Hold the output directory for this run, or raise OutputBusy at once.

    The lock is an exclusive flock on LOCK_FILE in the directory: the kernel drops it when the process ends
    however it ends, so a killed run never leaves the directory locked. The file records the holder's pid for the
    message of a refused run and stays in the directory.
    """
    descriptor = os.open(out / LOCK_FILE, os.O_RDWR | os.O_CREAT, 0o644)
    try:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            holder = os.pread(descriptor, 32, 0).decode(errors="replace").strip()
            raise OutputBusy(holder or "unknown") from None
        os.ftruncate(descriptor, 0)
        os.pwrite(descriptor, f"{os.getpid()}\n".encode(), 0)
        yield
    finally:
        os.close(descriptor)


def write_manifest(path: Path, entries: dict[str, dict]) -> None:
    manifest = {"schema": 1, "shots": sorted(entries.values(), key=lambda entry: entry["file"])}
    path.write_text(json.dumps(manifest, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, help="directory for the PNG files and manifest.json")
    parser.add_argument("--device", action="append", default=[], metavar="UDID", help="a booted Simulator (repeatable)")
    parser.add_argument("--only", metavar="REGEX", help="capture only shots whose id matches")
    parser.add_argument("--appearance", choices=("light", "dark"), help="capture only this appearance")
    parser.add_argument("--wait", type=float, default=0.0, help="minimum seconds after each launch")
    parser.add_argument("--list", action="store_true", help="list the shots and exit")
    args = parser.parse_args(argv)
    if args.list:
        for shot in SHOTS:
            print(f"{shot.id}\t{shot.group}\t{' '.join(shot.launch_arguments())}\t{'light,dark' if shot.dark else 'light'}")
        return 0
    if args.out is None or not args.device:
        parser.error("--out and at least one --device are required")
    ids = [shot.id for shot in SHOTS]
    if len(ids) != len(set(ids)):
        sys.exit("capture-screens.py: duplicate shot ids")
    args.out.mkdir(parents=True, exist_ok=True)
    try:
        with output_lock(args.out):
            return capture_all(args)
    except OutputBusy as busy:
        print(
            f"capture-screens.py: another capture is running into {args.out} (pid {busy}); a directory takes one run "
            "at a time, so wait for it, name several --device options in one run, or use another --out",
            file=sys.stderr,
        )
        return 1


def capture_all(args: argparse.Namespace) -> int:
    manifest_path = args.out / "manifest.json"
    entries: dict[str, dict] = {}
    if manifest_path.is_file():
        entries = {entry["file"]: entry for entry in json.loads(manifest_path.read_text())["shots"]}
    names = device_names()
    selected = [shot for shot in SHOTS if not args.only or re.search(args.only, shot.id)]
    status = 0
    for udid in args.device:
        name = names.get(udid)
        if name is None:
            sys.exit(f"capture-screens.py: no Simulator {udid}")
        simctl(
            "status_bar", udid, "override", "--time", "9:41", "--batteryState", "charged", "--batteryLevel", "100",
            "--cellularMode", "active", "--cellularBars", "4", "--wifiBars", "3", "--dataNetwork", "wifi",
        )
        orientation = preflight(udid, name, args.out)
        if orientation is None:
            print(f"capture-screens.py: {name} ({udid}): the Preview does not come to the front; nothing captured", file=sys.stderr)
            status = 1
            continue
        # The shot of the system's own alert runs last: the alert outlives its shot and covers whatever launches next.
        plain = [shot for shot in selected if not shot.alert]
        passes = [("light", plain), ("dark", [shot for shot in plain if shot.dark]),
                  ("light", [shot for shot in selected if shot.alert])]
        passes = [(appearance, shots) for appearance, shots in passes if args.appearance in (None, appearance)]
        asked = False
        for appearance, shots in passes:
            simctl("ui", udid, "appearance", appearance)
            for shot in shots:
                entry = capture(udid, name, shot, appearance, args.out, args.wait, orientation)
                if entry is None:
                    entry = capture(udid, name, shot, appearance, args.out, args.wait, orientation)
                asked = asked or shot.alert
                if entry is None:
                    status = 1
                    continue
                entries[entry["file"]] = entry
                write_manifest(manifest_path, entries)
                print(f"{name} {entry['orientation']} {appearance}: {shot.id}", flush=True)
        simctl("ui", udid, "appearance", "light")
        if asked:
            clear_system_alert(udid)
    write_manifest(manifest_path, entries)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
