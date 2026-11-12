#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guest probe: a contact the host cancels reaches evdev and libinput as a palm.

Run through run_guest_probe.py with --probe-input, whose host executable
(uml-touchscreen-reports) describes the Virtio touchscreen as the host attaches
it and gives the reports the host's Touchscreen model sends for a few contact
sequences. This probe builds a uinput device with those capabilities and
writes those reports to it. uinput and the virtio_input driver both register
through input_mt_init_slots(dev, n, 0), so the guest's input core treats the two
alike. Each step's reports are read back twice: raw from evdev, and through
the image's libinput (the path backend, with a udev database entry that tags
the node as a touchscreen).

A cancel is two reports: the slot turns MT_TOOL_PALM while it keeps its
tracking ID, then the tracking ID ends. Checked: the kernel passes both reports
as written; libinput turns them into TOUCH_CANCEL and no TOUCH_UP, also for
one finger of two while the other moves on and lifts normally; and a new
contact in a slot that was a palm is a normal TOUCH_DOWN, because the model
sets MT_TOOL_FINGER before the tracking ID (libinput before 1.30 swallows the
whole touch otherwise). A cancel of a slot that holds no contact sends nothing.
Prints one line per check and exits with the number of failures.
"""

import ctypes
import fcntl
import json
import os
import select
import struct
import subprocess
import sys
import time

PROBE_INPUT = "/usr/local/libexec/uml-test/probe-input"

failures = 0

EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0
BTN_TOUCH = 0x14A
ABS_X, ABS_Y = 0x00, 0x01
ABS_MT_SLOT = 0x2F
ABS_MT_POSITION_X, ABS_MT_POSITION_Y = 0x35, 0x36
ABS_MT_TOOL_TYPE = 0x37
ABS_MT_TRACKING_ID = 0x39
MT_TOOL_FINGER, MT_TOOL_PALM = 0, 2
INPUT_PROP_DIRECT = 0x01
BUS_VIRTUAL = 0x06

NAMES = {
    (EV_KEY, BTN_TOUCH): "BTN_TOUCH",
    (EV_ABS, ABS_X): "ABS_X",
    (EV_ABS, ABS_Y): "ABS_Y",
    (EV_ABS, ABS_MT_SLOT): "SLOT",
    (EV_ABS, ABS_MT_POSITION_X): "MT_X",
    (EV_ABS, ABS_MT_POSITION_Y): "MT_Y",
    (EV_ABS, ABS_MT_TOOL_TYPE): "TOOL",
    (EV_ABS, ABS_MT_TRACKING_ID): "TRKID",
    (EV_SYN, SYN_REPORT): "SYN",
}


def ioc(direction, number, size):
    return (direction << 30) | (size << 16) | (ord("U") << 8) | number


UI_DEV_CREATE = ioc(0, 1, 0)
UI_DEV_DESTROY = ioc(0, 2, 0)
UI_DEV_SETUP = ioc(1, 3, 92)
UI_ABS_SETUP = ioc(1, 4, 28)
UI_SET_EVBIT = ioc(1, 100, 4)
UI_SET_KEYBIT = ioc(1, 101, 4)
UI_SET_ABSBIT = ioc(1, 103, 4)
UI_SET_PROPBIT = ioc(1, 110, 4)
UI_GET_SYSNAME_64 = ioc(2, 44, 64)

EVENT = struct.Struct("qqHHi")


def check(condition, message):
    global failures
    print(("ok   " if condition else "FAIL ") + message, flush=True)
    if not condition:
        failures += 1


def observe(message):
    print("obs  " + message, flush=True)


def ensure_mounts():
    if not os.path.exists("/sys/class"):
        subprocess.run(["mount", "-t", "sysfs", "sysfs", "/sys"], check=False)
    os.makedirs("/run/udev/data", exist_ok=True)


def create_device(device):
    """A uinput device with the capabilities the host's device configuration
    lists; returns its descriptor and the event node's path."""
    fd = os.open("/dev/uinput", os.O_RDWR | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_ABS)
    for code in device["keys"]:
        fcntl.ioctl(fd, UI_SET_KEYBIT, code)
    for code in device["properties"]:
        fcntl.ioctl(fd, UI_SET_PROPBIT, code)
    for axis in device["absolute"]:
        fcntl.ioctl(fd, UI_SET_ABSBIT, axis["code"])
        fcntl.ioctl(
            fd,
            UI_ABS_SETUP,
            struct.pack(
                "HxxiiiiII", axis["code"], 0, axis["minimum"], axis["maximum"],
                0, 0, 0
            ),
        )
    name = b"Kernarium Touchscreen Probe"
    fcntl.ioctl(
        fd,
        UI_DEV_SETUP,
        struct.pack("HHHH80sI", BUS_VIRTUAL, 0x1AF4, 0x7001, 1, name, 0),
    )
    fcntl.ioctl(fd, UI_DEV_CREATE)
    buffer = bytearray(64)
    fcntl.ioctl(fd, UI_GET_SYSNAME_64, buffer)
    sysname = buffer.split(b"\0", 1)[0].decode()
    base = f"/sys/devices/virtual/input/{sysname}"
    deadline = time.monotonic() + 5
    node = None
    while time.monotonic() < deadline and node is None:
        for entry in os.listdir(base) if os.path.isdir(base) else []:
            if entry.startswith("event"):
                node = entry
        if node is None:
            time.sleep(0.05)
    if node is None:
        raise RuntimeError(f"no event node under {base}")
    with open(f"{base}/{node}/dev") as stream:
        major, minor = (int(part) for part in stream.read().split(":"))
    path = f"/dev/input/{node}"
    deadline = time.monotonic() + 5
    while not os.path.exists(path) and time.monotonic() < deadline:
        time.sleep(0.05)
    if not os.path.exists(path):
        os.makedirs("/dev/input", exist_ok=True)
        os.mknod(path, 0o600 | 0o020000, os.makedev(major, minor))
    with open(f"/run/udev/data/c{major}:{minor}", "w") as stream:
        stream.write(
            f"I:{int(time.monotonic() * 1e6)}\nE:ID_INPUT=1\n"
            "E:ID_INPUT_TOUCHSCREEN=1\nG:seat\nQ:seat\nV:1\n"
        )
    return fd, path


class LibInput:
    OPEN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p)
    CLOSE = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_void_p)

    class Interface(ctypes.Structure):
        pass

    Interface._fields_ = [("open_restricted", OPEN), ("close_restricted", CLOSE)]

    TYPES = {1: "ADDED", 2: "REMOVED", 500: "DOWN", 501: "UP", 502: "MOTION",
             503: "CANCEL", 504: "FRAME"}

    def __init__(self, path):
        self.lib = ctypes.CDLL("libinput.so.10")
        lib = self.lib
        lib.libinput_path_create_context.restype = ctypes.c_void_p
        lib.libinput_path_create_context.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        lib.libinput_path_add_device.restype = ctypes.c_void_p
        lib.libinput_path_add_device.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.libinput_get_fd.argtypes = [ctypes.c_void_p]
        lib.libinput_dispatch.argtypes = [ctypes.c_void_p]
        lib.libinput_get_event.restype = ctypes.c_void_p
        lib.libinput_get_event.argtypes = [ctypes.c_void_p]
        lib.libinput_event_get_type.argtypes = [ctypes.c_void_p]
        lib.libinput_event_get_touch_event.restype = ctypes.c_void_p
        lib.libinput_event_get_touch_event.argtypes = [ctypes.c_void_p]
        lib.libinput_event_touch_get_slot.argtypes = [ctypes.c_void_p]
        lib.libinput_event_destroy.argtypes = [ctypes.c_void_p]
        lib.libinput_log_set_priority.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.libinput_device_has_capability.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self._open = self.OPEN(self._open_restricted)
        self._close = self.CLOSE(lambda fd, data: os.close(fd))
        self.interface = self.Interface(self._open, self._close)
        self.context = lib.libinput_path_create_context(ctypes.byref(self.interface), None)
        lib.libinput_log_set_priority(self.context, 20)
        self.device = lib.libinput_path_add_device(self.context, path.encode())
        self.fd = lib.libinput_get_fd(self.context)

    @staticmethod
    def _open_restricted(path, flags, data):
        try:
            return os.open(path, flags)
        except OSError as error:
            return -error.errno

    def events(self, quiet=0.3):
        """The touch events since the last call, as (type, slot) pairs."""
        out = []
        deadline = time.monotonic() + quiet
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return out
            ready, _, _ = select.select([self.fd], [], [], remaining)
            if not ready:
                continue
            self.lib.libinput_dispatch(self.context)
            while True:
                event = self.lib.libinput_get_event(self.context)
                if not event:
                    break
                kind = self.lib.libinput_event_get_type(event)
                slot = None
                if 500 <= kind <= 503:
                    touch = self.lib.libinput_event_get_touch_event(event)
                    slot = self.lib.libinput_event_touch_get_slot(touch)
                # Frames carry no slot, and device events are not touches.
                if kind not in (1, 2, 504):
                    out.append((self.TYPES.get(kind, str(kind)), slot))
                self.lib.libinput_event_destroy(event)
            deadline = time.monotonic() + quiet


def write(fd, reports):
    for frame in reports:
        for kind, code, value in frame:
            os.write(fd, EVENT.pack(0, 0, kind, code, value))
        time.sleep(0.02)


def read_evdev(fd):
    """The frames evdev delivered since the last call, each a list of
    (name, value) pairs."""
    frames, frame = [], []
    while True:
        try:
            data = os.read(fd, EVENT.size * 64)
        except BlockingIOError:
            break
        for offset in range(0, len(data), EVENT.size):
            _, _, kind, code, value = EVENT.unpack_from(data, offset)
            if kind == EV_SYN and code == SYN_REPORT:
                frames.append(frame)
                frame = []
            else:
                frame.append((NAMES.get((kind, code), f"{kind}:{code}"), value))
    return frames


def index_of(frame, name):
    for index, (candidate, _) in enumerate(frame):
        if candidate == name:
            return index
    return None


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else PROBE_INPUT
    with open(path) as stream:
        model = json.load(stream)
    steps = {step["name"]: step["reports"] for step in model["steps"]}

    ensure_mounts()
    print(f"kernel {os.uname().release}", flush=True)
    try:
        version = subprocess.run(["dpkg-query", "-W", "-f=${Version}", "libinput10"],
                                 capture_output=True, text=True, timeout=30)
        print(f"libinput10 {version.stdout or version.stderr.strip()}", flush=True)
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"libinput10 version unknown: {error}", flush=True)

    tool = [axis for axis in model["absolute"] if axis["code"] == ABS_MT_TOOL_TYPE]
    check(len(tool) == 1 and (tool[0]["minimum"], tool[0]["maximum"]) == (0, MT_TOOL_PALM),
          "the touchscreen advertises ABS_MT_TOOL_TYPE from finger through palm")

    uinput, device_path = create_device(model)
    evdev = os.open(device_path, os.O_RDONLY | os.O_NONBLOCK)
    libinput = LibInput(device_path)
    check(libinput.device is not None, f"libinput opened {device_path}")
    check(libinput.lib.libinput_device_has_capability(libinput.device, 2) == 1
          if libinput.device else False, "libinput sees a touch device")
    observe(f"setup {libinput.events()}")

    def play(name):
        """Writes a step's reports; returns the evdev frames and libinput's
        touch events."""
        reports = steps[name]
        write(uinput, reports)
        frames = read_evdev(evdev)
        events = libinput.events()
        observe(f"{name}: evdev {frames}")
        observe(f"{name}: libinput {events}")
        return reports, frames, events

    # A plain tap, as the control.
    _, frames, events = play("tap")
    check(events == [("DOWN", 0), ("UP", 0)], f"tap: DOWN, UP ({events})")
    check(len(frames) == 2 and ("TRKID", -1) in frames[1], "tap: the kernel passes two reports")

    # One finger: down, move, cancel.
    reports, frames, events = play("cancel-one-finger")
    check(len(frames) == len(reports) == 4,
          "cancel-one-finger: the kernel passes the four reports as sent")
    if len(frames) == 4:
        check(index_of(frames[0], "TOOL") is None,
              "cancel-one-finger: the kernel drops a FINGER the slot already has")
        check(("TOOL", MT_TOOL_PALM) in frames[2] and index_of(frames[2], "TRKID") is None,
              "cancel-one-finger: the palm report keeps the tracking ID")
        # BTN_TOUCH is 1 already, so the kernel reports no change; what matters
        # is that it does not drop until the lift.
        check(("BTN_TOUCH", 0) not in frames[2],
              "cancel-one-finger: the palm report keeps BTN_TOUCH down")
        check(("TRKID", -1) in frames[3] and ("BTN_TOUCH", 0) in frames[3]
              and index_of(frames[3], "TOOL") is None,
              "cancel-one-finger: the next report ends the contact")
    check(events == [("DOWN", 0), ("MOTION", 0), ("CANCEL", 0)],
          f"cancel-one-finger: DOWN, MOTION, CANCEL and no UP ({events})")

    # The slot that held a palm takes a finger again: FINGER, then the tracking ID.
    _, frames, events = play("touch-after-cancel")
    first = frames[0] if frames else []
    tool_at, tracking_at = index_of(first, "TOOL"), index_of(first, "TRKID")
    check(len(frames) == 2 and ("TOOL", MT_TOOL_FINGER) in first
          and tool_at is not None and tracking_at is not None
          and tool_at < tracking_at,
          "touch-after-cancel: the kernel passes the FINGER, before the tracking ID")
    check(events == [("DOWN", 0), ("UP", 0)],
          f"touch-after-cancel: a FINGER-first contact after a palm is a normal touch ({events})")

    # Two fingers, the first cancelled while the second moves on.
    _, _, events = play("cancel-one-of-two")
    check(events == [("DOWN", 0), ("DOWN", 1), ("CANCEL", 0), ("MOTION", 1), ("UP", 1)],
          f"cancel-one-of-two: only the cancelled finger is cancelled ({events})")

    # The second finger cancelled, the first lifting normally after it.
    _, _, events = play("cancel-the-second-of-two")
    check(events == [("DOWN", 0), ("DOWN", 1), ("CANCEL", 1), ("UP", 0)],
          f"cancel-the-second-of-two: the first finger still lifts ({events})")

    # The cancelled slot is a finger again, and a tap in it is a tap.
    _, _, events = play("tap-in-the-cancelled-slot")
    check(events == [("DOWN", 1), ("UP", 1)],
          f"tap-in-the-cancelled-slot: DOWN, UP ({events})")

    # A cancel of a slot with no contact reaches the guest as nothing at all.
    reports, frames, events = play("cancel-nothing")
    check(reports == [] and frames == [] and events == [],
          "cancel-nothing: nothing is sent and nothing is seen")

    fcntl.ioctl(uinput, UI_DEV_DESTROY)
    return failures


if __name__ == "__main__":
    raise SystemExit(main())
