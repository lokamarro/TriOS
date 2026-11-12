#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise every benchmark entry point without booting a guest."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import contextlib
import hashlib
import importlib.util
import io
import pathlib
import subprocess
import sys
import tempfile
import types
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2] / "perf"
TOOLS = pathlib.Path(__file__).resolve().parents[3] / "tools"
RUNNERS = (
    "run_guest_geekbench.py",
    "run_guest_command_bench.py",
    "run_guest_exec_bench.py",
    "run_guest_link_bench.py",
)
MOBILE_RUNNERS = (
    "run_ios_device_discard_bench.py",
    "run_ios_device_workload.py",
)


class GuestConstructed(Exception):
    """Stops a runner at its Guest construction."""


# The arguments each runner needs, beyond the common ones, to reach its Guest.
RUNNER_ARGUMENTS = {
    "run_guest_geekbench.py": ("--console-log", "{directory}/console.log"),
    "run_guest_command_bench.py": ("--command", "true"),
    "run_guest_exec_bench.py": (
        "--cpus", "1", "--workers", "1", "--executions", "1",
    ),
    "run_guest_link_bench.py": (
        "--cpus", "1", "--workers", "1", "--binary", "{directory}/link-loop",
    ),
}


def verify_engine_forwarding(runner: str) -> None:
    """Run a runner up to its Guest and check the engine and core class."""
    module = load_runner_module(runner)
    for engine in ("jit", "gadgets"):
        with tempfile.TemporaryDirectory(prefix="uml-perf-cli-") as directory:
            pathlib.Path(directory, "link-loop").write_bytes(b"")
            arguments = [
                str(ROOT / runner),
                "--emulator",
                "emulator",
                "--rootfs",
                "rootfs.erofs",
                "--engine",
                engine,
                "--core-class",
                "efficiency",
                *(
                    argument.format(directory=directory)
                    for argument in RUNNER_ARGUMENTS[runner]
                ),
            ]
            with (
                mock.patch.object(sys, "argv", arguments),
                mock.patch.object(
                    module, "Guest", side_effect=GuestConstructed
                ) as constructor,
            ):
                try:
                    module.main()
                except GuestConstructed:
                    pass
                else:
                    raise RuntimeError(f"{runner}: main() built no Guest")
        call = constructor.call_args
        if call.kwargs.get("engine") != engine:
            raise RuntimeError(f"{runner}: Guest must receive engine={engine}")
        if call.kwargs.get("core_class") != "efficiency":
            raise RuntimeError(
                f"{runner}: Guest must receive the selected core class so an "
                "efficiency-core measurement is selectable"
            )
        if any("--execution-engine" in argument for argument in call.args[4]):
            raise RuntimeError(
                f"{runner}: engine selection must not be passed as a kernel "
                "argument"
            )
        if (
            runner == "run_guest_geekbench.py"
            and call.kwargs.get("overlay_shell") is not True
        ):
            raise RuntimeError(
                f"{runner}: Geekbench must enable overlay_shell=True"
            )


def load_runner_module(runner: str):
    source_path = ROOT / runner
    module_name = f"performance_runner_contract_{source_path.stem}"
    spec = importlib.util.spec_from_file_location(module_name, source_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {runner}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_guest_shell_module():
    source_path = TOOLS / "guest-shell.py"
    spec = importlib.util.spec_from_file_location(
        "performance_guest_shell_contract", source_path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load guest-shell.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_runner_arguments(runner: str, extra: list[str]) -> object:
    module = load_runner_module(runner)
    arguments = [
        str(ROOT / runner),
        "--emulator",
        "emulator",
        "--rootfs",
        "rootfs.erofs",
        *extra,
    ]
    with mock.patch.object(sys, "argv", arguments):
        return module.parse_arguments()


def verify_offline_geekbench_contract() -> None:
    module = load_runner_module("run_guest_geekbench.py")
    script = module.WORKLOAD.read_text(encoding="utf-8")
    runs = [
        line
        for line in script.splitlines()
        if line.strip().startswith('"$benchmark"')
    ]
    if not runs or any("--no-upload" not in line for line in runs):
        raise RuntimeError("Geekbench must disable the upload prompt")
    options = parse_runner_arguments(
        "run_guest_geekbench.py",
        ["--console-log", "console.log"],
    )
    if options.workloads is not None:
        raise RuntimeError("Geekbench must run the full benchmark by default")


def session_guest() -> mock.MagicMock:
    """A Guest stand-in that, like Guest, closes when its session ends."""
    guest = mock.MagicMock()
    guest.__enter__.return_value = guest

    def exit_session(*_exception: object) -> bool:
        guest.close()
        return False

    guest.__exit__.side_effect = exit_session
    return guest


def run_geekbench_shutdown_case(
    command_status: int,
    shutdown_ok: bool,
    emulator_status: int | None,
    *,
    workloads: str | None = None,
    payload: pathlib.Path | None = None,
    payload_sha256: str | None = None,
    root_type: str = "erofs",
    no_network: bool = False,
) -> tuple[int | None, RuntimeError | None, mock.Mock, mock.Mock]:
    module = load_runner_module("run_guest_geekbench.py")
    directory = tempfile.TemporaryDirectory(prefix="uml-geekbench-cli-")
    console_log = pathlib.Path(directory.name, "console.log")
    arguments = types.SimpleNamespace(
        emulator=pathlib.Path("emulator"),
        rootfs=pathlib.Path("rootfs.erofs"),
        console_log=console_log,
        workloads=workloads,
        payload=payload,
        payload_sha256=payload_sha256,
        root_type=root_type,
        no_network=no_network,
        kernel_argument=[],
        memory="8G",
        cpus=10,
        engine="jit",
        core_class="performance",
        timeout=30,
    )
    guest = session_guest()
    guest.run_command.return_value = types.SimpleNamespace(
        return_code=command_status, output="benchmark"
    )
    guest.shutdown.return_value = shutdown_ok
    guest.return_code = emulator_status

    status: int | None = None
    error: RuntimeError | None = None
    with (
        directory,
        mock.patch.object(module, "parse_arguments", return_value=arguments),
        mock.patch.object(module, "Guest", return_value=guest) as constructor,
        contextlib.redirect_stdout(io.StringIO()),
    ):
        try:
            status = module.main()
        except RuntimeError as caught:
            error = caught
    guest.close.assert_called_once_with()
    # The console streams into the log.
    sink = constructor.call_args.kwargs["console_sink"]
    if sink.name != str(console_log) or not sink.closed:
        raise RuntimeError(
            f"Geekbench runner did not stream the console into {console_log}: "
            f"{sink!r}"
        )
    return status, error, guest, constructor


def verify_geekbench_shutdown_contract() -> None:
    status, error, guest, _ = run_geekbench_shutdown_case(0, True, 0)
    if status != 0 or error is not None:
        raise RuntimeError(
            "Geekbench runner rejected a clean emulator exit: "
            f"status={status}, error={error}"
        )
    guest.shutdown.assert_called_once_with(60)

    status, error, _, _ = run_geekbench_shutdown_case(0, True, 23)
    if (
        status is not None
        or error is None
        or "emulator exited with status 23" not in str(error)
    ):
        raise RuntimeError(
            "Geekbench runner allowed a nonzero emulator exit to pass: "
            f"status={status}, error={error}"
        )

    status, error, _, _ = run_geekbench_shutdown_case(7, True, 23)
    if (
        status is not None
        or error is None
        or "Geekbench exited with status 7" not in str(error)
    ):
        raise RuntimeError(
            "Geekbench runner did not preserve benchmark failure precedence: "
            f"status={status}, error={error}"
        )


def run_guest_shell_case(
    command_status: int,
    shutdown_ok: bool,
    emulator_status: int | None,
    *,
    audio: bool = False,
) -> tuple[int, str, mock.Mock, mock.Mock]:
    module = load_guest_shell_module()
    arguments = types.SimpleNamespace(
        emulator=pathlib.Path("emulator"),
        rootfs=pathlib.Path("rootfs.erofs"),
        memory="4G",
        cpus=4,
        audio=audio,
        engine="jit",
        core_class="performance",
        quiet=True,
        boot_timeout=30,
        timeout=30,
        command=["benchmark"],
    )
    guest = mock.Mock()
    guest.run_command.return_value = types.SimpleNamespace(
        return_code=command_status,
        output="benchmark output",
    )
    guest.shutdown.return_value = shutdown_ok
    guest.return_code = emulator_status
    stderr = io.StringIO()
    stdout = io.StringIO()
    with (
        mock.patch.object(module, "parse_arguments", return_value=arguments),
        mock.patch.object(module, "Guest", return_value=guest) as constructor,
        contextlib.redirect_stdout(stdout),
        contextlib.redirect_stderr(stderr),
    ):
        status = module.main()
    guest.close.assert_called_once_with()
    return status, stderr.getvalue(), guest, constructor


def verify_guest_shell_shutdown_contract() -> None:
    status, stderr, guest, _ = run_guest_shell_case(0, True, 0)
    if status != 0 or stderr:
        raise RuntimeError(
            "guest-shell rejected a successful command and clean emulator exit: "
            f"status={status}, stderr={stderr!r}"
        )
    guest.shutdown.assert_called_once_with(60)

    status, stderr, _, _ = run_guest_shell_case(0, False, None)
    if status == 0 or "did not halt cleanly within 60 seconds" not in stderr:
        raise RuntimeError(
            "guest-shell allowed a successful command to mask shutdown timeout: "
            f"status={status}, stderr={stderr!r}"
        )

    status, stderr, _, _ = run_guest_shell_case(0, True, 23)
    if status == 0 or "emulator exited with status 23" not in stderr:
        raise RuntimeError(
            "guest-shell allowed a successful command to mask emulator failure: "
            f"status={status}, stderr={stderr!r}"
        )

    status, stderr, _, _ = run_guest_shell_case(7, False, None)
    if status != 7 or "did not halt cleanly within 60 seconds" not in stderr:
        raise RuntimeError(
            "guest-shell did not preserve command failure precedence across "
            f"shutdown timeout: status={status}, stderr={stderr!r}"
        )

    status, stderr, _, constructor = run_guest_shell_case(
        0, True, 0, audio=True)
    kernel_arguments = constructor.call_args.args[4]
    if status != 0 or stderr or kernel_arguments.count("--audio") != 1:
        raise RuntimeError(
            "guest-shell did not forward --audio exactly once as a host "
            f"option: status={status}, stderr={stderr!r}, "
            f"arguments={kernel_arguments!r}"
        )


def verify_geekbench_payloads() -> None:
    """The workload script, and its parameters when targeted, are payloads."""
    module = load_runner_module("run_guest_geekbench.py")
    script = module.WORKLOAD.read_bytes()
    for workloads, expected in (
        (None, {module.GUEST_WORKLOAD: script}),
        (
            "4,17",
            {
                module.GUEST_WORKLOAD: script,
                module.GUEST_PARAMETERS: b"UML_GEEKBENCH_WORKLOADS=4,17\n",
            },
        ),
    ):
        status, error, _, constructor = run_geekbench_shutdown_case(
            0, True, 0, workloads=workloads
        )
        payloads = constructor.call_args.kwargs["payloads"]
        contents = {path: payload.contents for path, payload in payloads.items()}
        if status != 0 or error is not None or contents != expected:
            raise RuntimeError(
                f"Geekbench payloads for workloads={workloads!r} were "
                f"{sorted(contents)}: status={status}, error={error}"
            )
        if payloads[module.GUEST_WORKLOAD].mode != 0o755:
            raise RuntimeError("the Geekbench script must be executable")
        command = constructor.return_value.run_command.call_args.args[0]
        if command != module.GUEST_WORKLOAD:
            raise RuntimeError(f"Geekbench ran {command!r}, not its script")
    for workloads in ("", "4,", "4;reboot", ",4"):
        try:
            run_geekbench_shutdown_case(0, True, 0, workloads=workloads)
        except ValueError:
            continue
        raise RuntimeError(f"Geekbench accepted workloads={workloads!r}")


def verify_geekbench_payload_image() -> None:
    """An attached payload is checked, then becomes the second disk."""
    module = load_runner_module("run_guest_geekbench.py")
    with tempfile.TemporaryDirectory(prefix="uml-geekbench-cli-") as name:
        image = pathlib.Path(name, "payload.erofs")
        image.write_bytes(b"payload image")
        digest = hashlib.sha256(b"payload image").hexdigest()
        disk = f"{module.PAYLOAD_DISK}={image.resolve()}"
        mount = f"UML_GEEKBENCH_PAYLOAD={module.PAYLOAD_DEVICE}\n".encode()
        for workloads, payload, arguments, parameters in (
            (None, None, ("ncpus=10",), None),
            (None, image, ("ncpus=10", disk), mount),
            (
                "102,201",
                image,
                ("ncpus=10", disk),
                b"UML_GEEKBENCH_WORKLOADS=102,201\n" + mount,
            ),
        ):
            status, error, _, constructor = run_geekbench_shutdown_case(
                0,
                True,
                0,
                workloads=workloads,
                payload=payload,
                payload_sha256=digest if payload else None,
            )
            built = constructor.call_args
            installed = built.kwargs["payloads"].get(module.GUEST_PARAMETERS)
            if (
                status != 0
                or error is not None
                or built.args[4] != arguments
                or (installed and installed.contents) != parameters
            ):
                raise RuntimeError(
                    f"Geekbench with payload={payload} and workloads="
                    f"{workloads!r} built {built}: status={status}, "
                    f"error={error}"
                )
        for payload, payload_sha256 in (
            (image, None),
            (image, digest.upper()),
            (image, hashlib.sha256(b"other").hexdigest()),
            (None, digest),
        ):
            try:
                run_geekbench_shutdown_case(
                    0, True, 0, payload=payload, payload_sha256=payload_sha256
                )
            except ValueError:
                continue
            raise RuntimeError(
                f"Geekbench accepted payload={payload} with "
                f"payload_sha256={payload_sha256!r}"
            )


def verify_geekbench_boot_options() -> None:
    """The root type and network choices reach the Guest only when made."""
    options = parse_runner_arguments(
        "run_guest_geekbench.py", ["--console-log", "console.log"]
    )
    if options.root_type != "erofs" or options.no_network:
        raise RuntimeError("Geekbench must boot the usual guest by default")
    _, _, _, constructor = run_geekbench_shutdown_case(0, True, 0)
    unusual = {"root_type", "network"} & set(constructor.call_args.kwargs)
    if unusual:
        raise RuntimeError(
            f"the default Geekbench guest was given {sorted(unusual)}"
        )
    for case, expected in (
        ({"root_type": "ext4"}, {"root_type": "ext4"}),
        ({"no_network": True}, {"network": "image"}),
        (
            {"root_type": "ext4", "no_network": True},
            {"root_type": "ext4", "network": "image"},
        ),
    ):
        status, error, _, constructor = run_geekbench_shutdown_case(
            0, True, 0, **case
        )
        passed = {
            name: value
            for name, value in constructor.call_args.kwargs.items()
            if name in ("root_type", "network")
        }
        if status != 0 or error is not None or passed != expected:
            raise RuntimeError(
                f"Geekbench with {case} built {passed}: status={status}, "
                f"error={error}"
            )
    parsed = parse_runner_arguments(
        "run_guest_geekbench.py",
        ["--console-log", "console.log", "--root-type", "ext4", "--no-network"],
    )
    if parsed.root_type != "ext4" or not parsed.no_network:
        raise RuntimeError("Geekbench ignored --root-type or --no-network")


# The guest scripts the exec and link benches time, at two workers. Recorded
# results stay comparable only while this text stays the same.
EXEC_WORKLOAD = (
    'rm -f /tmp/exec-bench-pids; : >/tmp/exec-bench-pids; worker=0; '
    'while [ "$worker" -lt 2 ]; do '
    '(i=0; while [ "$i" -lt 2 ]; do /bin/true || exit; i=$((i + 1)); done) & '
    'echo "$!" >>/tmp/exec-bench-pids; worker=$((worker + 1)); done; '
    'status=0; for pid in $(cat /tmp/exec-bench-pids); do '
    'wait "$pid" || status=1; done; '
    'rm -f /tmp/exec-bench-pids; test "$status" -eq 0'
)
LINK_WORKLOAD = (
    'rm -f /tmp/link-loop-output.*; : >/tmp/link-loop-pids; i=0; '
    'while [ "$i" -lt 2 ]; do /usr/local/libexec/uml-test/guest-link-loop '
    '>"/tmp/link-loop-output.$i" 2>&1 & '
    'echo "$!" >>/tmp/link-loop-pids; i=$((i + 1)); done; '
    'status=0; for pid in $(cat /tmp/link-loop-pids); do '
    'wait "$pid" || status=1; done; cat /tmp/link-loop-output.*; '
    'rm -f /tmp/link-loop-output.* /tmp/link-loop-pids; test "$status" -eq 0'
)
TIMED_RUNS = (
    "run 1: 0.500s wall, 1.000s host CPU (2.00 cores)\n",
    "run 2: 1.000s wall, 3.000s host CPU (3.00 cores)\n",
)
TIMED_MEDIAN = "median: 0.750s wall, 2.50 host cores"


def timed(command: str, timeout: int = 30) -> list[object]:
    """The host readings and guest command of one measured repetition."""
    return ["cpu", "clock", ("run", command, timeout), "clock", "cpu"]


def run_timed_session(
    runner: str,
    extra: tuple[str, ...],
    output: str,
    *,
    command_status: int = 0,
    shutdown_ok: bool = True,
    emulator_status: int = 0,
) -> tuple[list[object], object, str, str | None]:
    """Run a timed runner to the end against a stand-in guest.

    Returns, in order, every host reading, sampler and guest call; the Guest
    construction; stdout; and the RuntimeError text, if any. The two
    repetitions take 0.5 s and 1 s of wall time and 1 s and 3 s of host CPU.
    """
    module = load_runner_module(runner)
    events: list[object] = []
    walls = iter((0.0, 0.5, 10.0, 11.0))
    cpus = iter(("0:00.00", "0:01.00", "0:10.00", "0:13.00"))
    guest = session_guest()
    guest.process_id = 4242
    guest.return_code = emulator_status

    def record(name: str, result: object = None):
        def call(*arguments: object, **_options: object) -> object:
            events.append((name, *arguments))
            return result
        return call

    def clock() -> float:
        events.append("clock")
        return next(walls)

    def cpu_time(*_arguments: object, **_options: object) -> str:
        events.append("cpu")
        return next(cpus)

    def run_command(command: str, timeout: int = 30) -> object:
        events.append(("run", command, timeout))
        return types.SimpleNamespace(return_code=command_status, output=output)

    guest.boot_to_shell.side_effect = record("boot_to_shell")
    guest.boot_and_login.side_effect = record("boot_and_login")
    guest.run_command.side_effect = run_command
    guest.shutdown.side_effect = record("shutdown", shutdown_ok)
    stdout = io.StringIO()
    error = None
    with tempfile.TemporaryDirectory(prefix="uml-perf-cli-") as directory:
        resolved = str(pathlib.Path(directory).resolve())
        pathlib.Path(directory, "link-loop").write_bytes(b"probe")

        def start_sampler(argv: tuple[str, ...], **_options: object) -> object:
            events.append(
                ("sample", *(item.replace(resolved, "DIR") for item in argv))
            )
            return mock.Mock(wait=mock.Mock(side_effect=record("reap")))

        arguments = [
            str(ROOT / runner), "--emulator", "emulator",
            "--rootfs", "rootfs.erofs",
            *(item.format(directory=directory) for item in extra),
        ]
        with (
            mock.patch.object(sys, "argv", arguments),
            mock.patch.object(module, "Guest", return_value=guest) as built,
            mock.patch("time.monotonic", side_effect=clock),
            mock.patch("subprocess.check_output", side_effect=cpu_time),
            mock.patch("subprocess.Popen", side_effect=start_sampler),
            contextlib.redirect_stdout(stdout),
        ):
            try:
                module.main()
            except RuntimeError as caught:
                error = str(caught)
    guest.close.assert_called_once_with()
    return events, built.call_args, stdout.getvalue(), error


def verify_timed_sessions() -> None:
    """Pin each timed runner's Guest, guest commands, halt and report."""
    emulator = pathlib.Path("emulator").resolve()
    rootfs = pathlib.Path("rootfs.erofs").resolve()
    command = ("--command", "uname -m", "--repetitions", "2")
    link = ("--binary", "{directory}/link-loop", "--cpus", "2",
            "--workers", "2", "--repetitions", "2",
            "--core-class", "efficiency")
    sample = ("sample", "/usr/bin/sample", "4242", "1", "-file")
    output = (
        f"aarch64\n{TIMED_RUNS[0]}aarch64\n{TIMED_RUNS[1]}{TIMED_MEDIAN} "
        "[engine=jit cores=performance cpus=4]\n"
    )
    cases = (
        (
            "run_guest_command_bench.py",
            (*command, "--sample-output", "{directory}/profile.txt",
             "--sample-seconds", "1"),
            "aarch64",
            mock.call(emulator, rootfs, "1024M", "tmpfs", ("ncpus=4",),
                      init_shell=False, overlay_shell=True, engine="jit",
                      core_class="performance", payloads={}),
            [("boot_and_login", 180),
             (*sample, "DIR/profile-1.txt"), *timed("uname -m"), ("reap",),
             (*sample, "DIR/profile-2.txt"), *timed("uname -m"), ("reap",),
             ("shutdown", 60)],
            output,
        ),
        (
            "run_guest_command_bench.py",
            (*command, "--shell-init", "--timeout", "45"),
            "aarch64",
            mock.call(emulator, rootfs, "1024M", "tmpfs", ("ncpus=4",),
                      init_shell=True, overlay_shell=False, engine="jit",
                      core_class="performance", payloads={}),
            [("boot_to_shell", 180),
             ("run", "mount -t tmpfs -o mode=1777 tmpfs /tmp", 30),
             *timed("uname -m", 45), *timed("uname -m", 45),
             ("shutdown", 60)],
            output,
        ),
        (
            "run_guest_exec_bench.py",
            ("--cpus", "2", "--workers", "2", "--executions", "4",
             "--repetitions", "2", "--engine", "jit"),
            "",
            mock.call(emulator, rootfs, "1024M", "tmpfs", ("ncpus=2",),
                      overlay_shell=True, engine="jit",
                      core_class="performance"),
            [("boot_and_login",), *timed(EXEC_WORKLOAD),
             *timed(EXEC_WORKLOAD), ("shutdown", 60)],
            f"{TIMED_RUNS[0]}{TIMED_RUNS[1]}{TIMED_MEDIAN} "
            "[engine=jit cores=performance cpus=2 workers=2]\n",
        ),
        (
            "run_guest_link_bench.py",
            link,
            "UML_PERF_PROBE steps=7\nOK\nOK",
            mock.call(emulator, rootfs, "1024M", "tmpfs", ("ncpus=2",),
                      init_shell=False, overlay_shell=True, engine="jit",
                      core_class="efficiency",
                      payloads={"/usr/local/libexec/uml-test/guest-link-loop":
                                mock.ANY}),
            [("boot_and_login", 180), *timed(LINK_WORKLOAD),
             *timed(LINK_WORKLOAD), ("shutdown", 60)],
            f"guest run 1: UML_PERF_PROBE steps=7\n{TIMED_RUNS[0]}"
            f"guest run 2: UML_PERF_PROBE steps=7\n{TIMED_RUNS[1]}"
            f"{TIMED_MEDIAN} [engine=jit cores=efficiency cpus=2 workers=2]\n",
        ),
    )
    for runner, extra, guest_output, construction, expected, report in cases:
        events, built, stdout, error = run_timed_session(
            runner, extra, guest_output
        )
        if (error, built, events, stdout) != (
            None, construction, expected, report
        ):
            raise RuntimeError(
                f"{runner} {extra}: error={error}\nGuest: {built}\n"
                f"events: {events}\nstdout: {stdout!r}"
            )
        if runner == "run_guest_link_bench.py":
            probe = built.kwargs["payloads"][
                "/usr/local/libexec/uml-test/guest-link-loop"
            ]
            if (probe.contents, probe.mode) != (b"probe", 0o755):
                raise RuntimeError(f"link bench installed {probe!r}")

    for runner, extra, guest_output, *_ in (cases[0], *cases[2:]):
        for options, message in (
            ({"command_status": 3}, "repetition 1 failed (3)\n"),
            ({"shutdown_ok": False},
             "emulator did not halt cleanly within 60 seconds"),
            ({"emulator_status": 23}, "emulator exited with status 23"),
        ):
            *_, error = run_timed_session(
                runner, extra, guest_output, **options
            )
            if error is None or not error.startswith(message):
                raise RuntimeError(f"{runner} {options}: error={error}")
    *_, error = run_timed_session(*cases[1][:3], command_status=3)
    if error != "guest mount setup failed\naarch64":
        raise RuntimeError(f"shell-init accepted a failed mount: {error}")
    *_, error = run_timed_session("run_guest_link_bench.py", link, "OK")
    if error != "repetition 1 returned unexpected output 'OK'":
        raise RuntimeError(f"link bench accepted one OK of two: {error}")


def verify_default_cpu_counts() -> None:
    """Benchmarks default to 4 vCPUs on the Mac and 2 on the iPad."""
    for runner, extra in (
        ("run_guest_geekbench.py", ["--console-log", "console.log"]),
        ("run_guest_command_bench.py", ["--command", "true"]),
        ("run_guest_exec_bench.py", []),
        ("run_guest_link_bench.py", ["--binary", "probe"]),
    ):
        options = parse_runner_arguments(runner, extra)
        if options.cpus != 4:
            raise RuntimeError(f"{runner}: default is {options.cpus} vCPUs")
        if hasattr(options, "workers"):
            # The workers follow the guest's vCPUs unless named.
            if options.workers != 4:
                raise RuntimeError(f"{runner}: default is {options.workers}")
            options = parse_runner_arguments(runner, [*extra, "--cpus", "2"])
            if options.workers != 2:
                raise RuntimeError(f"{runner}: workers must follow --cpus")
    for runner, extra in (
        ("run_ios_device_workload.py", ["--geekbench"]),
        ("run_ios_device_discard_bench.py", []),
    ):
        module = load_runner_module(runner)
        with mock.patch.object(
            sys, "argv", [str(ROOT / runner), "--device", "device", *extra]
        ):
            options = module.parse_arguments()
        if options.cpus != 2:
            raise RuntimeError(f"{runner}: default is {options.cpus} vCPUs")


def verify_no_root_device_option() -> None:
    """The launcher chooses the root device; no runner restates it."""
    cases = (
        ("run_guest_geekbench.py", ["--console-log", "console.log"]),
        ("run_guest_command_bench.py", ["--command", "true"]),
        ("run_guest_exec_bench.py", []),
        ("run_guest_link_bench.py", ["--binary", "probe"]),
    )
    for runner, extra in cases:
        options = parse_runner_arguments(runner, extra)
        if hasattr(options, "root_device"):
            raise RuntimeError(f"{runner}: --root-device must not return")


def verify_core_class_launcher() -> None:
    """The core-class choice must only change the launcher prefix."""
    support = pathlib.Path(__file__).resolve().parents[2] / "integration" / "support"
    spec = importlib.util.spec_from_file_location(
        "guest_core_class_contract", support / "guest.py"
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load the guest support module")
    guest = importlib.util.module_from_spec(spec)
    # dataclasses resolves a class's defining module through sys.modules, so
    # register the module before executing it.
    sys.modules[spec.name] = guest
    try:
        spec.loader.exec_module(guest)
    except BaseException:
        del sys.modules[spec.name]
        raise

    def build(core_class: str) -> list[str]:
        return guest.emulator_arguments(
            pathlib.Path("/emulator"),
            pathlib.Path("/rootfs.erofs"),
            "8G",
            core_class=core_class,
        )

    performance = build("performance")
    if performance[0] != "/emulator":
        raise RuntimeError(
            "performance-core runs must launch the emulator directly: "
            f"{performance}"
        )

    if sys.platform == "darwin":
        efficiency = build("efficiency")
        prefix = [guest.TASKPOLICY, "-c", "background"]
        if efficiency[: len(prefix)] != prefix:
            raise RuntimeError(
                f"efficiency-core runs must apply {prefix}: {efficiency}"
            )
        if efficiency[len(prefix) :] != performance:
            raise RuntimeError(
                "the core class must change only the launcher prefix: "
                f"{efficiency} versus {performance}"
            )

    try:
        build("little")
    except ValueError:
        pass
    else:
        raise RuntimeError("an unknown core class was accepted")


def main() -> int:
    verify_offline_geekbench_contract()
    verify_geekbench_shutdown_contract()
    verify_guest_shell_shutdown_contract()
    verify_geekbench_payloads()
    verify_geekbench_payload_image()
    verify_geekbench_boot_options()
    verify_timed_sessions()
    verify_default_cpu_counts()
    verify_no_root_device_option()
    verify_core_class_launcher()
    for runner in RUNNERS:
        verify_engine_forwarding(runner)
        result = subprocess.run(
            (sys.executable, str(ROOT / runner), "--help"),
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0 or "usage:" not in result.stdout:
            print(
                f"{runner}: CLI/import smoke failed\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
                file=sys.stderr,
            )
            return 1
    for runner in MOBILE_RUNNERS:
        result = subprocess.run(
            (sys.executable, str(ROOT / runner), "--help"),
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0 or "usage:" not in result.stdout:
            print(
                f"{runner}: CLI/import smoke failed\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
                file=sys.stderr,
            )
            return 1
    count = len(RUNNERS) + len(MOBILE_RUNNERS)
    print(f"benchmark CLI/import smoke passed ({count} runners)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
