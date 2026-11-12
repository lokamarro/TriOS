#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Contract tests for the Steam appliance boot benchmark, without a guest.

The harness decides things a later reader depends on and that no other test
covers: which runs keep their user-data disk, that a campaign over two images
is counterbalanced, that a finished campaign can be re-read, and that its
console reports why a guest died. All are exercised here against the real
functions, with the boot, the emulator and the disk preparation stubbed, so
nothing boots, allocates a real image, or touches the network.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import contextlib
import importlib.util
import io
import json
import pathlib
import subprocess
import sys
import tempfile


HARNESS = (
    pathlib.Path(__file__).resolve().parents[2] / "perf" / "run_steam_boot_bench.py"
)


def load_harness():
    spec = importlib.util.spec_from_file_location("steam_boot_bench", HARNESS)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def stub_disk(path: pathlib.Path, size_gib: int) -> None:
    """Stand in for prepare_disk: a few bytes, never a real image."""
    path.write_bytes(b"stub user-data disk")
    pathlib.Path(str(path) + ".lock").write_bytes(b"lock")


def options(output_dir: pathlib.Path, **overrides) -> argparse.Namespace:
    settings = {
        "output_dir": output_dir,
        "size_gib": 1,
        "memory": "8G",
        "cpus": 10,
        "engine": "gadgets",
        "poll_seconds": 2.0,
        "keep_disks": False,
        "discard_failed_disks": False,
    }
    settings.update(overrides)
    return argparse.Namespace(**settings)


IMAGE = {"name": "img", "emulator": "/nonexistent/emulator", "rootfs": "/dev/null"}


def check_cleanup(harness) -> list[str]:
    """A failed run must keep the evidence; a good one must not hoard 32 GiB."""
    failures: list[str] = []
    cases = (
        ("success", True, {}, False),
        ("failure", False, {}, True),
        ("failure with --discard-failed-disks", False,
         {"discard_failed_disks": True}, False),
        ("success with --keep-disks", True, {"keep_disks": True}, True),
    )
    original_boot = harness.one_boot
    original_totals = harness.derived_totals
    original_prepare = harness.prepare_disk
    try:
        harness.prepare_disk = stub_disk
        harness.derived_totals = lambda record: None
        for label, succeeds, overrides, expect_kept in cases:
            if succeeds:
                harness.one_boot = (
                    lambda *args, **kwargs: {"boot_seconds": 1.0})
            else:
                def refuse(*args, **kwargs):
                    raise RuntimeError(
                        "first boot: the gaming session ended (inactive)")

                harness.one_boot = refuse
            with tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                # The harness narrates each boot, including the stubbed
                # failure. Keep that out of the test log, where a line saying
                # FAILED would read as this test failing.
                with contextlib.redirect_stdout(io.StringIO()):
                    run = harness.one_run(
                        IMAGE, options(root, **overrides), 1)
                disk = root / "img-run01" / "userdata.img"
                lock = pathlib.Path(str(disk) + ".lock")
                recorded = run.get("userdata_image")
                on_disk = json.loads(
                    (root / "img-run01" / "run.json").read_text())
                if disk.exists() is not expect_kept:
                    failures.append(
                        f"{label}: disk kept={disk.exists()}, "
                        f"expected {expect_kept}")
                if lock.exists() is not expect_kept:
                    failures.append(
                        f"{label}: lock kept={lock.exists()}, "
                        f"expected {expect_kept}")
                if expect_kept:
                    if recorded != str(disk):
                        failures.append(
                            f"{label}: run.json in memory records "
                            f"userdata_image={recorded!r}, expected {disk}")
                    if on_disk.get("userdata_image") != str(disk):
                        failures.append(
                            f"{label}: written run.json lacks the kept path")
                elif recorded is not None or "userdata_image" in on_disk:
                    failures.append(
                        f"{label}: a deleted disk must not be advertised")
                expected_status = "ok" if succeeds else "failed"
                if run.get("status") != expected_status:
                    failures.append(
                        f"{label}: status={run.get('status')!r}, "
                        f"expected {expected_status!r}")
    finally:
        harness.one_boot = original_boot
        harness.derived_totals = original_totals
        harness.prepare_disk = original_prepare
    return failures


def check_order(harness) -> list[str]:
    """Two images must alternate, or host drift reads as an image difference."""
    images = [dict(IMAGE, name="a"), dict(IMAGE, name="b")]
    schedule = harness.campaign_order(images, 3, "abba")
    names = [str(image["name"]) for image, _ in schedule]
    if names != ["a", "b", "b", "a", "a", "b"]:
        return [f"abba order is {names}, expected a b b a a b"]
    indices = [index for _, index in schedule]
    if sorted(indices) != [1, 1, 2, 2, 3, 3]:
        return [f"run indices are {indices}, expected each image numbered 1-3"]
    interleaved = [
        str(image["name"])
        for image, _ in harness.campaign_order(images, 2, "interleave")
    ]
    if interleaved != ["a", "b", "a", "b"]:
        return [f"interleave order is {interleaved}, expected a b a b"]
    return []


DRY_RUN_EXPECTED = ["a run 1", "b run 1", "b run 2", "a run 2", "a run 3",
                    "b run 3"]


def check_dry_run() -> list[str]:
    """--dry-run must print the schedule and start nothing."""
    # The harness resolves both paths strictly, so stand in with files that
    # exist and are never executed: --dry-run starts nothing.
    stand_in = sys.executable
    with tempfile.TemporaryDirectory() as directory:
        result = subprocess.run(
            (sys.executable, "-B", str(HARNESS),
             "--image", "a", "--emulator", stand_in,
             "--rootfs", stand_in,
             "--image", "b", "--emulator", stand_in,
             "--rootfs", stand_in,
             "--output-dir", directory, "--runs", "3", "--order", "abba",
             "--dry-run"),
            check=False, capture_output=True, text=True)
        if result.returncode != 0:
            return [f"--dry-run exited {result.returncode}: {result.stderr}"]
        printed = [line.strip() for line in result.stdout.splitlines()
                   if line.strip()]
        if printed[:len(DRY_RUN_EXPECTED)] != DRY_RUN_EXPECTED:
            return [f"--dry-run printed {printed}, expected {DRY_RUN_EXPECTED}"]
        leftovers = sorted(p.name for p in pathlib.Path(directory).iterdir())
        if leftovers:
            return [f"--dry-run wrote {leftovers}; it must run nothing"]
    return []


FIXTURE = [
    {"image": "a", "run": 1, "status": "ok",
     "host": {"load_average_1m": 1.0, "uml_processes_before_start": 0},
     "boots": [
         {"boot": "first", "boot_seconds": 300.0,
          "boot_seconds_excluding_download": 270.0, "storage_ready": 70.0},
         {"boot": "second", "boot_seconds": 60.0, "storage_ready": 2.0}]},
    {"image": "a", "run": 2, "status": "ok",
     "host": {"load_average_1m": 2.0, "uml_processes_before_start": 1},
     "boots": [
         {"boot": "first", "boot_seconds": 310.0,
          "boot_seconds_excluding_download": 280.0, "storage_ready": 72.0},
         {"boot": "second", "boot_seconds": 62.0, "storage_ready": 2.2}]},
]


def check_summarize() -> list[str]:
    """--summarize must re-read a finished campaign and run nothing."""
    with tempfile.TemporaryDirectory() as directory:
        results = pathlib.Path(directory) / "results.json"
        results.write_text(json.dumps(FIXTURE))
        result = subprocess.run(
            (sys.executable, "-B", str(HARNESS), "--summarize", str(results)),
            check=False, capture_output=True, text=True)
        if result.returncode != 0:
            return [f"--summarize exited {result.returncode}: {result.stderr}"]
        out = result.stdout
        for needle in ("first", "second", "305.0", "61.0"):
            if needle not in out:
                return [f"--summarize output lacks {needle!r}:\n{out}"]
    return []


def check_cpus(harness) -> list[str]:
    """The guest defaults to 4 vCPUs, and every TSV row records them."""
    failures: list[str] = []
    default = harness.parse_arguments([]).cpus
    if default != 4:
        failures.append(f"the default guest has {default} vCPUs, not 4")
    run = {"image": "a", "run": 1, "cpus": 4, "status": "ok", "host": {},
           "boots": [{"boot": "first", "boot_seconds": 1.0}]}
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory) / "results.tsv"
        harness.write_tsv([run], path)
        header, row = path.read_text().splitlines()
        columns = dict(zip(header.split("\t"), row.split("\t")))
        if columns.get("cpus") != "4":
            failures.append(f"results.tsv lacks the vCPU count: {columns}")
    return failures


# How long the stand-in emulators get to answer. A stand-in replies at once, so
# only a hung console reaches this deadline; it outlasts a host under many
# times its normal load, which a short one did not.
CONSOLE_SECONDS = 60

# Stand-ins for the emulator. The first plays the image's autologin, echoes
# the sudo exec as the guest tty does, then serves commands with the host
# shell; the others die the way a failing guest does.
AUTOLOGIN = """#!/bin/sh
printf 'steam login: uml (automatic login)\\n\\numl@steam:~$ '
IFS= read -r line
[ "$line" = "exec sudo -n /bin/sh" ] || exit 9
printf '%s\\n# ' "$line"
exec /bin/sh
"""
PANIC = "#!/bin/sh\nprintf 'Kernel panic - not syncing: test\\n'\nexit 1\n"
FLAKE = "#!/bin/sh\nprintf 'VFS: Cannot open root device\\n'\nexit 1\n"


def steam_guest(harness, directory: pathlib.Path, script: str):
    emulator = directory / "emulator"
    emulator.write_text(script)
    emulator.chmod(0o755)
    return harness.SteamGuest(
        emulator, pathlib.Path("/dev/null"), directory / "userdata.img",
        "8G", 4, "gadgets", "/usr/local/libexec/kernarium-init", [],
        directory / "boot.log")


def check_console(harness) -> list[str]:
    """Login, commands and exit diagnosis go through the shared transport."""
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as name:
        directory = pathlib.Path(name)
        guest = steam_guest(harness, directory, AUTOLOGIN)
        try:
            guest.login(CONSOLE_SECONDS)
            result = guest.run_command("printf '%s' steam-console",
                                        CONSOLE_SECONDS)
            if (result.return_code, result.output) != (0, "steam-console"):
                failures.append(f"run_command returned {result!r}")
        finally:
            guest.close()
        if "steam-console" not in (directory / "boot.log").read_text():
            failures.append("close did not save the console transcript")

    for script, expected, message in (
        (PANIC, RuntimeError, "guest reported 'Kernel panic'"),
        (FLAKE, harness.BootFlake, "VFS: Cannot open root device"),
    ):
        with tempfile.TemporaryDirectory() as name:
            guest = steam_guest(harness, pathlib.Path(name), script)
            try:
                guest.login(CONSOLE_SECONDS)
            except expected as error:
                if message not in str(error):
                    failures.append(f"a dying guest reported {error!r}")
            except Exception as error:  # noqa: BLE001 - report any other kind
                failures.append(
                    f"a dying guest raised {error!r}, not {expected.__name__}")
            else:
                failures.append("login succeeded on a guest that died")
            finally:
                guest.close()
    return failures


def main() -> int:
    harness = load_harness()
    failures: list[str] = []
    failures += check_cleanup(harness)
    failures += check_order(harness)
    failures += check_dry_run()
    failures += check_summarize()
    failures += check_cpus(harness)
    failures += check_console(harness)
    if failures:
        for failure in failures:
            print(f"steam boot bench: {failure}", file=sys.stderr)
        return 1
    print("steam boot bench contract passed "
          "(cleanup, counterbalancing, dry run, summarize, vCPUs, console)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
