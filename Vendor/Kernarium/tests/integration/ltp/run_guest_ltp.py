#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run verified LTP cases in isolated Darwin UML guests."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import concurrent.futures
import dataclasses
import functools
import json
import os
import pathlib
import re
import shlex
import signal
import sys
import threading
import time
from collections.abc import Mapping


SUPPORT = pathlib.Path(__file__).resolve().parents[1] / "support"
sys.path.insert(0, str(SUPPORT))
from guest import (  # noqa: E402
    BOOT_TIMEOUT_SECONDS,
    CommandTimeout,
    Guest as GuestHarness,
    GuestFile,
    cleanup_stale_swap_images,
    size_bytes,
)


SUMMARY_VALUE = re.compile(
    r"^(passed|failed|broken|skipped|warnings)\s+(\d+)\s*$",
    re.IGNORECASE | re.MULTILINE,
)
RESULT_TAG = re.compile(r"^.*?:\d+: (TPASS|TFAIL|TBROK|TCONF|TWARN):", re.MULTILINE)
LEGACY_RESULT_TAG = re.compile(
    r"^\S+\s+\d+\s+(TPASS|TFAIL|TBROK|TCONF|TWARN)\s*:", re.MULTILINE
)
LEGACY_CHILD_RESULT_TAG = re.compile(
    r"\bChild process returned (TPASS|TFAIL|TBROK|TCONF|TWARN)\b"
)
KERNEL_FATAL = re.compile(
    r"^(BUG:|Oops:|Kernel panic|Unable to handle kernel)", re.MULTILINE
)
VALID_EXPECTATIONS = {
    "exit-zero",
    "pass",
    "hardware-exempt",
    "hardware-failure-exempt",
    "libc-exempt",
    "static-kernel-exempt",
    "userspace-exempt",
    "userspace-broken-exempt",
    "userspace-failure-exempt",
    "userspace-flaky-exempt",
    "userspace-legacy-matrix-exempt",
    "userspace-missing-exempt",
    "userspace-variable-matrix-exempt",
}
VALID_WORKSPACES = {"tmpfs", "ext2"}
HOST_PRESSURE_OPTION = "host-pressure"
SERIAL_OPTION = "serial"
CGROUP2_NSDELEGATE_OPTION = "cgroup2=nsdelegate"
KERNEL_ARGUMENT_PREFIX = "kernel-arg="
CGROUP1_CONTROLLER = re.compile(r"^[a-z0-9_]+$")
LTP_ROOT = "/opt/ltp"
MANIFEST_DIRECTORY = pathlib.Path(__file__).resolve().parent
# The shared manifest describes the arm64 guest. Another guest architecture
# overrides the expectations its ABI changes and disables its own omissions.
GUEST_ARCHITECTURES = ("arm64", "x86_64")
LTP_BIN = f"{LTP_ROOT}/testcases/bin"
LTP_OUTPUT = "/tmp/uml-ltp-output"
# The run's payload files, in its writable layer. The modules payload goes
# through /usr, because a merged-/usr image's /lib is a link a payload cannot
# pass; the check script proves LTP sees it at /lib/modules.
LTP_TOOLS = "/usr/local/libexec/uml-ltp-tools"
LTP_METADATA_CHECK = "/usr/local/libexec/uml-ltp-check-metadata"
LTP_MODULES_PAYLOAD_ROOT = "/usr/lib/modules"
LTP_MODULES_ROOT = "/lib/modules"
LTP_TIMEOUT_MUL = 4
# The manifest's case bounds and LTP_TIMEOUT_MUL are sized on the gadget
# engine. A JIT compiles each exec'd process's code from scratch, which costs
# the arm64 JIT about four times and the x86-64 JIT about 2.7 times what the
# gadget engine pays per process (darwin/cpu/arm64/jit/README.md#cold-code,
# darwin/cpu/x86_64/jit/README.md#tiered-execution), so a case run on a JIT
# gets its guest's factor on its bound and on LTP's own timers alike: the
# measured ratio and at least a quarter more, rounded up.
JIT_CASE_BOUND_FACTORS = {"arm64": 5, "x86_64": 4}
MODULES_BUILTIN_ENTRY = re.compile(
    r"^kernel/(?:[A-Za-z0-9._+-]+/)*[A-Za-z0-9._+-]+\.ko$"
)
KERNEL_RELEASE_VALUE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]*$")
# Keep every ordinary guest single-vCPU.  Workloads which intentionally
# exercise more than one CPU are manifest-tagged host-pressure, retain the full
# guest topology, and run alone at the end of the suite.
DEFAULT_PARALLEL_GUEST_CPUS = 1
_ACTIVE_GUESTS: set["Guest"] = set()
_ACTIVE_GUESTS_LOCK = threading.Lock()


def validate_modules_builtin(data: bytes) -> tuple[str, ...]:
    """Validate canonical Kbuild ``modules.builtin`` bytes."""
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError("modules.builtin is not UTF-8 text") from error
    if not text or not text.endswith("\n"):
        raise ValueError("modules.builtin must be nonempty and newline-terminated")
    entries = tuple(text.splitlines())
    if any(
        not MODULES_BUILTIN_ENTRY.fullmatch(entry)
        or any(part in (".", "..") for part in entry.split("/"))
        for entry in entries
    ):
        raise ValueError(
            "modules.builtin entries must be canonical kernel/.../*.ko paths"
        )
    if len(entries) != len(set(entries)):
        raise ValueError("modules.builtin contains duplicate entries")
    return entries


def load_modules_builtin(path: pathlib.Path) -> tuple[bytes, str]:
    """Load a kernel artifact's modules.builtin and the release beside it."""
    try:
        data = path.read_bytes()
    except OSError as error:
        raise ValueError(f"cannot read modules.builtin: {path}: {error}") from error
    validate_modules_builtin(data)
    release_path = path.parent / "kernel.release"
    try:
        release_text = release_path.read_text(encoding="utf-8")
    except OSError as error:
        raise ValueError(
            f"cannot read sibling kernel release: {release_path}: {error}"
        ) from error
    release_lines = release_text.splitlines()
    if (
        len(release_lines) != 1
        or not KERNEL_RELEASE_VALUE.fullmatch(release_lines[0])
    ):
        raise ValueError(f"invalid sibling kernel release file: {release_path}")
    return data, release_lines[0]


def static_kmod_config_check(
    kernel_config: str, config_reader: tuple[str, ...]
) -> tuple[str, ...]:
    """Shell lines that fail unless the kernel config says CONFIG_MODULES=n."""
    if not config_reader:
        raise ValueError("config reader command must be nonempty")
    reader = shlex.join(config_reader)
    return (
        f"kernel_config_text=$({reader} {shlex.quote(kernel_config)})",
        "modules_unset_count=$(printf '%s\\n' \"$kernel_config_text\" | "
        "grep -cx '# CONFIG_MODULES is not set' || true)",
        "modules_set_count=$(printf '%s\\n' \"$kernel_config_text\" | "
        "grep -c '^CONFIG_MODULES=' || true)",
        'if [ "$modules_unset_count" -ne 1 ] || '
        '[ "$modules_set_count" -ne 0 ]; then',
        "printf '%s\\n' 'LTP static-kmod metadata requires CONFIG_MODULES=n' >&2",
        "exit 1",
        "fi",
    )


def ltp_kmod_shim(
    module_names: tuple[str, ...],
    kernel_release: str,
    modules_root: str = LTP_MODULES_ROOT,
    kernel_config: str = "/proc/config.gz",
    config_reader: tuple[str, ...] = ("zcat",),
) -> str:
    """A fail-closed kmod that knows only the exact built-in module names.

    Installed as kmod, modprobe and rmmod; only ``modprobe`` and ``kmod
    modprobe`` of built-in names succeed, and only while loadable modules are
    disabled. Unsupported operations and parameters fail."""
    return "\n".join((
        "#!/bin/sh",
        "set -eu",
        f"modules_root={shlex.quote(modules_root)}",
        f"kernel_release={shlex.quote(kernel_release)}",
        *static_kmod_config_check(kernel_config, config_reader),
        'modules_builtin="$modules_root/$kernel_release/modules.builtin"',
        '[ -r "$modules_builtin" ] || exit 1',
        "tool=${0##*/}",
        'if [ "$tool" = kmod ]; then',
        '[ "${1-}" = modprobe ] || exit 1',
        "shift",
        "tool=modprobe",
        "fi",
        '[ "$tool" = modprobe ] || exit 1',
        "load_all=0",
        'case "${1-}" in',
        "-a|--all) load_all=1; shift ;;",
        "-r|--remove) exit 1 ;;",
        "-*) exit 1 ;;",
        "esac",
        '[ "$#" -gt 0 ] || exit 1',
        '[ "$load_all" -eq 1 ] || [ "$#" -eq 1 ] || exit 1',
        "for driver do",
        'case "$driver" in',
        "''|-*|*[!A-Za-z0-9._+-]*) exit 1 ;;",
        "esac",
        'case "$driver" in',
        *(f"{shlex.quote(name)}) ;;" for name in module_names),
        "*) exit 1 ;;",
        "esac",
        "done",
        "exit 0",
        "",
    ))


def ltp_metadata_check(
    kernel_release: str,
    modules_root: str = LTP_MODULES_ROOT,
    kernel_config: str = "/proc/config.gz",
    config_reader: tuple[str, ...] = ("zcat",),
    release_reader: tuple[str, ...] = ("uname", "-r"),
) -> str:
    """The script a booted guest runs before its first case.

    The metadata is accepted only for the kernel it came from, only while
    loadable modules are disabled, and only where LTP looks for it."""
    if not release_reader:
        raise ValueError("release reader command must be nonempty")
    return "\n".join((
        "#!/bin/sh",
        "set -eu",
        f"expected_kernel_release={shlex.quote(kernel_release)}",
        f"kernel_release=$({shlex.join(release_reader)})",
        'if [ "$kernel_release" != "$expected_kernel_release" ]; then',
        "printf '%s\\n' \"kernel release mismatch: expected "
        '$expected_kernel_release, running $kernel_release" >&2',
        "exit 1",
        "fi",
        *static_kmod_config_check(kernel_config, config_reader),
        f"modules_dir={shlex.quote(modules_root)}/$kernel_release",
        'for file in modules.builtin modules.dep; do',
        'if [ ! -f "$modules_dir/$file" ]; then',
        "printf '%s\\n' \"LTP cannot see $modules_dir/$file\" >&2",
        "exit 1",
        "fi",
        "done",
        "",
    ))


def ltp_builtin_driver_payloads(
    modules_builtin_data: bytes, expected_kernel_release: str
) -> dict[str, GuestFile]:
    """Expose authoritative Kbuild built-in metadata to LTP.

    ``TST_NEEDS_DRIVERS`` searches ``modules.dep`` and ``modules.builtin`` for
    ``/<driver>.ko``. The complete metadata comes from the exact kernel build
    and lands, with the kmod shim, in the run's writable layer; the check
    script proves it matches the running guest.
    """
    entries = validate_modules_builtin(modules_builtin_data)
    if not KERNEL_RELEASE_VALUE.fullmatch(expected_kernel_release):
        raise ValueError("expected kernel release is invalid")
    module_names = tuple(
        dict.fromkeys(
            entry.rsplit("/", 1)[-1].removesuffix(".ko") for entry in entries
        )
    )
    shim = GuestFile(
        ltp_kmod_shim(module_names, expected_kernel_release).encode(), 0o755
    )
    modules = f"{LTP_MODULES_PAYLOAD_ROOT}/{expected_kernel_release}"
    return {
        f"{modules}/modules.builtin": GuestFile(modules_builtin_data),
        f"{modules}/modules.dep": GuestFile(b""),
        **{f"{LTP_TOOLS}/{tool}": shim for tool in ("kmod", "modprobe", "rmmod")},
        LTP_METADATA_CHECK: GuestFile(
            ltp_metadata_check(expected_kernel_release).encode(), 0o755
        ),
    }


@dataclasses.dataclass(frozen=True)
class Case:
    scenario: str
    test_id: str
    command: str
    expectation: str
    timeout_seconds: int
    reason: str
    memory: str | None = None
    workspace: str = "tmpfs"
    swap: str | None = None
    cgroup1: str | None = None
    cgroup2_nsdelegate: bool = False
    host_pressure: bool = False
    serial: bool = False
    kernel_arguments: tuple[str, ...] = ()
    engine: str = "jit"
    ltp_timeout_mul: int = LTP_TIMEOUT_MUL

    @property
    def key(self) -> str:
        return f"{self.scenario}/{self.test_id}"


def case_bound_factor(guest_arch: str, engine: str) -> int:
    """How many times its gadget-sized bound a case gets on ``engine``."""
    if engine != "jit":
        return 1
    return JIT_CASE_BOUND_FACTORS.get(guest_arch, 1)


def bound_for_engine(case: Case, guest_arch: str, engine: str) -> Case:
    """``case`` as ``engine`` runs it, its bound and LTP's timers scaled."""
    factor = case_bound_factor(guest_arch, engine)
    return dataclasses.replace(
        case,
        engine=engine,
        timeout_seconds=case.timeout_seconds * factor,
        ltp_timeout_mul=LTP_TIMEOUT_MUL * factor,
    )


@dataclasses.dataclass(frozen=True)
class Result:
    case: Case
    return_code: int | None
    summary: dict[str, int]
    output: str
    error: str | None = None


def parse_result_counts(output: str) -> dict[str, int]:
    summary = {
        name.lower(): int(value) for name, value in SUMMARY_VALUE.findall(output)
    }
    if summary:
        return summary

    tag_to_name = {
        "TPASS": "passed",
        "TFAIL": "failed",
        "TBROK": "broken",
        "TCONF": "skipped",
        "TWARN": "warnings",
    }
    counts: dict[str, int] = {}
    tags = RESULT_TAG.findall(output) + LEGACY_RESULT_TAG.findall(output)
    if not tags:
        tags = LEGACY_CHILD_RESULT_TAG.findall(output)
    for tag in tags:
        name = tag_to_name[tag]
        counts[name] = counts.get(name, 0) + 1
    return counts


def kernel_failure(output: str) -> str | None:
    """Reject a nominal test pass if the guest reported kernel corruption."""
    match = KERNEL_FATAL.search(output)
    if match is None:
        return None
    line = output[match.start() :].splitlines()[0]
    return f"guest kernel reported a fatal diagnostic: {line}"


def ltp_guest_command(
    command: str,
    tmpdir: str = "/tmp/uml-ltp-case",
    timeout_mul: int = LTP_TIMEOUT_MUL,
) -> str:
    """Build the command environment used by LTP's scenario executor."""
    return (
        f"mkdir -p {tmpdir} {LTP_OUTPUT} && "
        f"chmod 1777 {tmpdir} {LTP_OUTPUT} && "
        f"cd {LTP_BIN} && "
        f"PATH={LTP_TOOLS}:{LTP_BIN}:$PATH "
        f"LTPROOT={LTP_ROOT} "
        f"TMPDIR={tmpdir} "
        f"LTP_TIMEOUT_MUL={timeout_mul} "
        f"sh -c {shlex.quote(command)}"
    )


class Guest(GuestHarness):
    """Shared guest harness with LTP result and health interpretation."""

    def boot_and_login(
        self, timeout_seconds: float = BOOT_TIMEOUT_SECONDS
    ) -> None:
        super().boot_and_login(timeout_seconds)
        result = self.run_command(LTP_METADATA_CHECK, timeout_seconds)
        if result.return_code != 0:
            raise RuntimeError(
                f"LTP built-in driver metadata check failed: {result.output}"
            )

    def run(self, case: Case) -> Result:
        try:
            result = self.run_command(
                ltp_guest_command(
                    case.command, timeout_mul=case.ltp_timeout_mul
                ),
                case.timeout_seconds,
            )
        except CommandTimeout as error:
            return Result(case, None, {}, error.output, str(error))
        summary = parse_result_counts(result.output)
        health_error = kernel_failure(result.output) or self._health_check()
        return Result(
            case,
            result.return_code,
            summary,
            result.output,
            health_error,
        )

    def _health_check(self, timeout_seconds: int = 30) -> str | None:
        """Verify that a completed test did not damage later guest execution."""
        try:
            result = self.run_command(
                "stty sane 2>/dev/null; zcat /proc/config.gz >/dev/null",
                timeout_seconds,
            )
        except (RuntimeError, TimeoutError) as error:
            return f"post-test guest health check failed: {error}"
        if result.return_code != 0:
            return (
                "post-test guest health check failed: "
                f"zcat /proc/config.gz exited {result.return_code}"
            )
        return None


def load_manifest(path: pathlib.Path) -> list[Case]:
    cases: list[Case] = []
    keys: set[str] = set()
    with path.open(encoding="utf-8") as manifest:
        for line_number, raw_line in enumerate(manifest, 1):
            line = raw_line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) < 6:
                raise ValueError(
                    f"{path}:{line_number}: expected at least six "
                    "tab-separated fields"
                )
            scenario, test_id, command, expectation, timeout, reason = fields[:6]
            memory = None
            workspace = "tmpfs"
            swap = None
            cgroup1 = None
            cgroup2_nsdelegate = False
            host_pressure = False
            serial = False
            kernel_arguments: list[str] = []
            for extra in fields[6:]:
                if extra in VALID_WORKSPACES:
                    workspace = extra
                elif extra == HOST_PRESSURE_OPTION:
                    if host_pressure:
                        raise ValueError(
                            f"{path}:{line_number}: duplicate "
                            f"{HOST_PRESSURE_OPTION!r} option"
                        )
                    host_pressure = True
                elif extra == SERIAL_OPTION:
                    if serial:
                        raise ValueError(
                            f"{path}:{line_number}: duplicate "
                            f"{SERIAL_OPTION!r} option"
                        )
                    serial = True
                elif extra.startswith("swap=") and swap is None:
                    swap = extra.removeprefix("swap=")
                    size_bytes(swap)
                elif extra.startswith("cgroup1=") and cgroup1 is None:
                    cgroup1 = extra.removeprefix("cgroup1=")
                    if CGROUP1_CONTROLLER.fullmatch(cgroup1) is None:
                        raise ValueError(
                            f"{path}:{line_number}: invalid cgroup v1 "
                            f"controller {cgroup1!r}"
                        )
                elif extra == CGROUP2_NSDELEGATE_OPTION:
                    if cgroup2_nsdelegate:
                        raise ValueError(
                            f"{path}:{line_number}: duplicate "
                            f"{CGROUP2_NSDELEGATE_OPTION!r} option"
                        )
                    cgroup2_nsdelegate = True
                elif extra.startswith(KERNEL_ARGUMENT_PREFIX):
                    kernel_argument = extra.removeprefix(KERNEL_ARGUMENT_PREFIX)
                    if not kernel_argument:
                        raise ValueError(
                            f"{path}:{line_number}: empty kernel argument"
                        )
                    if kernel_argument in kernel_arguments:
                        raise ValueError(
                            f"{path}:{line_number}: duplicate kernel argument "
                            f"{kernel_argument!r}"
                        )
                    kernel_arguments.append(kernel_argument)
                elif extra and memory is None:
                    memory = extra
                else:
                    raise ValueError(
                        f"{path}:{line_number}: invalid manifest option {extra!r}"
                    )
            if cgroup1 is not None and cgroup2_nsdelegate:
                raise ValueError(
                    f"{path}:{line_number}: cgroup v1 and cgroup v2 "
                    "resource options are mutually exclusive"
                )
            if host_pressure and serial:
                raise ValueError(
                    f"{path}:{line_number}: {HOST_PRESSURE_OPTION!r} and "
                    f"{SERIAL_OPTION!r} options are mutually exclusive"
                )
            if expectation not in VALID_EXPECTATIONS:
                raise ValueError(
                    f"{path}:{line_number}: invalid expectation {expectation!r}"
                )
            case = Case(
                scenario=scenario,
                test_id=test_id,
                command=command,
                expectation=expectation,
                timeout_seconds=int(timeout),
                reason=reason,
                memory=memory,
                workspace=workspace,
                swap=swap,
                cgroup1=cgroup1,
                cgroup2_nsdelegate=cgroup2_nsdelegate,
                host_pressure=host_pressure,
                serial=serial,
                kernel_arguments=tuple(kernel_arguments),
            )
            if case.key in keys:
                raise ValueError(f"{path}:{line_number}: duplicate case {case.key}")
            keys.add(case.key)
            cases.append(case)
    if not cases:
        raise ValueError(f"{path}: manifest contains no verified cases")
    return cases


def load_expectation_overrides(
    path: pathlib.Path, cases: list[Case]
) -> list[Case]:
    """Apply one guest architecture's expectation overrides to the manifest."""
    by_key = {case.key: case for case in cases}
    overrides: dict[str, tuple[str, str]] = {}
    with path.open(encoding="utf-8") as manifest:
        for line_number, raw_line in enumerate(manifest, 1):
            line = raw_line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) != 4 or not all(fields):
                raise ValueError(
                    f"{path}:{line_number}: expected four nonempty "
                    "tab-separated fields"
                )
            scenario, test_id, expectation, reason = fields
            key = f"{scenario}/{test_id}"
            if key in overrides:
                raise ValueError(f"{path}:{line_number}: duplicate case {key}")
            if key not in by_key:
                raise ValueError(f"{path}:{line_number}: unknown case {key}")
            if expectation not in VALID_EXPECTATIONS:
                raise ValueError(
                    f"{path}:{line_number}: invalid expectation {expectation!r}"
                )
            if expectation == by_key[key].expectation:
                raise ValueError(
                    f"{path}:{line_number}: {key} already expects {expectation}"
                )
            overrides[key] = (expectation, reason)
    return [
        dataclasses.replace(
            case,
            expectation=overrides[case.key][0],
            reason=overrides[case.key][1],
        )
        if case.key in overrides
        else case
        for case in cases
    ]


def load_disabled_manifest(path: pathlib.Path) -> dict[str, str]:
    """Load cases intentionally omitted from this emulator's LTP campaign."""
    disabled: dict[str, str] = {}
    with path.open(encoding="utf-8") as manifest:
        for line_number, raw_line in enumerate(manifest, 1):
            line = raw_line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) != 3:
                raise ValueError(
                    f"{path}:{line_number}: expected three tab-separated fields"
                )
            scenario, test_id, reason = fields
            key = f"{scenario}/{test_id}"
            if not scenario or not test_id or not reason:
                raise ValueError(
                    f"{path}:{line_number}: disabled case fields must be nonempty"
                )
            if key in disabled:
                raise ValueError(
                    f"{path}:{line_number}: duplicate disabled case {key}"
                )
            disabled[key] = reason
    return disabled


def run_case(
    emulator: pathlib.Path,
    rootfs: pathlib.Path,
    memory: str,
    case: Case,
    *,
    driver_metadata: Mapping[str, GuestFile],
) -> Result:
    guest: Guest | None = None
    try:
        guest = Guest(
            emulator, rootfs, memory, case.workspace,
            kernel_arguments=case.kernel_arguments,
            swap_size=case.swap, cgroup1=case.cgroup1,
            cgroup2_nsdelegate=case.cgroup2_nsdelegate, overlay_shell=True,
            engine=case.engine, payloads=driver_metadata,
        )
        with _ACTIVE_GUESTS_LOCK:
            _ACTIVE_GUESTS.add(guest)
        guest.boot_and_login()
        return guest.run(case)
    except (OSError, RuntimeError, TimeoutError) as error:
        output = guest.output() if guest is not None else ""
        return Result(case, None, {}, output, str(error))
    finally:
        if guest is not None:
            try:
                guest.close()
            finally:
                with _ACTIVE_GUESTS_LOCK:
                    _ACTIVE_GUESTS.discard(guest)


def terminate_active_guests() -> None:
    """Stop every currently running worker guest after host interruption."""
    with _ACTIVE_GUESTS_LOCK:
        guests = tuple(_ACTIVE_GUESTS)
    for guest in guests:
        guest.terminate()


def limit_parallel_guest_cpus(case: Case, cpu_count: int) -> Case:
    """Give an ordinary guest exactly the requested single-CPU topology."""
    kernel_arguments = tuple(
        argument for argument in case.kernel_arguments
        if not argument.startswith("ncpus=")
    )
    return dataclasses.replace(
        case,
        kernel_arguments=kernel_arguments + (f"ncpus={cpu_count}",),
    )


def cases_from(cases: list[Case], key: str) -> list[Case]:
    """Return the manifest suffix beginning at key."""
    for index, case in enumerate(cases):
        if case.key == key:
            return cases[index:]
    raise ValueError(f"unknown start case: {key}")


def select_host_pressure_cases(
    cases: list[Case], run_host_pressure: bool
) -> tuple[list[Case], int]:
    """Exclude host-pressure cases unless they were explicitly requested."""
    pressure_count = sum(case.host_pressure for case in cases)
    if run_host_pressure:
        return cases, 0
    return [case for case in cases if not case.host_pressure], pressure_count


def run_cases(
    emulator: pathlib.Path,
    rootfs: pathlib.Path,
    memory: str,
    cases: list[Case],
    jobs: int,
    worker,
    parallel_guest_cpus: int = DEFAULT_PARALLEL_GUEST_CPUS,
):
    regular_cases = [
        case for case in cases if not case.host_pressure and not case.serial
    ]
    serial_cases = [case for case in cases if case.serial]
    pressure_cases = [case for case in cases if case.host_pressure]
    if jobs == 1:
        for index, case in enumerate(regular_cases, 1):
            case = limit_parallel_guest_cpus(case, parallel_guest_cpus)
            yield index, worker(emulator, rootfs, case.memory or memory, case)
    elif regular_cases:
        executor = concurrent.futures.ThreadPoolExecutor(
            max_workers=jobs, thread_name_prefix="guest-ltp"
        )
        try:
            futures = {
                executor.submit(
                    worker, emulator, rootfs, case.memory or memory,
                    limit_parallel_guest_cpus(case, parallel_guest_cpus),
                ): index
                for index, case in enumerate(regular_cases, 1)
            }
            for future in concurrent.futures.as_completed(futures):
                yield futures[future], future.result()
        except BaseException:
            for future in futures:
                future.cancel()
            terminate_active_guests()
            executor.shutdown(wait=True, cancel_futures=True)
            raise
        else:
            executor.shutdown(wait=True)

    first_serial_index = len(regular_cases) + 1
    for index, case in enumerate(serial_cases, first_serial_index):
        yield index, worker(emulator, rootfs, case.memory or memory, case)

    first_pressure_index = len(regular_cases) + len(serial_cases) + 1
    for index, case in enumerate(pressure_cases, first_pressure_index):
        yield index, worker(emulator, rootfs, case.memory or memory, case)


def accepted(result: Result) -> bool:
    if result.error is not None or result.return_code is None:
        return False
    code = result.return_code
    passed = result.summary.get("passed", 0)
    failed = result.summary.get("failed", 0)
    broken = result.summary.get("broken", 0)
    skipped = result.summary.get("skipped", 0)
    warnings = result.summary.get("warnings", 0)
    expectation = result.case.expectation
    # Outcomes more than one expectation accepts.
    clean_pass = (
        code == 0 and passed > 0 and failed == 0 and broken == 0 and skipped == 0
    )
    warning_only = (
        code == 4
        and passed > 0
        and failed == 0
        and broken == 0
        and skipped == 0
        and warnings > 0
    )
    if expectation == "exit-zero":
        return code == 0 and failed == 0 and broken == 0 and skipped == 0
    if expectation == "pass":
        return clean_pass or warning_only
    if expectation in ("userspace-failure-exempt", "hardware-failure-exempt"):
        return code != 0 and failed > 0 and broken == 0
    if expectation == "userspace-legacy-matrix-exempt":
        return (
            code == 0 and passed > 0 and failed > 0 and broken == 0 and skipped == 0
        )
    if expectation == "userspace-variable-matrix-exempt":
        mixed_result = code != 0 and passed > 0 and (failed > 0 or broken > 0)
        return (clean_pass and warnings == 0) or warning_only or mixed_result
    if expectation == "userspace-flaky-exempt":
        partial_break = code != 0 and passed > 0 and failed == 0 and broken > 0
        return clean_pass or warning_only or partial_break
    if expectation == "userspace-broken-exempt":
        return (
            code == 2 and passed == 0 and failed == 0 and broken > 0 and skipped == 0
        )
    if expectation == "userspace-missing-exempt":
        return code == 127 and not result.summary
    if failed != 0 or broken != 0:
        return False
    if skipped > 0:
        return code in (0, 32)
    return expectation == "userspace-exempt" and code == 0 and not result.summary


def parse_arguments(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--manifest", required=True, type=pathlib.Path)
    parser.add_argument(
        "--modules-builtin",
        required=True,
        type=pathlib.Path,
        help=(
            "the booted kernel artifact's modules.builtin, with kernel.release "
            "beside it"
        ),
    )
    parser.add_argument(
        "--engine",
        choices=("gadgets", "jit"),
        default="jit",
        help=(
            "userspace execution engine for every guest; the JIT multiplies "
            "every case bound and LTP_TIMEOUT_MUL by the guest's factor ("
            + ", ".join(
                f"{guest_arch} {factor}"
                for guest_arch, factor in JIT_CASE_BOUND_FACTORS.items()
            )
            + ") (default: jit)"
        ),
    )
    parser.add_argument(
        "--guest-arch",
        choices=GUEST_ARCHITECTURES,
        default="arm64",
        help=(
            "guest userspace architecture; x86_64 applies "
            "x86_64/expectations.tsv and x86_64/disabled-tests.tsv "
            "(default: arm64)"
        ),
    )
    parser.add_argument(
        "--results-dir",
        type=pathlib.Path,
        help="write raw per-case output and a machine-readable campaign summary",
    )
    parser.add_argument(
        "--disabled-manifest",
        type=pathlib.Path,
        default=pathlib.Path(__file__).with_name("disabled-tests.tsv"),
        help=(
            "cases intentionally omitted for this emulator "
            "(default: tests/integration/ltp/disabled-tests.tsv)"
        ),
    )
    parser.add_argument(
        "--memory",
        default="1024M",
        help="guest RAM used for each isolated case (default: 1024M)",
    )
    parser.add_argument(
        "--case",
        action="append",
        default=[],
        help="run only SCENARIO/TEST_ID (repeatable)",
    )
    parser.add_argument(
        "--start-at",
        help="run the manifest suffix beginning at SCENARIO/TEST_ID",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="print full guest output only for failed cases",
    )
    parser.add_argument(
        "--fail-fast",
        action="store_true",
        help="terminate active guests and stop after the first rejected result",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=1,
        help="number of isolated guest cases to run concurrently (default: 1)",
    )
    parser.add_argument(
        "--parallel-guest-cpus",
        type=int,
        default=DEFAULT_PARALLEL_GUEST_CPUS,
        help=(
            "vCPUs per ordinary guest; "
            "serial and host-pressure cases retain their configured SMP "
            f"topology (default: {DEFAULT_PARALLEL_GUEST_CPUS})"
        ),
    )
    parser.add_argument(
        "--run-host-pressure",
        action="store_true",
        help=(
            "include cases tagged host-pressure; these are excluded by default "
            "and run serially after ordinary and serial-only cases"
        ),
    )
    parser.add_argument(
        "--candidate",
        nargs=3,
        metavar=("SCENARIO", "TEST_ID", "COMMAND"),
        help="run one not-yet-verified case without adding it to the manifest",
    )
    parser.add_argument(
        "--candidate-expectation",
        choices=sorted(VALID_EXPECTATIONS),
        default="pass",
        help="expected outcome for --candidate (default: pass)",
    )
    parser.add_argument(
        "--candidate-timeout",
        type=int,
        default=300,
        help=(
            "the candidate's bound in seconds on the gadget engine "
            "(default: 300)"
        ),
    )
    parser.add_argument(
        "--candidate-memory",
        help="override guest RAM for the single --candidate case",
    )
    parser.add_argument(
        "--candidate-workspace",
        choices=sorted(VALID_WORKSPACES),
        default="tmpfs",
        help="volatile /tmp filesystem for --candidate (default: tmpfs)",
    )
    parser.add_argument(
        "--candidate-swap",
        help="attach and enable a disposable sparse swap disk for the candidate",
    )
    parser.add_argument(
        "--candidate-cgroup1",
        help="mount one legacy cgroup v1 controller for the candidate",
    )
    parser.add_argument(
        "--candidate-cgroup2-nsdelegate",
        action="store_true",
        help="mount a cgroup v2 hierarchy with nsdelegate for the candidate",
    )
    parser.add_argument(
        "--candidate-kernel-argument",
        action="append",
        default=[],
        help=(
            "append one kernel command-line argument for the candidate "
            "(repeatable)"
        ),
    )
    return parser.parse_args(argv)


def main() -> int:
    arguments = parse_arguments()
    cleanup_stale_swap_images()
    for path in (
        arguments.emulator,
        arguments.rootfs,
        arguments.manifest,
        arguments.disabled_manifest,
        arguments.modules_builtin,
    ):
        if not path.is_file():
            print(f"missing required file: {path}", file=sys.stderr)
            return 2
    try:
        modules_builtin_data, expected_kernel_release = load_modules_builtin(
            arguments.modules_builtin
        )
        driver_metadata = ltp_builtin_driver_payloads(
            modules_builtin_data, expected_kernel_release
        )
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 2
    cases = load_manifest(arguments.manifest)
    disabled = load_disabled_manifest(arguments.disabled_manifest)
    if arguments.guest_arch != "arm64":
        architecture = MANIFEST_DIRECTORY / arguments.guest_arch
        try:
            cases = load_expectation_overrides(
                architecture / "expectations.tsv", cases
            )
            for key, reason in load_disabled_manifest(
                architecture / "disabled-tests.tsv"
            ).items():
                if key in disabled:
                    raise ValueError(
                        f"{arguments.guest_arch} disables {key} twice"
                    )
                disabled[key] = reason
        except (OSError, ValueError) as error:
            print(str(error), file=sys.stderr)
            return 2
    if arguments.jobs < 1:
        print("--jobs must be at least 1", file=sys.stderr)
        return 2
    if arguments.parallel_guest_cpus < 1:
        print("--parallel-guest-cpus must be at least 1", file=sys.stderr)
        return 2
    if sum(bool(value) for value in (
        arguments.candidate, arguments.case, arguments.start_at
    )) > 1:
        print(
            "--candidate, --case, and --start-at are mutually exclusive",
            file=sys.stderr,
        )
        return 2
    if arguments.candidate:
        if (
            arguments.candidate_cgroup1
            and arguments.candidate_cgroup2_nsdelegate
        ):
            print(
                "--candidate-cgroup1 and --candidate-cgroup2-nsdelegate "
                "are mutually exclusive",
                file=sys.stderr,
            )
            return 2
        if (
            any(not value for value in arguments.candidate_kernel_argument)
            or len(set(arguments.candidate_kernel_argument))
            != len(arguments.candidate_kernel_argument)
        ):
            print(
                "candidate kernel arguments must be nonempty and unique",
                file=sys.stderr,
            )
            return 2
        scenario, test_id, command = arguments.candidate
        candidate_key = f"{scenario}/{test_id}"
        if candidate_key in disabled:
            print(
                f"disabled case {candidate_key}: {disabled[candidate_key]}",
                file=sys.stderr,
            )
            return 2
        cases = [
            Case(
                scenario=scenario,
                test_id=test_id,
                command=command,
                expectation=arguments.candidate_expectation,
                timeout_seconds=arguments.candidate_timeout,
                reason="candidate awaiting review",
                memory=arguments.candidate_memory,
                workspace=arguments.candidate_workspace,
                swap=arguments.candidate_swap,
                cgroup1=arguments.candidate_cgroup1,
                cgroup2_nsdelegate=arguments.candidate_cgroup2_nsdelegate,
                kernel_arguments=tuple(arguments.candidate_kernel_argument),
            )
        ]
    elif arguments.case:
        selected = set(arguments.case)
        disabled_selected = selected & disabled.keys()
        if disabled_selected:
            details = ", ".join(
                f"{key} ({disabled[key]})" for key in sorted(disabled_selected)
            )
            print(f"disabled cases requested: {details}", file=sys.stderr)
            return 2
        cases = [case for case in cases if case.key in selected]
        missing = selected - {case.key for case in cases}
        if missing:
            print(f"unknown cases: {', '.join(sorted(missing))}", file=sys.stderr)
            return 2
    else:
        disabled_present = [case for case in cases if case.key in disabled]
        if disabled_present:
            cases = [case for case in cases if case.key not in disabled]
            print(
                f"Skipping {len(disabled_present)} emulator-disabled cases",
                flush=True,
            )
        if arguments.start_at:
            try:
                cases = cases_from(cases, arguments.start_at)
            except ValueError as error:
                print(str(error), file=sys.stderr)
                return 2

    cases, skipped_pressure_count = select_host_pressure_cases(
        cases, arguments.run_host_pressure
    )
    cases = [
        bound_for_engine(case, arguments.guest_arch, arguments.engine)
        for case in cases
    ]
    bound_factor = case_bound_factor(arguments.guest_arch, arguments.engine)
    if bound_factor != 1:
        print(
            f"Case bounds and LTP_TIMEOUT_MUL are {bound_factor} times the "
            f"manifest's for the {arguments.guest_arch} {arguments.engine}",
            flush=True,
        )
    if skipped_pressure_count:
        print(
            f"Skipping {skipped_pressure_count} host-pressure cases by default; "
            "pass --run-host-pressure to include them",
            flush=True,
        )

    failures = 0
    failed_cases: list[Result] = []
    completed = 0
    run_started = time.monotonic()
    results_directory = (
        arguments.results_dir.resolve() if arguments.results_dir else None
    )
    campaign_records: list[dict[str, object]] = []
    if results_directory is not None:
        results_directory.mkdir(parents=True, exist_ok=True)
    emulator = arguments.emulator.resolve()
    rootfs = arguments.rootfs.resolve()
    if arguments.jobs > 1 and cases:
        regular_count = sum(
            not case.host_pressure and not case.serial for case in cases
        )
        serial_count = sum(case.serial for case in cases)
        pressure_count = sum(case.host_pressure for case in cases)
        print(
            f"Running {regular_count} regular isolated guests with "
            f"{arguments.jobs} worker threads and "
            f"{arguments.parallel_guest_cpus} vCPU per guest",
            flush=True,
        )
        if serial_count:
            print(
                f"Running {serial_count} serial-only cases one at a time "
                "after regular cases",
                flush=True,
            )
        if pressure_count:
            print(
                f"Running {pressure_count} host-pressure cases serially "
                "after regular cases",
                flush=True,
            )
    for index, result in run_cases(
        emulator,
        rootfs,
        arguments.memory,
        cases,
        arguments.jobs,
        worker=functools.partial(run_case, driver_metadata=driver_metadata),
        parallel_guest_cpus=arguments.parallel_guest_cpus,
    ):
        completed += 1
        case = result.case
        print(f"[{index}/{len(cases)}] {case.key}: {case.command}", flush=True)
        if result.output and not arguments.quiet:
            print(result.output)
        if result.error:
            print(f"HARNESS ERROR: {result.error}")
        result_accepted = accepted(result)
        outcome = "PASS" if result_accepted else "FAIL"
        campaign_records.append(
            {
                "case": case.key,
                "engine": arguments.engine,
                "accepted": result_accepted,
                "return_code": result.return_code,
                "summary": result.summary,
                "error": result.error,
            }
        )
        if results_directory is not None:
            artifact_name = case.key.replace("/", "__") + ".log"
            (results_directory / artifact_name).write_text(
                result.output, encoding="utf-8"
            )
        if outcome == "FAIL" and arguments.quiet and result.output:
            print(result.output)
        print(
            f"{outcome} {case.key}: rc={result.return_code} "
            f"summary={result.summary}",
            flush=True,
        )
        failures += outcome == "FAIL"
        if outcome == "FAIL":
            failed_cases.append(result)
        if outcome == "FAIL" and arguments.fail_fast:
            print("Stopping after first failure (--fail-fast)", flush=True)
            break
    if failed_cases:
        print(
            "Rejected cases: "
            + ", ".join(result.case.key for result in failed_cases),
            flush=True,
        )
        for result in failed_cases:
            detail = (
                f"rc={result.return_code} summary={result.summary}"
            )
            if result.error:
                detail += f" error={result.error}"
            print(
                f"Rejected case detail: {result.case.key}: {detail}",
                flush=True,
            )
    print(
        f"Completed {completed} cases: {completed - failures} accepted, "
        f"{failures} failed in {time.monotonic() - run_started:.1f}s",
        flush=True,
    )
    if results_directory is not None:
        campaign_summary = {
            "schema_version": 1,
            "engine": arguments.engine,
            "case_bound_factor": bound_factor,
            "elapsed_seconds": time.monotonic() - run_started,
            "completed": completed,
            "failures": failures,
            "cases": campaign_records,
        }
        (results_directory / "summary.json").write_text(
            json.dumps(campaign_summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
