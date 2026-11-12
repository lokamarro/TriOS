#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Measure Steam appliance boot time to Big Picture on a fresh and a seeded disk.

One run creates a fresh GPT user-data disk, boots the image once (Steam
downloads and installs its own update), shuts the guest down cleanly, then
boots the same disk a second time (no download) and shuts down again. Both
boots are timed from emulator exec to the moment the session supervisor's
readiness contract holds: an X window named "Steam Big Picture Mode" whose map
state is ``IsViewable``.

The harness never modifies a guest image. Phase times therefore come from what
an unmodified appliance already records: systemd's monotonic unit timestamps,
the storage stage's progress file, and Steam's own ``bootstrap_log.txt``. They
are read once per boot over the ttyS0 autologin console and converted to
seconds since emulator exec through one clock anchor, read in a command of its
own and bracketed by host monotonic readings so it is paired with the host time
it really happened at. Readiness is polled, so its resolution is the poll
interval; the other phases resolve to half an anchor round trip, which each
record reports as ``phase_anchor_round_trip``. Storage preparation completes
before the console exists, so it is recovered from the progress file's
modification time rather than observed live.

Run ``--help`` for the measurement knobs. A campaign over two images is
counterbalanced (ABBA) and strictly sequential.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import calendar
import contextlib
import importlib.util
import json
import os
import pathlib
import re
import statistics
import subprocess
import sys
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY / "tests/integration/support"))
from guest import ConsoleProcess, LINE_START  # noqa: E402


def _load_disk_preparation():
    """Reuse the appliance launcher's GPT layout rather than restating it.

    The user-data disk carries a one-shot initialization marker that the
    image-owned storage stage requires before it will ever run mkfs, so the
    benchmark must create disks exactly the way a real launch does. The
    appliance launcher owns that layout.
    """
    specification = importlib.util.spec_from_file_location(
        "steam_x86_64_boot", REPOSITORY / "tools/rootfs/steam-x86_64/boot.py")
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module.prepare_disk


prepare_disk = _load_disk_preparation()

# The image-owned contract of the Kernarium Steam appliance.
IMAGE_INIT = "/usr/local/libexec/kernarium-init"
STORAGE_STATUS = "/run/kernarium-progress.status"
INSTALL_STATUS = "/run/user/*/kernarium-install.status"
BOOTSTRAP_LOG = "/home/uml/.local/share/Steam/logs/bootstrap_log.txt"
READY_WINDOW = "Steam Big Picture Mode"

FAILURE_PATTERN = re.compile(
    r"Kernel panic|Oops:|detected stalls on CPUs|"
    r"SIGILL|illegal instruction|undefined instruction|"
    r"Kernarium boot failed",
    re.IGNORECASE,
)
# The size probe occasionally fails a ubd open with a swallowed errno and the
# root filesystem then cannot be mounted. That is a known boot flake, not a
# property of the image under measurement, so it is classified and retried.
FLAKE_PATTERN = re.compile(
    r"VFS: Cannot open root device|"
    r"Unable to mount root fs|"
    r"ubd.*(?:Couldn't determine size|unable to determine size)",
    re.IGNORECASE,
)

# Steam's bootstrap log is plain text with a local-time prefix per line.
BOOTSTRAP_TIMESTAMP = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\]")
BOOTSTRAP_DOWNLOAD = re.compile(
    r"Downloading update \(([\d,]+) of ([\d,]+) KB\)", re.IGNORECASE)

# One readiness probe. It prints the live installer caption and, when the
# contract holds, the VIEWABLE token. Output stays small on purpose: the serial
# console is lossy for bulk transfers.
READY_PROBE = (
    "cat " + INSTALL_STATUS + " 2>/dev/null | head -c 200 | tr '\\n' '|'; "
    "printf ' '; "
    "for socket in /tmp/.X11-unix/X*; do "
    "[ -e \"$socket\" ] || continue; "
    "display=\":${socket##*/X}\"; "
    # The session's X server belongs to the session user and the console shell
    # is root, which holds no authorization for it: an unauthorized query is
    # refused ("Authorization required, but no authorization protocol
    # specified") whatever the window state is, so it can never observe
    # readiness. Ask as whoever owns the display socket.
    "session=$(stat -c %U \"$socket\" 2>/dev/null) || continue; "
    "[ -n \"$session\" ] || continue; "
    "window=$(sudo -n -u \"$session\" -H env DISPLAY=$display "
    "xwininfo -root -tree 2>/dev/null "
    "| grep -F '" + READY_WINDOW + "' | grep -oE '0x[0-9a-f]+' | head -n1); "
    "[ -n \"$window\" ] || continue; "
    "sudo -n -u \"$session\" -H env DISPLAY=$display "
    "xwininfo -id \"$window\" 2>/dev/null "
    "| grep -q 'Map State: IsViewable' && { printf 'VIEWABLE'; break; }; "
    "done; echo"
)

# A single small report gathered once per boot. Every value is either a guest
# monotonic microsecond count or a wall-clock epoch, both anchored below.
# systemd does not promise to emit properties in the order they were asked for,
# so each unit is queried on its own and its lines are prefixed here rather
# than inferred from the position of an Id line.
PHASE_UNITS = ("multi-user.target", "graphical.target", "steam-gaming.service")
# The anchor is its own tiny command. Bundling it with the report below would
# pair a guest clock reading with a host time one whole report round trip away.
# /proc/timer_list's "now at" is CLOCK_MONOTONIC in nanoseconds, the same clock
# systemd stamps units with; /proc/uptime is CLOCK_BOOTTIME and only a fallback.
ANCHOR_REPORT = (
    "printf 'anchor_monotonic_ns=%s\\n' "
    "\"$(awk '/^now at/{print $3; exit}' /proc/timer_list 2>/dev/null)\"; "
    "printf 'anchor_uptime=%s\\n' \"$(cut -d' ' -f1 /proc/uptime)\"; "
    "printf 'anchor_realtime=%s\\n' \"$(date +%s.%N)\"; "
    "printf 'anchor_utc_offset=%s\\n' \"$(date +%z)\""
)
# Readiness comes from this unit and nothing else, so its state is the one
# terminal condition worth watching for while waiting.
SESSION_STATE = "systemctl show steam-gaming.service -p ActiveState --value"
SESSION_CHECK_SECONDS = 30.0
PHASE_REPORT = (
    "for unit in " + " ".join(PHASE_UNITS) + "; do "
    "systemctl show \"$unit\" -p ActiveEnterTimestampMonotonic "
    "-p InactiveExitTimestampMonotonic -p ExecMainStartTimestampMonotonic "
    "-p ActiveState 2>/dev/null | sed \"s|^|$unit\\||\"; done; "
    "printf 'storage_status_mtime=%s\\n' "
    "\"$(stat -c %Y " + STORAGE_STATUS + " 2>/dev/null)\"; "
    "printf 'storage_status_text=%s\\n' "
    "\"$(tail -n1 " + STORAGE_STATUS + " 2>/dev/null)\"; "
    "printf 'userdata_fstype=%s\\n' \"$(findmnt -n -o FSTYPE /userdata 2>/dev/null)\""
)


def host_conditions() -> dict[str, object]:
    """Record, never reject: what else the host was doing when a run started."""
    load = os.getloadavg()
    conditions: dict[str, object] = {
        "load_average_1m": round(load[0], 2),
        "load_average_5m": round(load[1], 2),
        "load_average_15m": round(load[2], 2),
        "host_cpus": os.cpu_count(),
    }
    for name, key in (
        ("kern.memorystatus_vm_pressure_level", "memory_pressure_level"),
        ("vm.page_free_count", "free_pages"),
    ):
        try:
            conditions[key] = int(subprocess.run(
                ["sysctl", "-n", name], capture_output=True, text=True,
                timeout=15, check=True).stdout.strip())
        except (subprocess.SubprocessError, ValueError):
            conditions[key] = None
    try:
        report = subprocess.run(
            ["memory_pressure"], capture_output=True, text=True, timeout=30,
            stdin=subprocess.DEVNULL).stdout
        free = re.search(r"free percentage:\s*(\d+)", report)
        conditions["memory_free_percent"] = int(free[1]) if free else None
    except subprocess.SubprocessError:
        conditions["memory_free_percent"] = None
    try:
        others = subprocess.run(
            ["pgrep", "-f", "uml-darwin"], capture_output=True, text=True,
            timeout=15, stdin=subprocess.DEVNULL).stdout.split()
        conditions["uml_processes_before_start"] = len(others)
    except subprocess.SubprocessError:
        conditions["uml_processes_before_start"] = None
    return conditions


class BootFlake(RuntimeError):
    """A recognized infrastructure flake; the run is retried, not reported."""


class SteamGuest(ConsoleProcess):
    """One appliance boot driven through its ttyS0 autologin console.

    The Steam image owns its init and the gaming session starts from
    ``graphical.target``, so this runner owns its command line and reaches a
    shell through the image's autologin. The console transport and command
    protocol are the shared harness's ``ConsoleProcess``.
    """

    # systemd needs longer than the harness shell to stop after SIGTERM.
    STOP_GRACE_SECONDS = 10.0

    def __init__(self, emulator: pathlib.Path, rootfs: pathlib.Path,
                 userdata: pathlib.Path, memory: str, cpus: int,
                 engine: str, init: str, extra: list[str],
                 transcript_path: pathlib.Path,
                 activity_log: pathlib.Path | None = None) -> None:
        self._argv = [
            str(emulator), f"--execution-engine={engine}", "--audio", "--video",
            "--gpu-display=vnc", "--gpu-vnc-port=0",
            f"ubd0rc={rootfs}", f"ubd1={userdata}", "root=/dev/ubda",
            "rootfstype=erofs", "ro", f"init={init}", f"mem={memory}",
            f"ncpus={cpus}", "console=ttyS0", "ip=off", "quiet", "loglevel=3",
            "systemd.show_status=false", "systemd.unit=graphical.target",
            *extra,
        ]
        self._transcript_path = transcript_path
        # The video device writes its activity log once, when a stream
        # closes, so naming a per-boot file records what each boot decoded
        # without costing timing. A caller that already named one is
        # collecting across boots on purpose, so leave theirs alone.
        environment = dict(os.environ)
        if activity_log is not None and "UML_VIDEO_ACTIVITY_LOG" not in os.environ:
            environment["UML_VIDEO_ACTIVITY_LOG"] = str(activity_log)
        self.started_at = time.monotonic()
        self.started_realtime = time.time()
        super().__init__(self._argv, environment=environment)

    @property
    def command_line(self) -> list[str]:
        return list(self._argv)

    def transcript(self) -> str:
        return self.output()

    def save_transcript(self) -> None:
        self._transcript_path.parent.mkdir(parents=True, exist_ok=True)
        self._transcript_path.write_text(self.transcript() + "\n")

    def login(self, timeout_seconds: float) -> float:
        """Reach a root shell and return the host monotonic time it answered."""
        login = self._read_until(
            re.compile(rb" login:\s*uml \(automatic login\)", re.MULTILINE),
            timeout_seconds)
        self._read_until(re.compile(LINE_START + rb"[^\r\n]*\$ $"),
                         timeout_seconds, login.end())
        search_from = len(self._transcript)
        self._write("exec sudo -n /bin/sh\n")
        self._read_until(re.compile(LINE_START + rb"[^\r\n]*# $"),
                         timeout_seconds, search_from)
        return time.monotonic()

    def poweroff(self, timeout_seconds: float) -> tuple[bool, float]:
        """Ask systemd for an orderly poweroff and wait for the emulator."""
        requested = time.monotonic()
        if self.return_code is not None:
            return True, 0.0
        with contextlib.suppress(BrokenPipeError, OSError, TimeoutError):
            self._write("sync; systemctl --no-block poweroff\n")
        if self.wait_for_exit(timeout_seconds):
            return True, time.monotonic() - requested
        # systemd refused or hung; force the kernel halt the shared harness
        # uses, and report that this boot did not shut down cleanly.
        with contextlib.suppress(BrokenPipeError, OSError, TimeoutError):
            self._write("sync; poweroff -f\n")
        self.wait_for_exit(30)
        return False, time.monotonic() - requested

    def close(self) -> None:
        try:
            super().close()
        finally:
            self.save_transcript()

    def check_health(self) -> None:
        text = self.transcript()
        if FLAKE_PATTERN.search(text):
            raise BootFlake(FLAKE_PATTERN.search(text)[0])
        found = FAILURE_PATTERN.search(text)
        if found:
            raise RuntimeError(f"guest reported {found[0]!r}")

    def _exited_without_output(self) -> None:
        # A guest that died says why on its console; report that, and a
        # recognized flake as one, before the bare exit status.
        self.check_health()


def parse_report(text: str) -> dict[str, str]:
    """Read ``key=value`` and ``unit|key=value`` lines from the phase report."""
    values: dict[str, str] = {}
    for line in text.splitlines():
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        value = value.strip()
        if "|" in key:
            unit, _, name = key.partition("|")
            values[f"{unit.strip()}.{name.strip()}"] = value
            continue
        values[key] = value
    return values


def monotonic_offset(report: dict[str, str], started_at: float,
                     anchored_at: float) -> tuple[float, float]:
    """Map the guest's two clocks onto seconds since the emulator started.

    ``anchored_at`` is the host monotonic time that the guest's own clock
    readings belong to. Guest monotonic zero therefore sits that many seconds
    of guest uptime earlier, and adding the returned shift to any guest
    monotonic value gives seconds since emulator exec. The second shift does
    the same for guest wall-clock values such as file modification times.
    """
    raw = report.get("anchor_monotonic_ns", "").strip()
    uptime = (int(raw) / 1e9) if raw.isdigit() else float(report["anchor_uptime"])
    realtime = float(report["anchor_realtime"])
    return anchored_at - uptime - started_at, anchored_at - realtime - started_at


def guest_utc_offset(report: dict[str, str]) -> int:
    """Seconds east of UTC in the guest, so its local log stamps convert."""
    raw = report.get("anchor_utc_offset", "").strip()
    match = re.fullmatch(r"([+-])(\d{2})(\d{2})", raw)
    if match is None:
        # Without the guest's own offset, assume it shares the host's zone.
        return -time.timezone if not time.daylight else -time.altzone
    sign = 1 if match[1] == "+" else -1
    return sign * (int(match[2]) * 3600 + int(match[3]) * 60)


def bootstrap_phases(log: str, realtime_shift: float,
                     utc_offset: int) -> dict[str, object]:
    """Extract Steam's own update phases from its unmodified bootstrap log.

    The log lives on the retained data disk, so a later boot still reads what
    every earlier boot did. Those lines land before this boot began, and
    reporting them would credit a second boot with a download it never
    performed. Only lines inside the boot being measured are read.
    """
    phases: dict[str, object] = {}
    downloads: list[tuple[float, int, int]] = []
    markers = (
        ("Checking for available updates", "steam_check_for_updates"),
        ("Verifying installation", "steam_verify_installation"),
        ("Extracting package", "steam_extract_package"),
        ("Installing update", "steam_install_update"),
        ("Cleaning up", "steam_cleaning_up"),
        ("Update complete", "steam_update_complete"),
    )
    for line in log.splitlines():
        stamp = BOOTSTRAP_TIMESTAMP.match(line)
        if stamp is None:
            continue
        try:
            seconds = calendar.timegm(
                time.strptime(stamp[1], "%Y-%m-%d %H:%M:%S")) - utc_offset
        except ValueError:
            continue
        at = seconds + realtime_shift
        if at < 0:
            continue
        progress = BOOTSTRAP_DOWNLOAD.search(line)
        if progress:
            downloads.append((
                at,
                int(progress[1].replace(",", "")),
                int(progress[2].replace(",", ""))))
        for token, name in markers:
            if token.lower() in line.lower() and name not in phases:
                phases[name] = round(at, 3)
    if downloads:
        phases["steam_download_start"] = round(downloads[0][0], 3)
        phases["steam_download_end"] = round(downloads[-1][0], 3)
        phases["steam_download_seconds"] = round(
            downloads[-1][0] - downloads[0][0], 3)
        phases["steam_download_bytes"] = downloads[-1][2] * 1024
        phases["steam_download_observed_bytes"] = downloads[-1][1] * 1024
    return phases


def collect_phases(guest: SteamGuest, first_boot: bool) -> dict[str, object]:
    # Bracket the anchor command so the guest's clock reading is paired with
    # the host time it actually happened at. The residual error is half a
    # console round trip, which is also this measurement's stated resolution.
    before = time.monotonic()
    anchor = guest.run_command(ANCHOR_REPORT, 90)
    anchored_at = (before + time.monotonic()) / 2
    report = parse_report(anchor.output)
    report.update(parse_report(guest.run_command(PHASE_REPORT, 90).output))
    phases: dict[str, object] = {
        "phase_anchor_round_trip": round(time.monotonic() - before, 3),
    }
    try:
        monotonic_shift, realtime_shift = monotonic_offset(
            report, guest.started_at, anchored_at)
    except (KeyError, ValueError):
        phases["phase_anchor_failed"] = True
        return phases
    phases["guest_kernel_start"] = round(monotonic_shift, 3)
    for unit, name in zip(PHASE_UNITS, ("multi_user_target", "graphical_target",
                                        "steam_gaming_service")):
        for suffix, label in (
            ("ActiveEnterTimestampMonotonic", "active"),
            ("InactiveExitTimestampMonotonic", "start"),
            ("ExecMainStartTimestampMonotonic", "exec_start"),
        ):
            raw = report.get(f"{unit}.{suffix}")
            if raw is None or not raw.isdigit() or raw == "0":
                continue
            phases[f"{name}_{label}"] = round(
                monotonic_shift + int(raw) / 1e6, 3)
        state = report.get(f"{unit}.ActiveState")
        if state:
            phases[f"{name}_state"] = state
    mtime = report.get("storage_status_mtime", "")
    if mtime.replace(".", "", 1).isdigit():
        phases["storage_ready"] = round(float(mtime) + realtime_shift, 3)
    if report.get("storage_status_text"):
        phases["storage_status_text"] = report["storage_status_text"]
    phases["userdata_fstype"] = report.get("userdata_fstype", "")
    # The bootstrap log is small (tens of KB); read only its update lines so
    # the lossy console never has to carry the whole file.
    log = guest.run_command(
        "grep -E 'Checking for available updates|Verifying installation|"
        "Downloading update|Extracting package|Installing update|Cleaning up|"
        "Update complete' " + BOOTSTRAP_LOG + " 2>/dev/null | tail -n 60", 120)
    phases.update(bootstrap_phases(log.output, realtime_shift,
                                   guest_utc_offset(report)))
    phases["first_boot"] = first_boot
    return phases


def one_boot(image: dict[str, object], userdata: pathlib.Path,
             options: argparse.Namespace, label: str,
             transcript: pathlib.Path,
             collected: list[dict[str, object]]) -> dict[str, object]:
    """Time one boot, appending its record to ``collected`` even on failure."""
    timeout = (options.first_boot_timeout if label == "first"
               else options.second_boot_timeout)
    activity_log = transcript.with_name(f"{label}-video-activity.log")
    guest = SteamGuest(
        pathlib.Path(str(image["emulator"])), pathlib.Path(str(image["rootfs"])),
        userdata, options.memory, options.cpus, options.engine, options.init,
        list(image["extra"]), transcript, activity_log)
    record: dict[str, object] = {
        "boot": label,
        "command_line": guest.command_line,
        "emulator_pid": guest.process_id,
        "video_activity_log": str(activity_log),
    }
    try:
        anchored_at = guest.login(options.login_timeout)
        record["serial_console_ready"] = round(
            anchored_at - guest.started_at, 3)
        if guest.first_output_at is not None:
            record["first_console_output"] = round(
                guest.first_output_at - guest.started_at, 3)
        deadline = guest.started_at + timeout
        caption = ""
        viewable_at = None
        announced_at = 0.0
        checked_at = 0.0
        session_started = False
        while True:
            probe = guest.run_command(READY_PROBE, 90)
            text = probe.output.strip()
            if "VIEWABLE" in text:
                viewable_at = time.monotonic()
                break
            guest.check_health()
            now = time.monotonic()
            # Readiness can only ever arrive from the session supervisor, so
            # once that unit has run and stopped, the window is never coming
            # and waiting out the whole deadline learns nothing. Check rarely:
            # this is a serial console, not a status bus.
            if now - checked_at >= SESSION_CHECK_SECONDS:
                checked_at = now
                state = guest.run_command(SESSION_STATE, 60).output.strip()
                session_started |= state == "active"
                if session_started and state in ("failed", "inactive"):
                    record["steam_gaming_service_state"] = state
                    raise RuntimeError(
                        f"{label} boot: the gaming session ended ({state}) "
                        f"after {now - guest.started_at:.1f}s without a "
                        f"viewable Big Picture window; last caption: "
                        f"{caption!r}")
            if text and (text != caption or now - announced_at >= 120):
                caption = text
                announced_at = now
                print(f"      [{now - guest.started_at:7.1f}s] "
                      f"{caption[:110]}", flush=True)
                # Persist the console transcript as the boot proceeds so a
                # stalled run can be diagnosed while it is still stalled.
                guest.save_transcript()
            if time.monotonic() >= deadline:
                raise TimeoutError(
                    f"{label} boot did not reach a viewable Big Picture "
                    f"window within {timeout:g}s; last caption: {caption!r}")
            time.sleep(options.poll_seconds)
        record["big_picture_viewable"] = round(
            viewable_at - guest.started_at, 3)
        record["boot_seconds"] = record["big_picture_viewable"]
        record.update(collect_phases(guest, label == "first"))
        clean, elapsed = guest.poweroff(options.shutdown_timeout)
        record["shutdown_clean"] = clean
        record["shutdown_seconds"] = round(elapsed, 3)
        record["emulator_status"] = guest.return_code
        guest.check_health()
    finally:
        guest.close()
        record["transcript"] = str(transcript)
        collected.append(record)
    return record


def derived_totals(record: dict[str, object]) -> None:
    """Add the network-independent view of a first boot.

    Steam's bootstrap log lives on the retained data disk, so a later boot
    still reads the download an earlier boot performed. Those lines convert to
    negative offsets, and subtracting that window would understate a boot that
    downloaded nothing at all; only a window inside this boot is deducted.
    """
    total = record.get("boot_seconds")
    start = record.get("steam_download_start")
    end = record.get("steam_download_end")
    if isinstance(total, (int, float)) and isinstance(start, (int, float)) \
            and isinstance(end, (int, float)) and end >= start >= 0:
        record["boot_seconds_excluding_download"] = round(total - (end - start), 3)


def one_run(image: dict[str, object], options: argparse.Namespace,
            index: int) -> dict[str, object]:
    name = str(image["name"])
    directory = options.output_dir / f"{name}-run{index:02d}"
    directory.mkdir(parents=True, exist_ok=True)
    userdata = directory / "userdata.img"
    userdata.unlink(missing_ok=True)
    pathlib.Path(str(userdata) + ".lock").unlink(missing_ok=True)
    prepare_disk(userdata, options.size_gib)
    run: dict[str, object] = {
        "image": name,
        "run": index,
        "started": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "emulator": str(image["emulator"]),
        "rootfs": str(image["rootfs"]),
        "memory": options.memory,
        "cpus": options.cpus,
        "engine": options.engine,
        "size_gib": options.size_gib,
        "poll_seconds": options.poll_seconds,
        "host": host_conditions(),
        "boots": [],
    }
    boots: list[dict[str, object]] = run["boots"]  # type: ignore[assignment]
    try:
        for label in ("first", "second"):
            print(f"    {name} run {index} {label} boot", flush=True)
            record = one_boot(image, userdata, options, label,
                              directory / f"{label}-boot.log", boots)
            derived_totals(record)
            print(f"    {name} run {index} {label} boot: "
                  f"{record['boot_seconds']:.1f}s to Big Picture", flush=True)
        run["status"] = "ok"
    except BootFlake as flake:
        run["status"] = "flake"
        run["error"] = str(flake)
    except KeyboardInterrupt:
        run["status"] = "interrupted"
        raise
    except Exception as error:  # noqa: BLE001 - one failed run is data, not the end
        run["status"] = "failed"
        run["error"] = f"{type(error).__name__}: {error}"
        print(f"    {name} run {index} FAILED: {run['error']}", flush=True)
    finally:
        run["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        # A run that did not reach Big Picture is the only evidence of why.
        # The guest journal and Steam's own logs live on the user-data disk,
        # so keep it: deleting it is what makes such a failure unclassifiable
        # afterwards. A successful run's disk carries nothing worth 32 GiB.
        keep = options.keep_disks or (
            run.get("status") != "ok" and not options.discard_failed_disks)
        if keep:
            run["userdata_image"] = str(userdata)
            if run.get("status") != "ok":
                print(f"    {name} run {index}: user-data disk kept for "
                      f"diagnosis at {userdata}", flush=True)
        else:
            userdata.unlink(missing_ok=True)
            pathlib.Path(str(userdata) + ".lock").unlink(missing_ok=True)
        (directory / "run.json").write_text(json.dumps(run, indent=2) + "\n")
    return run


def campaign_order(images: list[dict[str, object]], runs: int,
                   order: str) -> list[tuple[dict[str, object], int]]:
    """Counterbalance so host drift cannot masquerade as an image difference."""
    schedule: list[tuple[dict[str, object], int]] = []
    counters = {str(image["name"]): 0 for image in images}
    for cycle in range(runs):
        sequence = images if (order == "interleave" or cycle % 2 == 0) \
            else list(reversed(images))
        for image in sequence:
            name = str(image["name"])
            counters[name] += 1
            schedule.append((image, counters[name]))
    return schedule


TSV_COLUMNS = (
    "image", "run", "cpus", "boot", "status", "boot_seconds",
    "boot_seconds_excluding_download", "steam_download_bytes",
    "steam_download_seconds", "serial_console_ready", "guest_kernel_start",
    "storage_ready", "graphical_target_active", "steam_gaming_service_active",
    "steam_check_for_updates", "steam_update_complete", "shutdown_seconds",
    "shutdown_clean", "load_average_1m", "memory_pressure_level",
    "memory_free_percent",
)


def write_tsv(results: list[dict[str, object]], path: pathlib.Path) -> None:
    lines = ["\t".join(TSV_COLUMNS)]
    for run in results:
        for record in run.get("boots", []):  # type: ignore[union-attr]
            row = []
            for column in TSV_COLUMNS:
                if column == "image":
                    value: object = run["image"]
                elif column in ("run", "cpus"):
                    value = run.get(column, "")
                elif column == "status":
                    value = run.get("status", "")
                elif column in ("load_average_1m", "memory_pressure_level",
                                "memory_free_percent"):
                    value = run["host"].get(column, "")  # type: ignore[union-attr]
                else:
                    value = record.get(column, "")
                row.append("" if value is None else str(value))
            lines.append("\t".join(row))
    path.write_text("\n".join(lines) + "\n")


def summarize(results: list[dict[str, object]]) -> list[str]:
    rows: dict[tuple[str, str], list[float]] = {}
    excluding: dict[tuple[str, str], list[float]] = {}
    for run in results:
        if run.get("status") != "ok":
            continue
        for record in run["boots"]:  # type: ignore[union-attr]
            key = (str(run["image"]), str(record["boot"]))
            rows.setdefault(key, []).append(float(record["boot_seconds"]))
            without = record.get("boot_seconds_excluding_download")
            if isinstance(without, (int, float)):
                excluding.setdefault(key, []).append(float(without))
    header = (f"{'image':<12} {'boot':<7} {'n':>2} {'median':>9} "
              f"{'min':>9} {'max':>9} {'median-no-download':>19}")
    lines = [header, "-" * len(header)]
    for key in sorted(rows):
        samples = sorted(rows[key])
        without = excluding.get(key)
        lines.append(
            f"{key[0]:<12} {key[1]:<7} {len(samples):>2} "
            f"{statistics.median(samples):>9.1f} {samples[0]:>9.1f} "
            f"{samples[-1]:>9.1f} "
            f"{statistics.median(without) if without else float('nan'):>19.1f}")
    return lines + phase_medians(results)


# Boot order, so the table reads as the sequence a user actually waits through.
PHASE_OFFSETS = (
    "guest_kernel_start", "storage_ready", "graphical_target_active",
    "steam_gaming_service_exec_start", "serial_console_ready",
    "steam_check_for_updates", "steam_download_start", "steam_download_end",
    "steam_extract_package", "steam_install_update", "steam_update_complete",
    "big_picture_viewable",
)
# Durations and sizes, which are not points on the boot timeline.
PHASE_MAGNITUDES = (
    "boot_seconds_excluding_download", "steam_download_seconds",
    "steam_download_bytes", "shutdown_seconds",
)
PHASE_ORDER = PHASE_OFFSETS + PHASE_MAGNITUDES


def phase_medians(results: list[dict[str, object]]) -> list[str]:
    """Per-phase medians side by side, which is what locates a difference.

    A total says one image is slower; only the phases say whether that is the
    kernel, the seed copy, a network-bound download, or Steam's own install.
    """
    samples: dict[tuple[str, str, str], list[float]] = {}
    images: list[str] = []
    for run in results:
        if run.get("status") != "ok":
            continue
        image = str(run["image"])
        if image not in images:
            images.append(image)
        for record in run["boots"]:  # type: ignore[union-attr]
            for phase in PHASE_ORDER:
                value = record.get(phase)
                if isinstance(value, (int, float)) and not isinstance(value, bool):
                    samples.setdefault(
                        (str(record["boot"]), phase, image), []).append(float(value))
    lines: list[str] = []
    for boot in ("first", "second"):
        for group, caption in (
            (PHASE_OFFSETS, "median seconds since emulator exec"),
            (PHASE_MAGNITUDES, "median durations and sizes"),
        ):
            present = [p for p in group
                       if any((boot, p, image) in samples for image in images)]
            if not present:
                continue
            lines.append("")
            lines.append(f"{boot} boot, {caption}")
            header = f"{'phase':<34}" + "".join(f"{i:>16}" for i in images)
            lines.extend((header, "-" * len(header)))
            for phase in present:
                row = f"{phase:<34}"
                for image in images:
                    values = samples.get((boot, phase, image))
                    if not values:
                        row += f"{'-':>16}"
                    elif phase.endswith("_bytes"):
                        row += f"{round(statistics.median(values)):>16d}"
                    else:
                        row += f"{statistics.median(values):>16.1f}"
                lines.append(row)
    return lines


def parse_arguments(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--summarize", type=pathlib.Path, metavar="RESULTS",
                        help="re-print the tables from a finished campaign's "
                             "results.json and exit, running nothing")
    parser.add_argument("--image", action="append", metavar="NAME",
                        help="image label; repeat per image")
    parser.add_argument("--emulator", action="append", type=pathlib.Path,
                        help="emulator for the image at the same position")
    parser.add_argument("--rootfs", action="append", type=pathlib.Path,
                        help="EROFS image for the image at the same position")
    parser.add_argument("--image-extra", action="append", default=[],
                        metavar="NAME=ARGUMENT",
                        help="extra emulator argument for one image; repeatable")
    parser.add_argument("--output-dir", type=pathlib.Path)
    parser.add_argument("--runs", type=int, default=3,
                        help="runs per image (default: 3)")
    parser.add_argument("--order", choices=("abba", "interleave"),
                        default="abba",
                        help="campaign order across images (default: abba)")
    parser.add_argument("--init", default=IMAGE_INIT,
                        help=f"image-owned init (default: {IMAGE_INIT})")
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--engine", choices=("jit", "gadgets"),
                        default="gadgets")
    parser.add_argument("--size-gib", type=int, default=32)
    parser.add_argument("--poll-seconds", type=float, default=2.0,
                        help="readiness poll interval; identical for every "
                             "image (default: 2.0)")
    parser.add_argument("--login-timeout", type=float, default=1800,
                        help="deadline for the ttyS0 root shell; a first boot "
                             "seeds the bundled client before any getty exists")
    parser.add_argument("--first-boot-timeout", type=float, default=3600)
    parser.add_argument("--second-boot-timeout", type=float, default=1200)
    parser.add_argument("--shutdown-timeout", type=float, default=180)
    parser.add_argument("--retries", type=int, default=2,
                        help="retries for a classified boot flake (default: 2)")
    parser.add_argument("--keep-disks", action="store_true",
                        help="retain every run's user-data disk for inspection")
    parser.add_argument("--discard-failed-disks", action="store_true",
                        help="delete a failed run's user-data disk too; the "
                             "default keeps it, because it carries the guest "
                             "journal that explains the failure")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the schedule and the first command line")
    return parser.parse_args(argv)


def build_images(options: argparse.Namespace) -> list[dict[str, object]]:
    if not len(options.image) == len(options.emulator) == len(options.rootfs):
        raise SystemExit(
            "--image, --emulator and --rootfs must be repeated the same "
            "number of times; the Nth value of each describes one image")
    if len(set(options.image)) != len(options.image):
        raise SystemExit("image names must be unique")
    extras: dict[str, list[str]] = {name: [] for name in options.image}
    for entry in options.image_extra:
        name, separator, argument = entry.partition("=")
        if not separator or name not in extras:
            raise SystemExit(f"--image-extra must be NAME=ARGUMENT: {entry}")
        extras[name].append(argument)
    images = []
    for name, emulator, rootfs in zip(options.image, options.emulator,
                                      options.rootfs):
        images.append({
            "name": name,
            "emulator": emulator.resolve(strict=True),
            "rootfs": rootfs.resolve(strict=True),
            "extra": extras[name],
        })
    return images


def main(argv: list[str] | None = None) -> int:
    options = parse_arguments(argv)
    if options.summarize is not None:
        print("\n".join(summarize(json.loads(options.summarize.read_text()))))
        return 0
    if not (options.image and options.emulator and options.rootfs
            and options.output_dir):
        raise SystemExit(
            "--image, --emulator, --rootfs and --output-dir are required "
            "unless --summarize is given")
    if options.runs < 1:
        raise SystemExit("--runs must be positive")
    if options.poll_seconds <= 0:
        raise SystemExit("--poll-seconds must be positive")
    images = build_images(options)
    schedule = campaign_order(images, options.runs, options.order)
    if options.dry_run:
        for image, index in schedule:
            print(f"{image['name']} run {index}")
        return 0
    options.output_dir.mkdir(parents=True, exist_ok=True)
    results: list[dict[str, object]] = []
    for image, index in schedule:
        print(f"  == {image['name']} run {index} "
              f"({len(results) + 1} of {len(schedule)}) ==", flush=True)
        for attempt in range(options.retries + 1):
            run = one_run(image, options, index)
            results.append(run)
            if run.get("status") != "flake":
                break
            print(f"     classified boot flake ({run.get('error')}); "
                  f"retry {attempt + 1} of {options.retries}", flush=True)
        write_tsv(results, options.output_dir / "results.tsv")
        (options.output_dir / "results.json").write_text(
            json.dumps(results, indent=2) + "\n")
    lines = summarize(results)
    for run in results:
        if run.get("status") != "ok":
            lines.append(f"anomaly: {run['image']} run {run['run']} "
                         f"{run.get('status')}: {run.get('error', '')}")
    print("\n".join(lines), flush=True)
    (options.output_dir / "summary.txt").write_text("\n".join(lines) + "\n")
    completed = {str(run["image"]) for run in results if run.get("status") == "ok"}
    return 0 if completed == {str(image["name"]) for image in images} else 1


if __name__ == "__main__":
    raise SystemExit(main())
