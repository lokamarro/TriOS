# SPDX-License-Identifier: GPL-2.0-or-later
"""Drive one of our iOS applications on a connected device with devicectl.

The plumbing every device runner shares: install an application, list and
copy files in its container's Documents directory, and launch it with
settings in its environment while holding its console until the run ends.
What differs between the applications (Kernarium, Andarium) is one
``Application``: its bundle identifier, the families of settings a launch
clears from the inherited environment, and the console line that reports the
run's result. tests/integration/support/kernarium.py holds Kernarium's boot
payloads and tests/integration/support/andarium.py Andarium's staging and
console lines; both build on this module.

``devicectl`` has no command that removes a file from an application
container and none that replaces one safely, so these helpers never
overwrite a file on a device, and never pass ``--remove-existing-content``,
which empties the whole container.
"""

from __future__ import annotations
# Imported support must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

from collections.abc import Callable, Mapping
from dataclasses import dataclass
import json
import os
import pathlib
import re
import signal
import subprocess
import tempfile
import time
from typing import IO


PROGRESS_INTERVAL_SECONDS = 60


class DeviceError(RuntimeError):
    pass


@dataclass(frozen=True)
class Application:
    """What the plumbing needs to know about the application it drives.

    ``setting_prefixes`` name the settings a launch clears from the inherited
    environment (``DEVICECTL_CHILD_`` followed by a prefix), because an
    inherited one would select a different guest. ``runtime_result`` matches
    the console line that ends a run and has one group, the runtime's result.
    """

    bundle_id: str
    setting_prefixes: tuple[str, ...]
    runtime_result: re.Pattern[str]


def device_command(*arguments: str) -> list[str]:
    return ["xcrun", "devicectl", *arguments]


def device_documents(
    device: str, application: Application, timeout: int = 120
) -> dict[str, int]:
    """The files in the application's Documents directory and their sizes."""
    with tempfile.TemporaryDirectory(prefix="ios-device-documents-") as name:
        report = pathlib.Path(name) / "files.json"
        subprocess.run(
            device_command(
                "device", "info", "files", "--device", device,
                "--domain-type", "appDataContainer",
                "--domain-identifier", application.bundle_id,
                "--subdirectory", "Documents",
                "--json-output", str(report),
                "--quiet", "--timeout", str(timeout),
            ),
            check=True,
            stdin=subprocess.DEVNULL,
            timeout=timeout + 30,
        )
        listing = json.loads(report.read_text(encoding="utf-8"))
    return {
        entry["relativePath"]: entry["metadata"]["size"]
        for entry in listing["result"]["files"]
        if not entry["resources"]["isDirectory"]
    }


def copy_to_device(
    device: str,
    application: Application,
    source: pathlib.Path,
    name: str,
    timeout: int,
) -> None:
    """Copy ``source`` to ``Documents/name``, never replacing a file there.

    A file of the same name and size is taken to be this one: payload names
    carry their digest, and an image name is its release.
    """
    if not name or "/" in name or name in (".", ".."):
        raise DeviceError(f"not a plain Documents name: {name!r}")
    size = source.stat().st_size
    existing = device_documents(device, application).get(name)
    if existing == size:
        print(f"the device already holds Documents/{name}", flush=True)
        return
    if existing is not None:
        raise DeviceError(
            f"the device holds a different Documents/{name}; this lane "
            "never replaces a file on a device"
        )
    subprocess.run(
        device_command(
            "device", "copy", "to", "--device", device,
            "--domain-type", "appDataContainer",
            "--domain-identifier", application.bundle_id,
            "--user", "mobile",
            "--source", str(source),
            "--destination", f"Documents/{name}",
            "--quiet", "--timeout", str(timeout),
        ),
        check=True,
        stdin=subprocess.DEVNULL,
        timeout=timeout + 30,
    )


def device_lock_state(device: str, timeout: int = 60) -> str:
    """``unlocked``, ``locked`` or ``unknown`` (a failed or unreadable query).

    A passcode-locked device refuses every launch, and only its owner can
    unlock it.
    """
    try:
        completed = subprocess.run(
            device_command(
                "device", "info", "lockState", "--device", device
            ),
            capture_output=True,
            text=True,
            stdin=subprocess.DEVNULL,
            timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    if completed.returncode != 0:
        return "unknown"
    if "passcodeRequired: false" in completed.stdout:
        return "unlocked"
    if "passcodeRequired: true" in completed.stdout:
        return "locked"
    return "unknown"


def install_app(device: str, app: pathlib.Path, timeout: int) -> None:
    subprocess.run(
        ["codesign", "--verify", "--deep", "--strict", str(app)],
        check=True,
        stdin=subprocess.DEVNULL,
    )
    subprocess.run(
        device_command(
            "device", "install", "app", "--device", device, str(app),
            "--quiet", "--timeout", str(timeout),
        ),
        check=True,
        stdin=subprocess.DEVNULL,
        timeout=timeout + 30,
    )


def device_launch_environment(
    application: Application, settings: Mapping[str, str]
) -> dict[str, str]:
    """devicectl forwards each ``DEVICECTL_CHILD_`` variable to the app.

    Inherited settings of the application's families are cleared, because one
    would select a different guest.
    """
    cleared = tuple(
        f"DEVICECTL_CHILD_{prefix}" for prefix in application.setting_prefixes
    )
    environment = {
        name: value
        for name, value in os.environ.items()
        if not name.startswith(cleared)
    }
    for name, value in settings.items():
        environment[f"DEVICECTL_CHILD_{name}"] = value
    return environment


def stop_console(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=10)


def runtime_result(application: Application, transcript: str) -> int | None:
    results = application.runtime_result.findall(transcript)
    return int(results[-1]) if results else None


class Console:
    """The console of a launched application, held until the block ends.

    ``devicectl device process launch --console`` stays attached for as long
    as the application runs and writes what it prints to ``log``; leaving the
    block interrupts it.
    """

    def __init__(
        self,
        device: str,
        application: Application,
        settings: Mapping[str, str],
        log: pathlib.Path,
    ) -> None:
        self.log = log
        self._command = device_command(
            "device", "process", "launch", "--device", device,
            "--console", "--terminate-existing", application.bundle_id,
        )
        self._environment = device_launch_environment(application, settings)
        self._output: IO[bytes] | None = None
        self._process: subprocess.Popen[bytes] | None = None

    def __enter__(self) -> "Console":
        self._output = self.log.open("wb")
        try:
            self._process = subprocess.Popen(
                self._command,
                stdout=self._output,
                stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL,
                env=self._environment,
            )
        except BaseException:
            self._output.close()
            raise
        return self

    def __exit__(self, *_exception: object) -> None:
        assert self._output is not None and self._process is not None
        try:
            stop_console(self._process)
        finally:
            self._output.close()

    def transcript(self) -> str:
        return self.log.read_text(encoding="utf-8", errors="replace")

    def size(self) -> int:
        return self.log.stat().st_size

    def running(self) -> bool:
        assert self._process is not None
        return self._process.poll() is None


def run_device_console(
    device: str,
    application: Application,
    settings: Mapping[str, str],
    log: pathlib.Path,
    *,
    timeout: int,
    stall_timeout: int,
    label: str,
    finished: Callable[[str], bool] | None = None,
) -> str:
    """Launch the application and hold its console until the run ends.

    The run ends at the application's own runtime-result line or when
    ``finished`` accepts the transcript. The wait is bounded by an overall
    deadline and by a stall deadline that only a growing console resets.
    """
    started = time.monotonic()
    deadline = started + timeout
    stall_deadline = started + stall_timeout
    next_update = started + PROGRESS_INTERVAL_SECONDS
    observed = 0
    with Console(device, application, settings, log) as console:
        while True:
            transcript = console.transcript()
            result = application.runtime_result
            if result.search(transcript) is not None or (
                finished is not None and finished(transcript)
            ):
                return transcript
            if not console.running():
                return transcript
            now = time.monotonic()
            size = console.size()
            if size > observed:
                observed = size
                stall_deadline = now + stall_timeout
            if now >= deadline:
                raise DeviceError(
                    f"{label} exceeded {timeout} seconds; see {log}"
                )
            if now >= stall_deadline:
                raise DeviceError(
                    f"the console of {label} stopped growing for "
                    f"{stall_timeout} seconds; see {log}"
                )
            if now >= next_update:
                print(
                    f"{label} is running; console_bytes={size}",
                    flush=True,
                )
                next_update = now + PROGRESS_INTERVAL_SECONDS
            time.sleep(1.0)
