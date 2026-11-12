#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stand-ins for xcrun devicectl, iproxy, adb and codesign.

The tooling tests put one executable wrapper per tool on PATH; each runs
``fake_device_tools.py TOOL ARGUMENTS...``. All state lives in the directory
named by ``FAKE_STATE``: ``documents/`` is the application's container (the
runner must leave it as it is), ``commands.jsonl`` records every invocation,
and ``poweroff`` is written by the guest's power-off command and ends the fake
console. ``FAKE_*`` variables choose a failure: ADB (``never-ready``), CONSOLE
(``silent`` after power off), AUTH (``missing`` line), FOCUS (``alert`` or
``stuck``), GUEST (``dies``) and LOCKED. ``FAKE_START_AFTER=TOOL:OTHER``
holds TOOL (``launch`` or ``iproxy``) back until OTHER has recorded itself,
which fixes the order two concurrently started processes appear in
``commands.jsonl``; without it that order is the scheduler's. Nothing here
touches a device.
"""

from __future__ import annotations

import json
import os
import pathlib
import signal
import socket
import sys
import time

STATE = pathlib.Path(os.environ["FAKE_STATE"])
START_AFTER_SECONDS = 20
CHILD = "DEVICECTL_CHILD_"
LOGCAT = (
    "100.100 I ActivityTaskManager: START u0 {{cmp={component}}}\n"
    "100.900 I ActivityTaskManager: Displayed {displayed}\n"
    "300.000 I Test: SCENARIO DONE score=42\n"
)


def record(tool: str, arguments: list[str], **extra: object) -> None:
    entry = {"tool": tool, "args": arguments, **extra}
    with (STATE / "commands.jsonl").open("a") as log:
        log.write(json.dumps(entry) + "\n")


def recorded() -> list[str]:
    log = STATE / "commands.jsonl"
    tools = []
    for line in log.read_text().splitlines() if log.exists() else []:
        try:
            tools.append(json.loads(line)["tool"])
        except ValueError:
            pass
    return tools


def start_after(tool: str, arguments: list[str]) -> None:
    """Honor FAKE_START_AFTER; give up after START_AFTER_SECONDS."""
    launching = (tool == "xcrun"
                 and arguments[1:4] == ["device", "process", "launch"])
    held, _, other = os.environ.get("FAKE_START_AFTER", "").partition(":")
    if held != ("launch" if launching else tool):
        return
    deadline = time.monotonic() + START_AFTER_SECONDS
    while other not in recorded() and time.monotonic() < deadline:
        time.sleep(0.01)


def devicectl(arguments: list[str]) -> int:
    kind = arguments[:3]
    if kind == ["device", "info", "lockState"]:
        locked = os.environ.get("FAKE_LOCKED") == "1"
        print(f"passcodeRequired: {'true' if locked else 'false'}")
    elif kind == ["device", "install", "app"]:
        pass
    elif kind == ["device", "process", "launch"]:
        return launch(arguments)
    else:
        print(f"unexpected devicectl: {arguments}", file=sys.stderr)
        return 99
    return 0


def launch(arguments: list[str]) -> int:
    environment = {
        name[len(CHILD):]: value
        for name, value in os.environ.items() if name.startswith(CHILD)
    }
    record("launch", arguments, environment=environment)
    signal.signal(signal.SIGINT, lambda *_: terminate())
    signal.signal(signal.SIGTERM, lambda *_: terminate())
    print("Andarium thermal state: phase=start state=nominal uptime=2.0",
          flush=True)
    if os.environ.get("FAKE_AUTH") != "missing":
        print("Kernarium: ADB authorization is off for this launch",
              flush=True)
    focus = os.environ.get("FAKE_FOCUS")
    if focus:
        print("Andarium: application inactive", flush=True)
    if focus == "alert":
        time.sleep(1.5)
        print("Andarium: application active", flush=True)
    if os.environ.get("FAKE_GUEST") == "dies":
        print("Andarium: runtime returned 1", flush=True)
        return 0
    while not (STATE / "poweroff").exists():
        time.sleep(0.02)
    if os.environ.get("FAKE_CONSOLE") == "silent":
        while True:
            time.sleep(0.1)
    print("Andarium thermal state: phase=finish state=fair uptime=99.0",
          flush=True)
    print("Andarium: runtime returned 0", flush=True)
    return 0


def terminate() -> None:
    (STATE / "terminated").write_text("")
    raise SystemExit(130)


def iproxy(arguments: list[str]) -> int:
    record("iproxy", arguments)
    if os.environ.get("FAKE_IPROXY") == "fail":
        print("no device found", file=sys.stderr)
        return 1
    port = int(arguments[-1].split(":")[0])
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", port))
        listener.listen(4)
        while True:
            time.sleep(0.1)


def adb(arguments: list[str]) -> int:
    record("adb", arguments,
           server=os.environ.get("ANDROID_ADB_SERVER_PORT"))
    if arguments[:1] == ["-s"]:
        arguments = arguments[2:]
    never = os.environ.get("FAKE_ADB") == "never-ready"
    command = arguments[0]
    if command == "connect":
        print(f"{'failed to connect' if never else 'connected'} to "
              f"{arguments[1]}")
    elif command == "get-state":
        if never:
            print("error: device offline", file=sys.stderr)
            return 1
        print("device")
    elif command == "push":
        source = pathlib.Path(arguments[1])
        if os.environ.get("FAKE_ADB_PUSH") == "fail":
            print("adb: error: failed to copy " + source.name,
                  file=sys.stderr)
            return 1
        (STATE / "pushed").mkdir(exist_ok=True)
        (STATE / "pushed" / pathlib.PurePosixPath(arguments[2]).name
         ).write_bytes(source.read_bytes())
    elif command == "logcat":
        if "-d" in arguments:
            components = (STATE / "launched").read_text().split() \
                if (STATE / "launched").exists() else []
            for component in components:
                package = component.split("/", 1)[0]
                sys.stdout.write(LOGCAT.format(
                    component=component, displayed=f"{package}/.Main"))
    elif command == "shell":
        return shell(arguments[1])
    return 0


def fresh_guest() -> bool:
    """FAKE_FRESH_GUEST: an erased guest, in Setup Wizard until provisioned."""
    return os.environ.get("FAKE_FRESH_GUEST") == "1"


def wizard_disabled() -> bool:
    # FAKE_PROVISION=ignored: a guest that takes no change from the helper.
    return (STATE / "wizard-disabled").exists() \
        and os.environ.get("FAKE_PROVISION") != "ignored"


def wizard_running() -> bool:
    """The wizard's process lives on a fresh guest until something force-stops it."""
    return fresh_guest() and not (STATE / "wizard-stopped").exists()


STATUS_BAR = """\
    mDisabled1={disabled}
  mDisableRecords.size=2
{wizard}    [0] userId=0 what1=0x00000000 what2=0x00000000 pkg=com.android.systemui token=android.os.BinderProxy@d59b6af
"""
WIZARD_BAR_RECORD = ("    [1] userId=0 what1=0x03A50000 what2=0x00000000 "
                     "pkg=org.lineageos.setupwizard token=android.os.BinderProxy@2065e8e\n")


def settings(words: list[str]) -> None:
    """`settings get|put <namespace> <name> [value]`, 0 until put on a fresh guest."""
    key = STATE / ("setting-" + "-".join(words[2:4]))
    if words[1] == "put":
        if os.environ.get("FAKE_PROVISION") != "ignored":
            key.write_text(words[4])
    elif key.exists():
        print(key.read_text())
    else:
        print("0" if fresh_guest() else "1")


def shell(text: str) -> int:
    if text == "getprop sys.boot_completed":
        polls = STATE / "boot-polls"
        count = int(polls.read_text()) + 1 if polls.exists() else 1
        polls.write_text(str(count))
        print("1" if count > 2 else "0")
    elif text == "echo ready":
        print("ready")
    elif text == "pidof adbd":
        print("500")
    elif text.startswith("settings "):
        settings(text.split())
    elif text.startswith("pm disable-user --user 0 "):
        (STATE / "wizard-disabled").write_text(text)
    elif text == "am force-stop org.lineageos.setupwizard":
        # The wizard's process holds a status bar disable record until it dies.
        (STATE / "wizard-stopped").write_text(text)
    elif text == "am get-started-user-state 0":
        # Provisioning starts on a guest whose user 0 is unlocked; the wizard
        # of a fresh one is HOME and holds its status bar record until stopped.
        print("RUNNING_UNLOCKED")
    elif text == "pidof org.lineageos.setupwizard":
        if wizard_running():
            print("1445")
    elif text == "dumpsys statusbar":
        print(STATUS_BAR.format(disabled="0x3a50000" if wizard_running() else "0x0",
                                wizard=WIZARD_BAR_RECORD if wizard_running() else ""))
    elif text.startswith("cmd package resolve-activity --brief"):
        print("priority=0 preferredOrder=0 match=0x108000 specificIndex=-1")
        print("org.lineageos.setupwizard/.SetupWizardActivity"
              if fresh_guest() and not wizard_disabled()
              else "com.android.launcher3/.uioverrides.QuickstepLauncher")
    elif text in ("reboot -p", "svc power shutdown"):
        (STATE / "poweroff").write_text(text)
    elif text.startswith("sh /data/local/tmp/"):
        print("workload ran")
        print(f"ANDARIUM_SCRIPT_EXIT:{os.environ.get('FAKE_SCRIPT_EXIT', 0)}")
    elif text.startswith("am start -W -n "):
        with (STATE / "launched").open("a") as launched:
            launched.write(text.split()[-1] + "\n")
        print("Status: ok\nTotalTime: 321")
    elif text == "date +%s.%N":
        print("200.000")
    return 0


def main() -> int:
    tool, arguments = sys.argv[1], sys.argv[2:]
    start_after(tool, arguments)
    if tool == "xcrun":
        assert arguments[0] == "devicectl", arguments
        record("devicectl", arguments[1:])
        return devicectl(arguments[1:])
    if tool == "codesign":
        record("codesign", arguments)
        return 0
    return {"iproxy": iproxy, "adb": adb}[tool](arguments)


if __name__ == "__main__":
    sys.exit(main())
