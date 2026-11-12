#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the guest SME tests on one execution engine.

The tests need a host with SME and an engine at SmeSupport control or better
(the SME control core). While the host or the
engine falls short, the runner names the test, says why it is skipped and
exits with CTest's skip status. Otherwise it runs, with the guest-SME policy
at its default (on), in order:

- sme_probe in "kernel" mode as the guest's /init: the kernel's SME task
  state reached through sigreturn frames and ptrace alone (sme_probe.cpp);
- "spin" mode: a vCPU in streaming mode with ZA live while the emulator
  receives SIGUSR1 and SIGCHLD, then the kernel's copy of the state checked;
- "engine" mode: the control core executed by the engine;
- "engine-spin" mode: the signal test, with ZA loaded and stored
  by the engine around the signals;
- a boot per process-directed terminating signal (SIGTERM, SIGINT, SIGHUP)
  sent while a vCPU spins in streaming mode, which must end the emulator at
  once with the status it has without SME;
- glibc_za_probe.py on the image: glibc's __libc_arm_za_disable() commits a
  pending ZA lazy save before fork, posix_spawn and thread creation and in
  longjmp and siglongjmp; a signal handler installed through glibc runs
  outside streaming mode, returns into the region intact, or siglongjmps
  out of it; libgcc's SME support routines save, commit and restore ZA.

--minimum-support and --modes select less for a manual run. "--modes off"
instead boots the probe with the policy forced off (DARWIN_UML_GUEST_SME=0),
on any host and engine, and checks that the run shows no SME at all.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import signal
import subprocess
import sys
import time

# The script's own directory and the harness's are on sys.path.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "support"))

from guest import ConsoleProcess, Guest, GuestFile, emulator_arguments  # noqa: E402


SKIP = 77
SUPPORT_LEVELS = ("none", "control", "complete", "sme2")
PROBE_MODES = ("kernel", "spin", "engine", "engine-spin")
SME_MODES = (*PROBE_MODES, "terminate", "glibc")
ENGINE_MODES = ("engine", "engine-spin", "glibc")
# Each terminating signal and the emulator's status after it: the kernel's
# fatal handlers exit 128 + n for SIGINT and SIGTERM, and SIGHUP takes its
# default action. SIGQUIT is blocked on the carriers with them and takes the
# same path as SIGHUP, but its default action dumps core, which the host
# reports as a crash, so it is not sent.
TERMINATING_SIGNALS = {"SIGTERM": 128 + 15, "SIGINT": 128 + 2, "SIGHUP": -1}
SUMMARY = re.compile(
    rb"sme-probe: (?P<mode>[a-z-]+): (?P<failures>\d+) failure\(s\) in "
    rb"(?P<checks>\d+) check\(s\)"
)
SPINNING = re.compile(rb"sme-probe: spinning")
HOST_CRASH = re.compile(rb"SIGILL|[Ii]llegal instruction|host fault|Kernel panic")
GLIBC_PROBE = "/usr/local/libexec/uml-test/glibc_za_probe.py"


def host_has_sme() -> bool:
    result = subprocess.run(
        ["sysctl", "-n", "hw.optional.arm.FEAT_SME"],
        capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=30,
    )
    return result.returncode == 0 and result.stdout.strip() == "1"


def engine_support(tool: pathlib.Path, engine: str) -> str:
    output = subprocess.run(
        [str(tool)], capture_output=True, text=True, check=True,
        stdin=subprocess.DEVNULL, timeout=30,
    ).stdout
    levels = dict(field.split("=", 1) for field in output.split())
    return levels[engine]


def probe_arguments(arguments: argparse.Namespace, mode: str) -> list[str]:
    return [
        *emulator_arguments(
            arguments.emulator, arguments.rootfs, "512M",
            (f"ncpus={arguments.ncpus}", f"SME_PROBE_MODE={mode}",
             "console=tty0", "console=ttyS0", *arguments.kernel_argument),
            engine=arguments.engine,
        ),
    ]


def run_probe_boot(arguments: argparse.Namespace, mode: str,
                   terminating: str | None = None) -> int:
    """Boot sme_probe as /init in `mode`; returns its failure count."""
    command = probe_arguments(arguments, mode)
    # The launcher's per-run initramfs takes the probe as its /init.
    command.insert(command.index(str(arguments.emulator)) + 1,
                   f"--guest-init={arguments.probe}")
    with ConsoleProcess(command) as guest:
        if mode in ("spin", "engine-spin"):
            guest._read_until(SPINNING, arguments.timeout)
            # The marker precedes the sigreturn into streaming mode.
            time.sleep(1)
            if terminating is not None:
                os.kill(guest.process_id, getattr(signal, terminating))
                if not guest.wait_for_exit(5):
                    raise RuntimeError(f"{terminating}: the emulator did not exit")
                output = guest.output().encode()
                if HOST_CRASH.search(output):
                    raise RuntimeError(f"{terminating}: the emulator crashed")
                # A probe that reached its summary outlived the signal.
                expected = TERMINATING_SIGNALS[terminating]
                if guest.return_code != expected or SUMMARY.search(output):
                    raise RuntimeError(
                        f"{terminating}: the emulator exited with status "
                        f"{guest.return_code}, not {expected}, or after the "
                        "probe finished")
                print(f"PASS {terminating}: the emulator exited with status "
                      f"{expected} while streaming")
                return 0
            for _ in range(20):
                for host_signal in (signal.SIGUSR1, signal.SIGCHLD):
                    os.kill(guest.process_id, host_signal)
                time.sleep(0.05)
        match = guest._read_until(SUMMARY, arguments.timeout)
        guest.wait_for_exit(30)
        output = guest.output()
        for line in output.splitlines():
            if line.startswith(("PASS ", "FAIL ", "sme-probe: ")):
                print(line)
        if HOST_CRASH.search(output.encode()):
            raise RuntimeError(f"{mode}: the guest or the emulator crashed")
        if match.group("mode").decode() != mode or int(match.group("checks")) == 0:
            raise RuntimeError(f"{mode}: the probe ran no checks")
        return int(match.group("failures"))


def run_glibc(arguments: argparse.Namespace) -> int:
    with Guest(
        arguments.emulator, arguments.rootfs, "1G", "tmpfs",
        (f"ncpus={arguments.ncpus}", *arguments.kernel_argument),
        init_shell=True,
        engine=arguments.engine,
        payloads={GLIBC_PROBE: GuestFile(arguments.glibc_probe.read_bytes())},
    ) as guest:
        guest.boot_to_shell(arguments.timeout)
        result = guest.run_command(f"python3 -B {GLIBC_PROBE}", arguments.timeout)
        print(result.output.strip())
        guest.check_command("echo alive", 20)
        return result.return_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", required=True, help="the registered test name")
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), required=True)
    parser.add_argument("--probe", required=True, type=pathlib.Path)
    parser.add_argument("--glibc-probe", required=True, type=pathlib.Path)
    parser.add_argument("--engine-support-tool", type=pathlib.Path)
    parser.add_argument("--minimum-support", choices=SUPPORT_LEVELS,
                        default="control")
    parser.add_argument("--modes", default=",".join(SME_MODES),
                        help='SME modes, comma-separated, or "off" alone')
    parser.add_argument("--ncpus", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--kernel-argument", action="append", default=[])
    arguments = parser.parse_args()
    arguments.emulator = arguments.emulator.resolve()
    arguments.rootfs = arguments.rootfs.resolve()
    arguments.probe = arguments.probe.resolve()
    modes = arguments.modes.split(",")
    if modes == ["off"]:
        os.environ["DARWIN_UML_GUEST_SME"] = "0"
        if run_probe_boot(arguments, "off"):
            raise RuntimeError(f"{arguments.name}: the probe found SME with the "
                               "policy off")
        print(f"{arguments.name}: the run shows no SME with the policy off")
        return 0
    if unknown := set(modes) - set(SME_MODES):
        parser.error(f"unknown modes: {sorted(unknown)}")

    if not host_has_sme():
        print(f"{arguments.name}: SKIP: the host has no SME")
        return SKIP
    if arguments.minimum_support != "none":
        if arguments.engine_support_tool is None:
            parser.error("--engine-support-tool is required unless "
                         "--minimum-support none")
        level = engine_support(arguments.engine_support_tool, arguments.engine)
        if SUPPORT_LEVELS.index(level) < SUPPORT_LEVELS.index(
                arguments.minimum_support):
            print(f"{arguments.name}: SKIP: the {arguments.engine} engine's SME "
                  f"support is {level}; these tests need "
                  f"{arguments.minimum_support}")
            return SKIP
        if level == "none":
            modes = [mode for mode in modes if mode not in ENGINE_MODES]

    os.environ.pop("DARWIN_UML_GUEST_SME", None)
    failures = 0
    for mode in modes:
        print(f"== {arguments.name}: {mode}", flush=True)
        if mode in PROBE_MODES:
            failures += run_probe_boot(arguments, mode)
        elif mode == "terminate":
            for terminating in TERMINATING_SIGNALS:
                # The emulator inherits an ignored signal, as from nohup or
                # a non-interactive shell's background job, and honours it.
                number = getattr(signal, terminating)
                if signal.getsignal(number) == signal.SIG_IGN:
                    signal.signal(number, signal.SIG_DFL)
                run_probe_boot(arguments, "spin", terminating)
        else:
            failures += run_glibc(arguments)
    if failures:
        raise RuntimeError(f"{arguments.name}: {failures} failure(s)")
    print(f"{arguments.name}: every mode passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
