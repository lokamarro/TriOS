# SPDX-License-Identifier: GPL-2.0-or-later
"""Run a static guest probe installed by the per-run initramfs.

A runner hands over the probe and any fixtures it reads. They reach the guest
as payload files in one directory, beside a SHA-256 list the guest checks
before anything runs. A disposable overlay-shell guest runs the probe there,
and the run fails on a missing PASS marker, an unclean shutdown, or a kernel
failure in the transcript. A failure names the probe, its exit status, the step
that failed and the last thing it said.
"""

from __future__ import annotations
# Imported support must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import pathlib
import shlex
import struct
from collections.abc import Mapping, Sequence

from guest import Guest, GuestFile


BOOT_TIMEOUT_SECONDS = 120
SHUTDOWN_TIMEOUT_SECONDS = 45
REPORT_TIMEOUT_SECONDS = 20
PROBE_SIZE_LIMIT = 8 * 1024 * 1024
# ELF e_machine values of the x86_64 and AArch64 guest ABIs.
GUEST_MACHINES = (62, 183)
PROBE_DIRECTORY = "/usr/local/libexec/uml-probe"
CHECKSUMS = "SHA256SUMS"
RESULT_LOG = "/tmp/uml-probe-result.log"
# Written to RESULT_LOG before each step, so that the last one names the step a
# failure came from, including one that says nothing itself.
STEP_MARKER = "UML_PROBE_STEP: "
KERNEL_FAILURE_MARKERS = (
    "Kernel panic",
    "BUG:",
    "Oops:",
    "Illegal instruction",
    "detected stalls on CPUs",
)


def parse_arguments(
    description: str | None, engines: Sequence[str] = (), gpu: bool = False
) -> argparse.Namespace:
    """Parse a probe runner's command line and check that the probe is one.

    A runner of several engines names them and receives --engine; one that can
    boot its guest with the GPU receives --gpu."""
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--probe", required=True, type=pathlib.Path)
    if engines:
        parser.add_argument("--engine", required=True, choices=tuple(engines))
    if gpu:
        parser.add_argument("--gpu", action="store_true")
    parser.add_argument("--log", type=pathlib.Path)
    arguments = parser.parse_args()
    binary = arguments.probe.read_bytes()
    if (
        not 64 <= len(binary) <= PROBE_SIZE_LIMIT
        or binary[:6] != b"\x7fELF\x02\x01"
    ):
        parser.error("probe must be a bounded little-endian ELF64 executable")
    if struct.unpack_from("<H", binary, 18)[0] not in GUEST_MACHINES:
        parser.error("probe must use the x86_64 or AArch64 guest ABI")
    return arguments


def probe_payloads(
    probe: bytes, fixtures: Mapping[str, bytes]
) -> dict[str, GuestFile]:
    """The probe, its fixtures and their checksum list, as guest payloads.

    Fixture names are relative to the probe's directory."""
    if "probe" in fixtures or CHECKSUMS in fixtures:
        raise ValueError(f"a fixture cannot replace probe or {CHECKSUMS}")
    files = {"probe": probe, **fixtures}
    checksums = "".join(
        f"{hashlib.sha256(contents).hexdigest()}  {name}\n"
        for name, contents in sorted(files.items())
    )
    payloads = {
        f"{PROBE_DIRECTORY}/{name}": GuestFile(
            contents, 0o755 if name == "probe" else 0o644
        )
        for name, contents in files.items()
    }
    payloads[f"{PROBE_DIRECTORY}/{CHECKSUMS}"] = GuestFile(checksums.encode())
    return payloads


def failure_message(
    subject: str, probe: str, status: int, output: str, pass_marker: str
) -> str:
    """Say which probe failed, how, in which step and with what last words."""
    step = "before any step"
    last = ""
    for line in output.splitlines():
        line = line.strip()
        if line.startswith(STEP_MARKER):
            step = line[len(STEP_MARKER) :]
        elif line:
            last = line
    if status == 0:
        reason = f"probe {probe} did not print {pass_marker!r}"
    else:
        reason = f"probe {probe} exited {status} in step {step!r}"
    return f"guest {subject} failed: {reason}: {last or 'it printed nothing'}"


def run_probe(
    arguments: argparse.Namespace,
    *,
    subject: str,
    kernel_arguments: Sequence[str],
    probe_timeout_seconds: int,
    command_timeout_seconds: int,
    pass_marker: str,
    fixtures: Mapping[str, bytes] = {},
    probe_arguments: Sequence[str] = (),
    precondition: str | None = None,
    engine: str = "jit",
) -> None:
    """Boot a guest with the probe and fixtures installed and run ``./probe``.

    precondition runs before the checksum check. Every step's output is kept
    in RESULT_LOG, which must contain pass_marker; a step that fails ends the
    run with a message that names the probe, its status, the step and the last
    line it printed."""
    probe = shlex.join(
        ("timeout", str(probe_timeout_seconds), "./probe", *probe_arguments)
    )
    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "2G",
        "tmpfs",
        kernel_arguments=kernel_arguments,
        overlay_shell=True,
        engine=engine,
        payloads=probe_payloads(arguments.probe.read_bytes(), fixtures),
    ) as guest:
        try:
            guest.boot_and_login(BOOT_TIMEOUT_SECONDS)
            steps: list[tuple[str, str]] = []
            if precondition:
                steps.append(("precondition", precondition))
            steps += [
                ("guest page size", 'test "$(getconf PAGESIZE)" = 4096'),
                ("payload directory", f"cd {PROBE_DIRECTORY}"),
                ("payload checksums", f"sha256sum -c {CHECKSUMS} >/dev/null"),
                ("probe", probe),
            ]
            script = "; ".join(
                f"echo {shlex.quote(STEP_MARKER + name)}; {command}"
                for name, command in steps
            )
            # The session shell is the guest's PID 1, so a failing step under
            # its own `set -e` would end the guest before it could say why. A
            # subshell fails alone, and everything it printed is kept.
            result = guest.run_command(
                f"( set -e; {script} ) >{RESULT_LOG} 2>&1",
                command_timeout_seconds,
            )
            report = guest.run_command(
                f"cat {RESULT_LOG}", REPORT_TIMEOUT_SECONDS
            )
            print(report.output, flush=True)
            if (
                result.return_code != 0
                or report.return_code != 0
                or pass_marker not in report.output
            ):
                raise RuntimeError(
                    failure_message(
                        subject,
                        arguments.probe.name,
                        result.return_code,
                        report.output,
                        pass_marker,
                    )
                )
            if (
                not guest.shutdown(SHUTDOWN_TIMEOUT_SECONDS)
                or guest.return_code != 0
            ):
                raise RuntimeError(f"{subject} guest did not shut down cleanly")
            transcript = guest.output()
            if any(marker in transcript for marker in KERNEL_FAILURE_MARKERS):
                raise RuntimeError(f"{subject} guest reported a kernel failure")
        finally:
            if arguments.log:
                arguments.log.parent.mkdir(parents=True, exist_ok=True)
                arguments.log.write_text(guest.output())
