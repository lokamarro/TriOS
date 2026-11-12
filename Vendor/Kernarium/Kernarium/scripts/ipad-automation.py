#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Drive UI automation on a connected iPad through one long-lived host.

Each XCTest session that turns UI automation on can make the device ask for
its owner's passcode or biometrics. The KernariumAutomation UI-test target
keeps one session alive for hours and executes commands this script places in
its runner's data container over `devicectl`, so one approval at the device
serves every later command. Build the host once, start it, approve the prompt
at the device, and then send commands:

  ipad-automation.py --device UDID build
  ipad-automation.py --device UDID start --hours 8
  ipad-automation.py --device UDID launch --env KERNARIUM_ROOTFS=none.erofs
  ipad-automation.py --device UDID tap --id displayModeButton
  ipad-automation.py --device UDID orientation landscapeLeft
  ipad-automation.py --device UDID --bundle com.apple.springboard drag 500 700 400 600 1
  ipad-automation.py --device UDID screenshot -o shot.png
  ipad-automation.py --device UDID stop --force

Every command has a bounded wait. A command fails with exit status 3 when the
host is not running. UI automation stays approved only while some XCTest
session keeps it on, so a new host normally needs a new approval at the
device: never stop a working host without someone to approve its successor.
Run `start` only while someone can approve, because XCTest gives up 60 s after
the runner starts and leaves the prompt on screen. The host targets
com.imxieyi.Kernarium unless --bundle names another application, such as
com.apple.springboard.

A coordinate tap or drag names points as offsets from the origin of that
application's frame, which `tree` prints; springboard's frame is the whole
screen. A drag presses its start for the given seconds before moving, which is
what picks up a window's resize handle. `orientation` rotates the device and
reports the orientation it reached; without a name it only reports.

No command ends the host: a failed command reports its error, and the host
prints it to its log. If every command starts taking about 60 s, a system
keyboard animation has left XCTest's wait for an idle application stuck;
relaunch the application. The host is not in the Kernarium scheme, so
reinstalling Kernarium or running other tests leaves it serving, and its log
records each such termination as a Kernarium crash.
"""

import argparse
import json
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import time
import uuid

RUNNER = "com.imxieyi.KernariumAutomation.xctrunner"
KERNARIUM = "com.imxieyi.Kernarium"
CHANNEL = "Documents/automation"
TEST = "KernariumAutomation/AutomationHost/testServe"
HEARTBEAT_STALE = 20
EXIT_FAILED = 1
EXIT_NO_HOST = 3
# How long `stop` lets the supervisor end on its own before signalling it.
SUPERVISOR_EXIT_WAIT = 60
SCRIPT = pathlib.Path(__file__).resolve()
CHECKOUT = SCRIPT.parents[2]

# XCTest types a key from the character it produces. On a device it delivers
# nothing for the characters of XCUIKeyboardKey.return ("\r"), .delete
# ("\x7f") and .escape ("\x1b"), or for a backspace ("\b"): no event reaches
# any responder of the application, a plain UITextField included, and the
# command still succeeds. "\n" produces the real Return key (HID usage 0x28),
# modifiers included. Key names therefore resolve to characters that arrive,
# and the characters that would be dropped are refused with a working route.
# The arrows are the iOS values of XCUIKeyboardKey, the arrow symbols; the
# macOS values, U+F700 to U+F703, press nothing on a device.
KEYS = {
    "return": "\n",
    "tab": "\t",
    "space": " ",
    "up": "\u2191",
    "down": "\u2193",
    "left": "\u2190",
    "right": "\u2192",
}
UNDELIVERABLE = {
    "delete": "XCTest drops a synthesized Delete key; `type $'\\x7f'` deletes "
              "through the text input system instead",
    "escape": "XCTest drops a synthesized Escape key; the terminal's key bar "
              "sends one with `tap --label Escape`",
}
# The key each dropped character stands for.
DROPPED = {"\r": "return", "\x7f": "delete", "\b": "delete", "\x1b": "escape"}
# The UIDeviceOrientation cases XCUIDevice can be set to.
ORIENTATIONS = ("portrait", "portraitUpsideDown", "landscapeLeft", "landscapeRight")


class NoHost(Exception):
    pass


def log(message):
    print(f"ipad-automation: {message}", file=sys.stderr, flush=True)


def devicectl(args, device, timeout):
    """Runs one devicectl device subcommand; returns (status, output)."""
    command = ["xcrun", "devicectl", "device", *args, "--device", device]
    try:
        done = subprocess.run(
            command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, f"devicectl timed out after {timeout} s"
    return done.returncode, done.stdout


def container_args(options):
    return ["--domain-type", "appDataContainer",
            "--domain-identifier", options.runner]


def pull(options, source, destination, timeout=60):
    status, _ = devicectl(
        ["copy", "from", "--source", source, "--destination", str(destination),
         *container_args(options)], options.device, timeout)
    return status == 0 and pathlib.Path(destination).exists()


def push(options, sources, destination, timeout=60):
    args = ["copy", "to"]
    for source in sources:
        args += ["--source", str(source)]
    status, output = devicectl(
        [*args, "--destination", destination, *container_args(options)],
        options.device, timeout)
    if status != 0:
        raise NoHost(f"cannot copy into the runner's container: {output.strip()[-400:]}")


def host_state(options):
    """The device-side heartbeat, or None when there is none."""
    with tempfile.TemporaryDirectory() as scratch:
        local = pathlib.Path(scratch) / "state.json"
        if not pull(options, f"{CHANNEL}/state.json", local):
            return None
        try:
            return json.loads(local.read_text())
        except ValueError:
            return None


def liveness(state):
    """Why the host is not serving, or None when it is."""
    if state is None:
        return "no heartbeat in the runner's container"
    if state.get("finished"):
        return "the host has finished"
    age = time.time() - state.get("time", 0)
    if age > HEARTBEAT_STALE:
        return f"the heartbeat is {age:.0f} s old"
    return None


def state_dir(options):
    directory = pathlib.Path(options.state_dir) / options.device
    directory.mkdir(parents=True, exist_ok=True)
    return directory


def local_record(options):
    try:
        return json.loads((state_dir(options) / "host.json").read_text())
    except (OSError, ValueError):
        return {}


def recorded_supervisor(record):
    """The pid `start` recorded, while that process is still its supervisor.

    Nothing clears the record when the host ends, and by then the pid can
    name any process, including another agent's process-group leader. The
    pid is trusted only while its command line still carries `_supervise`
    and the record's nonce."""
    pid = record.get("supervisor")
    nonce = record.get("nonce")
    if not isinstance(pid, int) or pid <= 0 or not nonce:
        return None
    try:
        done = subprocess.run(
            ["ps", "-o", "command=", "-p", str(pid)], stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
            timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return None
    words = done.stdout.split()
    if done.returncode != 0 or "_supervise" not in words:
        return None
    for index, word in enumerate(words[:-1]):
        if word == "--nonce" and words[index + 1] == nonce:
            return pid
    return None


def send(options, command, timeout):
    """Sends one command and waits at most `timeout` s for its result."""
    ident = f"{time.time_ns():020d}-{uuid.uuid4().hex[:8]}"
    command = dict(command, bundle=command.get("bundle") or options.bundle)
    deadline = time.monotonic() + timeout + 30
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        body = scratch / f"{ident}.json"
        marker = scratch / f"{ident}.ready"
        body.write_text(json.dumps(command))
        marker.write_text("")
        push(options, [body, marker], f"{CHANNEL}/inbox")
        result = scratch / "result.json"
        next_check = time.monotonic() + 10
        while time.monotonic() < deadline:
            if pull(options, f"{CHANNEL}/outbox/{ident}.json", result):
                reply = json.loads(result.read_text())
                if "file" in reply:
                    attachment = scratch / reply["file"]
                    if not pull(options, f"{CHANNEL}/outbox/{reply['file']}", attachment, 120):
                        raise NoHost("the host wrote an attachment that cannot be copied")
                    reply["data"] = attachment.read_bytes()
                return reply
            if time.monotonic() >= next_check:
                reason = liveness(host_state(options))
                if reason is not None:
                    raise NoHost(f"the host is not running: {reason}")
                next_check = time.monotonic() + 10
            time.sleep(0.5)
    raise NoHost(f"no result for {command['op']} within {timeout} s")


def command_status(options):
    state = host_state(options)
    reason = liveness(state)
    record = local_record(options)
    summary = {"serving": reason is None}
    if reason is not None:
        summary["reason"] = reason
    if state:
        now = time.time()
        summary.update({
            "heartbeat_age_s": round(now - state.get("time", 0), 1),
            "uptime_s": round(now - state.get("started", now)),
            "remaining_s": round(state.get("deadline", now) - now),
            "served": state.get("served"),
            "busy": state.get("command"),
        })
    if record:
        summary["xcodebuild_log"] = record.get("log")
        summary["supervisor_running"] = recorded_supervisor(record) is not None
    print(json.dumps(summary, indent=2))
    return 0 if reason is None else EXIT_NO_HOST


def derived_data(options):
    return pathlib.Path(options.derived_data)


def command_build(options):
    command = [
        "xcodebuild", "build-for-testing",
        "-project", str(CHECKOUT / "Kernarium/Kernarium.xcodeproj"),
        "-scheme", "KernariumAutomation",
        "-destination", "generic/platform=iOS",
        "-derivedDataPath", str(derived_data(options)),
        "-allowProvisioningUpdates", *options.xcodebuild_arg]
    log("building the automation host")
    try:
        return subprocess.run(command, stdin=subprocess.DEVNULL,
                              timeout=options.timeout).returncode
    except subprocess.TimeoutExpired:
        log(f"the build took longer than {options.timeout} s")
        return EXIT_FAILED


def command_supervise(options):
    """Runs xcodebuild for the host's lifetime; started detached by `start`."""
    environment = dict(os.environ,
                       TEST_RUNNER_KERNARIUM_AUTOMATION_HOST="1",
                       TEST_RUNNER_KERNARIUM_AUTOMATION_HOST_SECONDS=str(options.seconds),
                       TEST_RUNNER_KERNARIUM_AUTOMATION_NONCE=options.nonce)
    command = ["xcodebuild", "test-without-building",
               "-xctestrun", options.xctestrun,
               "-destination", f"platform=iOS,id={options.device}",
               f"-only-testing:{TEST}"]
    # xcodebuild stays in this process group, which `start` and `stop`
    # signal as a whole.
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, env=environment)
    try:
        status = process.wait(timeout=options.seconds + 900)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            status = process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            process.kill()
            status = process.wait(timeout=60)
    print(f"ipad-automation: xcodebuild exited with status {status}", flush=True)
    return status


def command_start(options):
    state = host_state(options)
    if liveness(state) is None:
        log("a host is already serving this device; use it or stop it first")
        return EXIT_FAILED
    runs = sorted(derived_data(options).glob("Build/Products/KernariumAutomation_*.xctestrun"))
    if not runs:
        log(f"no built host under {derived_data(options)}; run `build` first")
        return EXIT_FAILED
    seconds = int(options.hours * 3600)
    nonce = uuid.uuid4().hex
    directory = state_dir(options)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log_path = directory / f"host-{stamp}.log"
    with open(log_path, "w") as output:
        supervisor = subprocess.Popen(
            [sys.executable, str(SCRIPT), "--device", options.device,
             "--runner", options.runner, "--state-dir", options.state_dir,
             "_supervise", "--xctestrun", str(runs[-1]),
             "--seconds", str(seconds), "--nonce", nonce],
            stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
            start_new_session=True)
    (directory / "host.json").write_text(json.dumps({
        "supervisor": supervisor.pid, "log": str(log_path), "nonce": nonce,
        "started": time.time(), "seconds": seconds}))
    log(f"started xcodebuild under supervisor {supervisor.pid}; log {log_path}")
    deadline = time.monotonic() + options.wait
    announced = False
    while time.monotonic() < deadline:
        if supervisor.poll() is not None:
            break
        if not announced and "Running tests..." in log_path.read_text(errors="replace"):
            # XCTest gives up on enabling automation 60 s after this line.
            log("the runner is up: approve Enable UI Automation at the device within 60 s")
            announced = True
        state = host_state(options)
        if state and state.get("nonce") == nonce and liveness(state) is None:
            log("the host is serving")
            return 0
        time.sleep(3)
    text = log_path.read_text(errors="replace")
    if "enabling automation mode" in text:
        log("the device did not enable UI automation: the prompt was not approved "
            "within 60 s, and it stays on the screen until someone dismisses it")
    elif supervisor.poll() is not None:
        log("xcodebuild ended before the host served; see the log")
    else:
        log(f"the host did not serve within {options.wait} s; see the log")
    if supervisor.poll() is None:
        os.killpg(supervisor.pid, signal.SIGTERM)
    return EXIT_NO_HOST


def command_stop(options):
    if not options.force:
        log("refusing to stop: automation stays approved only while a session runs, "
            "so a new host will ask for approval at the device again; pass --force "
            "when someone can approve it")
        return EXIT_FAILED
    record = local_record(options)
    try:
        send(options, {"op": "stop"}, 30)
        log("the host stopped")
    except NoHost as error:
        log(str(error))
    supervisor = recorded_supervisor(record)
    if supervisor is None:
        return 0
    deadline = time.monotonic() + SUPERVISOR_EXIT_WAIT
    while time.monotonic() < deadline:
        if recorded_supervisor(record) is None:
            return 0
        time.sleep(1)
    if recorded_supervisor(record) is not None:
        log(f"terminating supervisor {supervisor}")
        try:
            os.killpg(supervisor, signal.SIGTERM)
        except ProcessLookupError:
            pass
    return 0


def element_fields(options):
    fields = {}
    if options.id:
        fields["id"] = options.id
    if options.label:
        fields["label"] = options.label
    return fields


def key_character(key):
    """The character the host types for a `key` argument: a key name or one
    character. Raises ValueError for a key XCTest would silently drop."""
    key = DROPPED.get(key, key)
    if key in UNDELIVERABLE:
        raise ValueError(UNDELIVERABLE[key])
    if key in KEYS:
        return KEYS[key]
    # XCTest's typeKey raises for anything but one UTF-16 code unit, so a
    # character outside the Basic Multilingual Plane is refused as well.
    if len(key.encode("utf-16-le")) != 2:
        raise ValueError(f"unknown key {key!r}: name one of "
                         f"{', '.join(sorted(KEYS))} or give one character")
    return key


def build_command(options):
    """The host command for an application operation."""
    op = options.command
    command = {"op": op}
    if op == "launch":
        command["environment"] = dict(item.split("=", 1) for item in options.env)
        command["arguments"] = options.arg
    elif op in ("tap", "wait", "exists"):
        command.update(element_fields(options))
        if op == "tap" and options.x is not None:
            command.update(x=options.x, y=options.y)
        if op == "wait":
            command["gone"] = options.gone
        command["timeout"] = options.element_timeout
    elif op in ("type", "siri"):
        command["text"] = options.text
    elif op == "key":
        command["key"] = key_character(options.key)
        command["modifiers"] = [item for item in options.modifiers.split(",") if item]
    elif op == "orientation":
        if options.orientation is not None:
            command["orientation"] = options.orientation
    elif op == "drag":
        if options.seconds < 0 or options.hold < 0:
            raise ValueError("a drag's press and hold durations cannot be negative")
        command.update(x1=options.x1, y1=options.y1, x2=options.x2, y2=options.y2,
                       press=options.seconds, hold=options.hold)
        if options.velocity is not None:
            if options.velocity <= 0:
                raise ValueError("a drag's velocity is a positive number of points per second")
            command["velocity"] = options.velocity
    return command


def run_op(options):
    op = options.command
    try:
        command = build_command(options)
    except ValueError as error:
        log(str(error))
        return EXIT_FAILED
    reply = send(options, command, options.timeout)
    data = reply.pop("data", None)
    if op == "screenshot" and data is not None:
        pathlib.Path(options.output).write_bytes(data)
        reply["file"] = options.output
    if op == "tree" and reply.get("ok"):
        print(reply.pop("tree"))
    print(json.dumps(reply))
    return 0 if reply.get("ok") else EXIT_FAILED


def parse(argv):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", default=os.environ.get("KERNARIUM_AUTOMATION_DEVICE"),
                        help="device UDID (default: $KERNARIUM_AUTOMATION_DEVICE)")
    parser.add_argument("--runner", default=RUNNER, help=argparse.SUPPRESS)
    parser.add_argument("--bundle", default=KERNARIUM,
                        help=f"application a command acts on (default: {KERNARIUM})")
    parser.add_argument("--state-dir", default=os.environ.get(
        "KERNARIUM_AUTOMATION_STATE_DIR", str(CHECKOUT / ".build/kernarium-automation")),
        help="where `start` keeps the host's xcodebuild log and supervisor pid")
    parser.add_argument("--derived-data", default=str(CHECKOUT / ".build/kernarium-automation/derived"),
                        help="derived data of the host build")
    parser.add_argument("--timeout", type=int, default=120,
                        help="seconds to wait for a command's result (default: 120)")
    commands = parser.add_subparsers(dest="command", required=True)

    build = commands.add_parser("build", help="build the host for iOS devices")
    build.add_argument("--xcodebuild-arg", action="append", default=[],
                       help="extra xcodebuild argument, such as a package cache path")
    start = commands.add_parser("start", help="start the host; the device may ask for approval")
    start.add_argument("--hours", type=float, default=8, help="host lifetime (default: 8)")
    start.add_argument("--wait", type=int, default=240,
                       help="seconds to wait for the host to serve (default: 240)")
    supervise = commands.add_parser("_supervise")
    supervise.add_argument("--xctestrun", required=True)
    supervise.add_argument("--seconds", type=int, required=True)
    supervise.add_argument("--nonce", required=True)
    commands.add_parser("status", help="report whether the host is serving")
    stop = commands.add_parser(
        "stop", help="end the host session; starting another needs a new approval")
    stop.add_argument("--force", action="store_true",
                      help="stop even though a new host must be approved at the device")

    commands.add_parser("ping", help="round-trip an empty command")
    launch = commands.add_parser("launch", help="launch (or relaunch) the application")
    launch.add_argument("--env", action="append", default=[], metavar="KEY=VALUE")
    launch.add_argument("--arg", action="append", default=[])
    for name, text in (("activate", "bring the application to the foreground"),
                       ("terminate", "terminate the application"),
                       ("state", "report the application's run state"),
                       ("tree", "print the application's element tree")):
        commands.add_parser(name, help=text)
    for name, text in (("tap", "tap an element or a point in the application"),
                       ("wait", "wait for an element to appear or disappear"),
                       ("exists", "report whether an element exists")):
        sub = commands.add_parser(name, help=text)
        sub.add_argument("--id", help="accessibility identifier")
        sub.add_argument("--label", help="accessibility label")
        sub.add_argument("--element-timeout", type=float, default=10)
        if name == "tap":
            sub.add_argument("--x", type=float, help="point offset from the application's origin")
            sub.add_argument("--y", type=float)
        if name == "wait":
            sub.add_argument("--gone", action="store_true", help="wait for disappearance")
    drag = commands.add_parser(
        "drag", help="press a point, drag it to another and lift (points from the "
                     "application's origin)")
    for name in ("x1", "y1", "x2", "y2"):
        drag.add_argument(name, type=float)
    drag.add_argument("seconds", type=float, nargs="?", default=0.5,
                      help="how long to press before dragging (default: 0.5)")
    drag.add_argument("--velocity", type=float,
                      help="drag speed in points per second (default: XCTest's)")
    drag.add_argument("--hold", type=float, default=0.0,
                      help="seconds to hold at the end before lifting (default: 0)")
    orientation = commands.add_parser(
        "orientation", help="rotate the device, or report its orientation")
    orientation.add_argument("orientation", nargs="?", choices=ORIENTATIONS)
    type_ = commands.add_parser("type", help="type text into the focused element")
    type_.add_argument("text")
    siri = commands.add_parser(
        "siri", help="start a Siri request with typed text (interrupts the app's audio)")
    siri.add_argument("text")
    key = commands.add_parser("key", help="press one hardware-keyboard key")
    key.add_argument("key", help="one character, or return, tab, space, up, down, left, "
                                 "right; XCTest cannot press delete or escape")
    key.add_argument("--modifiers", default="", help="comma list of shift, control, option, command")
    shot = commands.add_parser("screenshot", help="save a screenshot of the device")
    shot.add_argument("-o", "--output", required=True)

    options = parser.parse_args(argv)
    if not options.device:
        parser.error("name the device with --device or $KERNARIUM_AUTOMATION_DEVICE")
    if options.command == "tap" and (options.x is None) != (options.y is None):
        parser.error("tap needs both --x and --y")
    return options


def main(argv):
    options = parse(argv)
    handlers = {"build": command_build, "start": command_start, "stop": command_stop,
                "status": command_status, "_supervise": command_supervise}
    try:
        handler = handlers.get(options.command, run_op)
        if options.command == "build":
            options.timeout = max(options.timeout, 1800)
        return handler(options)
    except NoHost as error:
        log(str(error))
        return EXIT_NO_HOST


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
