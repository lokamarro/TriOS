#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import importlib.util
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

import receipt_fixtures


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "tests/integration/gpu/run_chromium_webgl.py"


def load_module(name: str, path: Path):
    specification = importlib.util.spec_from_file_location(name, path)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


run_chromium_webgl = load_module("run_chromium_webgl", SCRIPT)
webgl_launcher = run_chromium_webgl.webgl_launcher


def metric_line(samples: list[float]) -> str:
    raw = ",".join(f"{sample:.3f}" for sample in samples)
    return (
        "[browser] UML_AQUARIUM_OK"
        f"|AVG_FPS={sum(samples) / len(samples):.3f}"
        f"|MIN_FPS={min(samples):.3f}|MAX_FPS={max(samples):.3f}"
        f"|SAMPLES={len(samples)}|WARMUP_SECONDS=15|RESOURCES=73"
        "|FISH=500|CANVAS=440x860|SHADER_COMPILES=18"
        "|SHADER_COMPILE_CPU_MS=7.250|PROGRAM_LINKS=9"
        "|PROGRAM_LINK_CPU_MS=3.125|INSTRUMENTED_METHODS=4"
        f"|INSTRUMENTATION_ERRORS=0|RAW_FPS={raw}"
        "|VERSION=WebGL 2.0 (OpenGL ES 3.0 Chromium)"
        "|RENDERER=ANGLE (Mesa, virgl, OpenGL ES 3.0)"
    )


def guest_measurement_transcript(samples: int = 30) -> str:
    rows = []
    phases = [("BEGIN", 0)]
    phases.extend(("SAMPLE", index) for index in range(1, samples + 1))
    phases.append(("END", 0))
    for offset, (phase, index) in enumerate(phases):
        cpu = (
            100 + offset,
            0,
            20 + offset,
            200 + offset,
            10,
            0,
            5 + offset,
            0,
            0,
            0,
        )
        rows.append(
            f"UML_AQUARIUM_GUEST_{phase}|INDEX={index}"
            f"|UPTIME={100 + offset:.2f}|CLK_TCK=100"
            f"|CPU_JIFFIES={','.join(str(value) for value in cpu)}"
            f"|MEM_AVAILABLE_KIB={900000 - offset}"
            f"|MEM_FREE_KIB={500000 - offset}"
            f"|COMMITTED_AS_KIB={400000 + offset}"
        )
    return "\n".join(rows)


class AquariumMetricTests(unittest.TestCase):
    def test_complete_metric_line_is_parsed_from_raw_samples(self) -> None:
        samples = [20.0 + index / 10 for index in range(30)]
        fields = run_chromium_webgl.parse_metrics(metric_line(samples))
        self.assertEqual(fields["RAW_FPS"], samples)
        self.assertEqual(fields["SAMPLES"], 30)
        self.assertEqual(fields["FISH"], 500)
        self.assertEqual(fields["SHADER_COMPILES"], 18)
        self.assertGreater(fields["P95_FRAME_TIME_MS"], 0)

    def test_chromium_console_source_suffix_is_removed(self) -> None:
        line = metric_line([14.0] * 30).replace(
            "ANGLE (Mesa, virgl, OpenGL ES 3.0)",
            "virgl (Apple M1 Ultra)",
        )
        fields = run_chromium_webgl.parse_metrics(
            line + '", source: chrome-extension://test/verify.js (1)'
        )
        self.assertEqual(fields["RENDERER"], "virgl (Apple M1 Ultra)")

    def test_incomplete_or_inconsistent_metrics_are_rejected(self) -> None:
        with self.assertRaises(RuntimeError):
            run_chromium_webgl.parse_metrics(
                "UML_AQUARIUM_OK|AVG_FPS=10|MIN_FPS=9|MAX_FPS=11"
            )
        with self.assertRaises(RuntimeError):
            run_chromium_webgl.parse_metrics(
                metric_line([10.0] * 30).replace(
                    "|AVG_FPS=10.000", "|AVG_FPS=11.000"
                )
            )
        with self.assertRaises(RuntimeError):
            run_chromium_webgl.parse_metrics(
                metric_line([10.0] * 30).replace(
                    "|INSTRUMENTATION_ERRORS=0",
                    "|INSTRUMENTATION_ERRORS=1",
                )
            )

    def test_sample_stream_requires_exact_order(self) -> None:
        transcript = "\n".join(
            f"UML_AQUARIUM_SAMPLE|INDEX={index}|FPS={index + 0.5:.3f}"
            for index in range(1, 31)
        )
        self.assertEqual(
            run_chromium_webgl.parse_sample_lines(transcript),
            [index + 0.5 for index in range(1, 31)],
        )
        with self.assertRaises(RuntimeError):
            run_chromium_webgl.parse_sample_lines(
                transcript.replace("INDEX=15", "INDEX=16", 1)
            )


class AquariumProbePolicyTests(unittest.TestCase):
    def test_common_probe_freezes_policy_and_main_world(self) -> None:
        script = run_chromium_webgl.build_probe_script()
        self.assertIn('"world": "MAIN"', run_chromium_webgl.EXTENSION_MANIFEST)
        self.assertIn("const warmupSeconds = 15;", script)
        self.assertIn("const sampleSeconds = 30;", script)
        self.assertIn("const fishCount = 500;", script)
        self.assertIn('canvas?.getContext("webgl2")', script)
        self.assertIn("UML_AQUARIUM_MEASURE_BEGIN", script)
        self.assertIn("INSTRUMENTED_METHODS", script)
        self.assertIn("RAW_FPS=", script)
        self.assertNotIn("@WARMUP_SECONDS@", script)

    def test_launcher_pins_angle_and_emits_synchronized_snapshots(self) -> None:
        launcher = run_chromium_webgl.build_guest_launcher(
            auto_poweroff=True
        )
        self.assertIn("--use-gl=angle", launcher)
        self.assertIn("--use-angle=gles-egl", launcher)
        self.assertIn("UML_AQUARIUM_GUEST_IDENTITY", launcher)
        self.assertIn("emit_guest_snapshot SAMPLE", launcher)
        self.assertIn("UML_AQUARIUM_GUEST_POWEROFF|RESULT=pass", launcher)
        self.assertIn("poweroff -f", launcher)
        self.assertIn('[ "0" = 1 ]', launcher)
        self.assertNotIn("@EGL_CONFIG_DIAGNOSTICS@", launcher)

    def test_launcher_can_capture_x11_egl_config_diagnostics(self) -> None:
        launcher = run_chromium_webgl.build_guest_launcher(
            auto_poweroff=False,
            egl_config_diagnostics=True,
        )
        self.assertIn('[ "1" = 1 ]', launcher)
        self.assertIn("UML_EGL_CONFIG_DIAGNOSTICS_BEGIN", launcher)
        self.assertIn("chromium --version", launcher)
        self.assertIn("eglinfo -v -a gles -p x11", launcher)
        self.assertIn("xdpyinfo", launcher)
        self.assertIn("xwininfo -root -stats", launcher)
        self.assertIn("UML_EGL_CONFIG_DIAGNOSTICS_END", launcher)

    def test_launcher_can_require_zink_for_kde_and_chromium(self) -> None:
        launcher = run_chromium_webgl.build_guest_launcher(
            auto_poweroff=False,
            renderer_backend="zink",
        )
        self.assertIn("export MESA_LOADER_DRIVER_OVERRIDE=zink", launcher)
        self.assertIn("UML_KDE_ZINK_READY", launcher)
        self.assertIn("/proc/$kwin_pid/environ", launcher)
        self.assertIn("qdbus6 org.kde.KWin /KWin supportInformation", launcher)
        self.assertIn("OpenGL renderer string:", launcher)
        self.assertIn("virtio-gpu venus", launcher)
        self.assertIn('display_socket="$runtime/wayland-0"', launcher)
        self.assertIn(
            'chromium_platform_flags="--ozone-platform=wayland"',
            launcher,
        )
        self.assertIn("eglinfo -v -a gles -p wayland", launcher)
        self.assertNotIn("@RENDERER_BACKEND@", launcher)

    def test_native_display_reports_frame_statistics(self) -> None:
        with mock.patch.dict(run_chromium_webgl.os.environ, {}, clear=True):
            environment = run_chromium_webgl.build_host_environment("vnc")
        self.assertEqual(environment, {})

        with mock.patch.dict(run_chromium_webgl.os.environ, {}, clear=True):
            environment = run_chromium_webgl.build_host_environment("native")
        self.assertEqual(environment, {"UML_GPU_FRAME_STATS": "1"})

    def test_guest_shutdown_escalates_for_a_wedged_emulator(self) -> None:
        process = mock.Mock()
        process.pid = 1234
        process.poll.side_effect = (None, None, None)
        process.wait.side_effect = (
            subprocess.TimeoutExpired("guest", 45),
            subprocess.TimeoutExpired("guest", 15),
            0,
        )
        with mock.patch.object(run_chromium_webgl.os, "killpg") as killpg:
            run_chromium_webgl.stop_guest(process)
        self.assertEqual(
            killpg.call_args_list,
            [
                mock.call(1234, run_chromium_webgl.signal.SIGINT),
                mock.call(1234, run_chromium_webgl.signal.SIGTERM),
                mock.call(1234, run_chromium_webgl.signal.SIGKILL),
            ],
        )
        process.send_signal.assert_not_called()

    def test_runner_waits_for_the_closing_guest_snapshot(self) -> None:
        # The launcher prints the measurement's closing snapshot after
        # Chromium's report, and a busy two-vCPU guest took over a second to;
        # the runner had already read the console and failed the run.
        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "console.log"
            log_path.write_text(
                "UML_AQUARIUM_GUEST_SAMPLE|INDEX=30\n"
                "[1:1:INFO:CONSOLE:1] UML_AQUARIUM_OK|AVG_FPS=9.6\n",
                encoding="utf-8",
            )
            text = log_path.read_text(encoding="utf-8")
            with log_path.open("ab") as log:
                with run_chromium_webgl.guest_child(
                    [
                        sys.executable,
                        "-c",
                        "import time; time.sleep(1.5); "
                        "print('UML_AQUARIUM_GUEST_END|INDEX=0', flush=True); "
                        "time.sleep(30)",
                    ],
                    log,
                ) as process:
                    text = run_chromium_webgl.wait_for_guest_end(
                        process, log_path, text
                    )
            self.assertIn("UML_AQUARIUM_GUEST_END|INDEX=0", text)

    def test_guest_that_exits_without_its_closing_snapshot_is_named(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "console.log"
            log_path.write_text("UML_AQUARIUM_OK|\n", encoding="utf-8")
            with log_path.open("ab") as log:
                with run_chromium_webgl.guest_child(
                    [sys.executable, "-c", "pass"], log
                ) as process:
                    process.wait(timeout=30)
                    with self.assertRaisesRegex(
                        RuntimeError, "guest exited before"
                    ):
                        run_chromium_webgl.wait_for_guest_end(
                            process, log_path, "UML_AQUARIUM_OK|\n"
                        )

    def test_guest_starts_as_the_leader_of_its_own_session(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "console.log"
            with log_path.open("wb") as log:
                with run_chromium_webgl.guest_child(
                    [
                        sys.executable,
                        "-c",
                        "import os; print(os.getpgid(0), os.getsid(0))",
                    ],
                    log,
                ) as process:
                    self.assertEqual(process.wait(timeout=30), 0)
            group, session = map(int, log_path.read_text().split())
        self.assertEqual((group, session), (process.pid, process.pid))

    @unittest.skipUnless(os.name == "posix", "requires POSIX sessions")
    def test_terminated_runner_still_reaps_its_guest(self) -> None:
        # SIGTERM used to end the runner before its finally clause could stop
        # a harness in its own session; repeated signals must not skip the
        # stop either.
        harness = (
            "import os, pathlib, signal, sys, time\n"
            "def stop(*_):\n"
            "    time.sleep(0.5)\n"
            "    sys.exit(0)\n"
            "signal.signal(signal.SIGINT, stop)\n"
            "pathlib.Path(sys.argv[1]).write_text(str(os.getpid()))\n"
            "time.sleep(60)\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            record = root / "harness.pid"
            runner = (
                "import sys\n"
                "sys.dont_write_bytecode = True\n"
                f"sys.path.insert(0, {str(Path(run_chromium_webgl.__file__).parent)!r})\n"
                "import run_chromium_webgl as runner\n"
                f"with open({str(root / 'console.log')!r}, 'wb') as log:\n"
                "    with runner.guest_child(\n"
                f"        [sys.executable, '-c', {harness!r}, {str(record)!r}], log\n"
                "    ) as child:\n"
                "        child.wait(timeout=60)\n"
            )
            process = subprocess.Popen(
                [sys.executable, "-B", "-c", runner],
                start_new_session=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            harness_pid = None
            try:
                deadline = time.monotonic() + 30
                while not record.exists() or not record.read_text():
                    self.assertIsNone(process.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.05)
                harness_pid = int(record.read_text())
                process.send_signal(signal.SIGTERM)
                for _ in range(3):
                    time.sleep(0.05)
                    for repeat in (signal.SIGINT, signal.SIGTERM):
                        try:
                            process.send_signal(repeat)
                        except ProcessLookupError:
                            pass
                process.wait(timeout=60)
                with self.assertRaises(ProcessLookupError):
                    os.kill(harness_pid, 0)
                harness_pid = None
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)
                if harness_pid is not None:
                    try:
                        os.kill(harness_pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def test_guest_shutdown_signals_a_harness_that_leads_no_group(
        self,
    ) -> None:
        # A harness left in its runner's process group has no group of its
        # own to signal, yet it is still running.
        process = subprocess.Popen(
            [
                sys.executable,
                "-c",
                "import signal, time\n"
                "signal.signal(signal.SIGINT, signal.default_int_handler)\n"
                "print('ready', flush=True)\n"
                "time.sleep(60)\n",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        try:
            assert process.stdout is not None
            self.assertEqual(process.stdout.readline(), b"ready\n")
            self.assertNotEqual(os.getpgid(process.pid), process.pid)
            started = time.monotonic()
            run_chromium_webgl.stop_guest(process)
            self.assertIsNotNone(process.poll())
            # SIGINT stopped it; no escalation was needed.
            self.assertLess(time.monotonic() - started, 30)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            if process.stdout is not None:
                process.stdout.close()

    def test_guest_shutdown_never_reports_a_live_harness_stopped(
        self,
    ) -> None:
        process = mock.Mock()
        process.pid = 1234
        process.poll.return_value = None
        process.wait.side_effect = subprocess.TimeoutExpired("guest", 1)
        with mock.patch.object(
            run_chromium_webgl.os, "killpg", side_effect=ProcessLookupError
        ):
            with self.assertRaisesRegex(
                RuntimeError, "guest harness 1234 did not stop"
            ):
                run_chromium_webgl.stop_guest(process)
        self.assertEqual(
            process.send_signal.call_args_list,
            [
                mock.call(run_chromium_webgl.signal.SIGINT),
                mock.call(run_chromium_webgl.signal.SIGTERM),
                mock.call(run_chromium_webgl.signal.SIGKILL),
            ],
        )

    def test_renderer_policy_distinguishes_virgl_and_zink(self) -> None:
        line = metric_line([20.0] * 30)
        # Guest Virgl embeds the host renderer, Zink over KosmicKrisp, as a
        # virgl Aquarium run reported it.
        virgl = line.replace(
            "ANGLE (Mesa, virgl, OpenGL ES 3.0)",
            "ANGLE (Mesa, virgl (zink Vulkan 1.4(Apple M1 Ultra "
            "(MESA_KOSMICKRISP))), OpenGL ES 3.2)",
        )
        # Guest Zink on Venus, as a zink Aquarium run on the Vulkan
        # derivative reported it.
        zink = line.replace(
            "ANGLE (Mesa, virgl, OpenGL ES 3.0)",
            "ANGLE (Mesa, zink Vulkan 1.4(Virtio-GPU Venus (Apple M1 Ultra) "
            "(MESA_KOSMICKRISP)), OpenGL ES 3.2)",
        )
        run_chromium_webgl.validate_renderer(virgl, "virgl")
        run_chromium_webgl.validate_renderer(zink, "zink")
        with self.assertRaisesRegex(RuntimeError, "did not report virgl"):
            run_chromium_webgl.validate_renderer(zink, "virgl")
        with self.assertRaisesRegex(RuntimeError, "did not report zink"):
            run_chromium_webgl.validate_renderer(virgl, "zink")

    def test_checked_vulkan_rootfs_is_accepted_for_zink(self) -> None:
        receipt, contract = run_chromium_webgl.load_vulkan_rootfs_contract(
            receipt_fixtures.current_epoch_receipt_file(
                self, receipt_fixtures.VULKAN_RECEIPT
            )
        )
        self.assertEqual(receipt["artifact"], "uml-vulkan-rootfs")
        self.assertEqual(contract.filesystem_type, "ext4")
        self.assertEqual(contract.sha256, receipt["derivative"]["sha256"])

    def test_kernarium_payload_boots_the_probe_target(self) -> None:
        payload = run_chromium_webgl.kernarium_payload().files
        self.assertEqual(
            set(payload),
            {
                "/payload/var/tmp/uml-aquarium-extension/manifest.json",
                "/payload/var/tmp/uml-aquarium-extension/verify.js",
                "/payload/usr/local/libexec/uml-chromium-aquarium",
                "/payload/etc/systemd/system/uml-chromium-aquarium.service",
                "/payload/etc/systemd/system/uml-aquarium.target",
            },
        )
        launcher = payload["/payload/usr/local/libexec/uml-chromium-aquarium"]
        self.assertEqual(launcher.mode, 0o755)
        # Kernarium reads the console without a host deadline, so the guest
        # powers itself off.
        self.assertIn(b"poweroff -f", launcher.contents)
        target = payload[
            "/payload/etc/systemd/system/uml-aquarium.target"
        ].contents.decode()
        self.assertEqual(target, run_chromium_webgl.TARGET)
        self.assertIn("Requires=graphical.target\n", target)
        self.assertIn("Wants=uml-chromium-aquarium.service\n", target)
        conflicts = next(
            line.split("=", 1)[1].split()
            for line in target.splitlines()
            if line.startswith("Conflicts=")
        )
        self.assertIn("docker.socket", conflicts)
        self.assertIn("apt-daily-upgrade.timer", conflicts)
        self.assertNotIn("display-manager.service", conflicts)

    def test_only_unrelated_site_favicon_is_ignored(self) -> None:
        script = run_chromium_webgl.EXTENSION_SCRIPT
        self.assertIn('url.pathname === "/favicon.ico"', script)
        self.assertIn(
            "document load did not finish before the page deadline", script
        )
        self.assertIn("!isIgnoredSiteDecoration(entry.name)", script)
        self.assertIn("resource returned ", script)


FAKE_CLOCK_STEP = 10
STUB_COMMANDS = {
    "id": "echo 1001",
    # Waiting advances the fake guest clock, in coarse steps, instead of the
    # host's.
    "sleep": (
        'read -r up rest < "$FAKE_UPTIME"\n'
        'echo "$((${up%.*} + FAKE_CLOCK_STEP)).00 0" > "$FAKE_UPTIME"'
    ),
    # Run the bounded command without its bound; the fakes never block.
    "timeout": (
        'while :; do case "$1" in -k) shift 2 ;; -*) shift ;; *) break ;; '
        "esac; done\n"
        'shift\nexec "$@"'
    ),
    "runuser": 'shift 3\nexec "$@"',
    "loginctl": "echo stub-loginctl",
    "journalctl": "echo stub-journalctl",
    "ps": "echo stub-ps",
    "glxinfo": "echo stub-glxinfo",
    # Plasma's splash runs until the fake guest clock reaches
    # $FAKE_SPLASH_UNTIL; every other process the launcher looks for runs.
    "pgrep": (
        'if [ "$2" = ksplashqml ]; then\n'
        '    read -r up rest < "$FAKE_UPTIME"\n'
        '    [ "${up%.*}" -lt "${FAKE_SPLASH_UNTIL:-0}" ] || exit 1\n'
        "fi\n"
        "echo 4242"
    ),
    # Stopping Chromium leaves a mark its stub waits for.
    "pkill": 'touch "$(dirname "$FAKE_UPTIME")/chromium-stopped"',
    # The guest has GNU sed; give macOS hosts its in-place form.
    "sed": (
        'if [ "$1" = -i ]; then\n'
        '    /usr/bin/sed "$2" "$3" > "$3.new" && mv "$3.new" "$3"\n'
        'else\n    exec /usr/bin/sed "$@"\nfi'
    ),
}


# Chromium's own output from a guest too starved for its start-up
# connections (WebGL Aquarium on gadgets, the emulator stopped and resumed to
# run a third of the time, 2026-10-12): two children give up after 15 s, then
# the network service is lost and the page never loads.
STARVED_START = "".join(
    line + "\n"
    for line in (
        "[609:609:1012/060701.927686:WARNING:sandbox/policy/linux/s"
        "andbox_linux.cc:405] InitializeSandbox() called with "
        "multiple threads in process gpu-process.",
        "[612:612:1012/060713.674833:INFO:content/child/child_threa"
        "d_impl.cc:908] Terminating current process after 15 "
        "seconds with no connection.",
        "[609:609:1012/060714.896876:ERROR:ui/gl/gl_surface_egl.cc:"
        "277] No suitable EGL configs found.",
        "[609:609:1012/060714.909403:ERROR:ui/gl/gl_surface_egl.cc:"
        "380] No suitable EGL configs found for initialization.",
        "[626:1:1012/060715.429673:INFO:content/child/child_thread_"
        "impl.cc:908] Terminating current process after 15 seconds "
        "with no connection.",
        "[537:537:1012/060717.000443:ERROR:content/browser/network_"
        "service_instance_impl.cc:618] Network service crashed or "
        "was terminated, restarting service.",
    )
)
# The same workload run half the time: one other child gave up after 15 s,
# and the page still loaded and reported.
CHILD_TIMEOUT_ONLY = "".join(
    line + "\n"
    for line in (
        "[608:608:1012/052418.569083:WARNING:sandbox/policy/linux/s"
        "andbox_linux.cc:405] InitializeSandbox() called with "
        "multiple threads in process gpu-process.",
        "[608:608:1012/052429.359844:ERROR:ui/gl/gl_surface_egl.cc:"
        "277] No suitable EGL configs found.",
        "[608:608:1012/052429.363906:ERROR:ui/gl/gl_surface_egl.cc:"
        "380] No suitable EGL configs found for initialization.",
        "[629:1:1012/052432.498916:INFO:content/child/child_thread_"
        "impl.cc:908] Terminating current process after 15 seconds "
        "with no connection.",
        "[539:580:1012/052452.906298:ERROR:google_apis/gcm/engine/r"
        "egistration_request.cc:291] Registration response error "
        "message: DEPRECATED_ENDPOINT",
        "[539:539:1012/052501.819075:INFO:CONSOLE:55] \"--Setup "
        "Laser----------------------------------------\", source: "
        "https://webglsamples.org/tdl/log.js (55)",
        "[539:539:1012/052503.344426:INFO:CONSOLE:55] \"--Draw "
        "Fish---------------------------------------\", source: "
        "https://webglsamples.org/tdl/log.js (55)",
    )
)
NETWORK_SERVICE_LOST = STARVED_START.splitlines()[-1] + "\n"
STARVED_ERROR = (
    "UML_CHROMIUM_ERROR: network service missed Chromium's start-up "
    "connection; guest starved"
)


def chromium_stub(output: str, seconds: int = 10) -> str:
    """A Chromium that prints ``output`` and then runs until it is stopped,
    or exits with status 3 after ``seconds`` if nothing stops it."""

    return (
        "cat <<'CHROMIUM_OUTPUT'\n"
        + output
        + "CHROMIUM_OUTPUT\n"
        'stopped="$(dirname "$FAKE_UPTIME")/chromium-stopped"\n'
        "i=0\n"
        f'while [ ! -e "$stopped" ] && [ "$i" -lt {seconds * 10} ]; do\n'
        "    /bin/sleep 0.1\n"
        "    i=$((i + 1))\n"
        "done\n"
        '[ -e "$stopped" ] && exit 143\n'
        "exit 3"
    )


class AquariumLauncherBudgetTests(unittest.TestCase):
    """Every guest wait must end in a named error before the runner's."""

    def run_launcher(
        self,
        backend: str,
        *,
        session_ready: bool,
        stubs: dict[str, str] | None = None,
        splash_until: int = 0,
    ) -> tuple[subprocess.CompletedProcess[str], int, str]:
        budget = run_chromium_webgl.guest_budget(360, backend)
        launcher = run_chromium_webgl.build_guest_launcher(
            auto_poweroff=False, budget=budget, renderer_backend=backend
        )
        with tempfile.TemporaryDirectory(dir="/tmp") as directory:
            root = Path(directory)
            uptime = root / "uptime"
            uptime.write_text("5.00 0\n", encoding="utf-8")
            runtime = root / "run"
            x11 = root / "x11"
            extension = root / "ext"
            for path in (runtime, x11, extension):
                path.mkdir()
            (extension / "verify.js").write_text(
                run_chromium_webgl.build_probe_script(), encoding="utf-8"
            )
            environ = root / "environ"
            environ.write_bytes(b"MESA_LOADER_DRIVER_OVERRIDE=zink\0")
            listeners = []
            if session_ready:
                (runtime / "xauth_test").write_text("", encoding="utf-8")
                for path in (runtime / "bus", runtime / "wayland-0", x11 / "X0"):
                    listener = socket.socket(socket.AF_UNIX)
                    listener.bind(str(path))
                    listeners.append(listener)
            bin_directory = root / "bin"
            bin_directory.mkdir()
            for name, body in {**STUB_COMMANDS, **(stubs or {})}.items():
                stub = bin_directory / name
                stub.write_text(f"#!/bin/sh\n{body}\n", encoding="utf-8")
                stub.chmod(0o755)
            script = root / "launcher"
            script.write_text(
                launcher.replace("/proc/uptime", str(uptime))
                .replace('/run/user/$uml_uid', str(runtime))
                .replace("/tmp/.X11-unix", str(x11))
                .replace('"/proc/$kwin_pid/environ"', f'"{environ}"')
                .replace("/var/tmp/uml-aquarium-extension", str(extension)),
                encoding="utf-8",
            )
            try:
                result = subprocess.run(
                    ["/bin/sh", str(script)],
                    capture_output=True,
                    text=True,
                    timeout=60,
                    env={
                        "PATH": f"{bin_directory}:/usr/bin:/bin",
                        "FAKE_UPTIME": str(uptime),
                        "FAKE_CLOCK_STEP": str(FAKE_CLOCK_STEP),
                        "FAKE_SPLASH_UNTIL": str(splash_until),
                    },
                )
            finally:
                for listener in listeners:
                    listener.close()
            elapsed = int(uptime.read_text(encoding="utf-8").split(".")[0])
            armed = (extension / "verify.js").read_text(encoding="utf-8")
        return result, elapsed, armed

    def test_budget_keeps_every_phase_inside_the_runner_deadline(self) -> None:
        for backend in ("virgl", "zink"):
            for timeout in (360, 900):
                budget = run_chromium_webgl.guest_budget(timeout, backend)
                self.assertEqual(
                    budget.deadline,
                    timeout - webgl_launcher.GUEST_REPORT_MARGIN_SECONDS,
                )
                reserve = (
                    run_chromium_webgl.DEFAULT_WARMUP_SECONDS
                    + run_chromium_webgl.DEFAULT_SAMPLE_SECONDS
                    + run_chromium_webgl.CHROMIUM_MINIMUM_SECONDS
                )
                self.assertEqual(
                    budget.compositor_deadline, budget.deadline - reserve
                )
                self.assertLess(budget.session_deadline, budget.deadline)
                self.assertLessEqual(
                    budget.session_deadline, budget.compositor_deadline
                )
        zink = run_chromium_webgl.guest_budget(360, "zink")
        self.assertEqual(
            zink.compositor_deadline - zink.session_deadline,
            run_chromium_webgl.COMPOSITOR_PROBE_SECONDS,
        )
        with self.assertRaises(ValueError):
            run_chromium_webgl.guest_budget(200, "zink")

    def test_launcher_carries_its_budget_and_bounds_each_call(self) -> None:
        launcher = run_chromium_webgl.build_guest_launcher(
            auto_poweroff=False,
            budget=run_chromium_webgl.guest_budget(600, "zink"),
            renderer_backend="zink",
        )
        self.assertIn("deadline=555\n", launcher)
        self.assertIn("session_deadline=390\n", launcher)
        self.assertIn("compositor_deadline=450\n", launcher)
        self.assertIn('timeout -k 2 "$call_seconds"', launcher)
        self.assertIn('run_chromium "$chromium_seconds"', launcher)
        self.assertIn('timeout -k 10 "$chromium_bound"', launcher)
        self.assertIsNone(re.search(r"@[A-Z_]+@", launcher))
        self.assertNotIn("remaining=", launcher)

    def test_session_that_never_appears_is_named_at_its_deadline(self) -> None:
        budget = run_chromium_webgl.guest_budget(360, "virgl")
        result, elapsed, _ = self.run_launcher("virgl", session_ready=False)
        self.assertEqual(result.returncode, 1, result.stderr)
        lines = result.stdout.strip().splitlines()
        self.assertEqual(
            lines[-1],
            "UML_CHROMIUM_ERROR: KDE X11 bridge did not become ready by "
            f"guest uptime {budget.session_deadline}s",
        )
        self.assertIn("UML_SESSION_DIAGNOSTICS_BEGIN", lines)
        self.assertIn("stub-journalctl", lines)
        self.assertGreaterEqual(elapsed, budget.session_deadline)
        self.assertLess(elapsed, budget.session_deadline + FAKE_CLOCK_STEP)

    def test_chromium_starts_once_the_session_finished_starting(
        self,
    ) -> None:
        # Chromium started during the desktop's own start-up shares a small
        # guest with all of it, and its first children then miss their 15 s
        # IPC connection deadline. Plasma's splash marks that start-up.
        for backend in ("virgl", "zink"):
            with self.subTest(backend=backend):
                result, _, _ = self.run_launcher(
                    backend,
                    session_ready=True,
                    splash_until=45,
                    stubs={
                        "qdbus6": (
                            "echo 'OpenGL renderer string: zink Vulkan 1.3"
                            "(Virtio-GPU Venus (Apple M1))'"
                        ),
                        "chromium": (
                            'read -r up rest < "$FAKE_UPTIME"\n'
                            'echo "CHROMIUM_STARTED|UPTIME=${up%.*}"\n'
                            "exit 3"
                        ),
                    },
                )
                started = re.search(
                    r"^CHROMIUM_STARTED\|UPTIME=(\d+)$",
                    result.stdout,
                    re.MULTILINE,
                )
                self.assertIsNotNone(started, result.stdout)
                assert started is not None
                self.assertGreaterEqual(int(started.group(1)), 45)
                ready = re.search(
                    r"^UML_SESSION_READY\|UPTIME=(\d+)\.\d+$",
                    result.stdout,
                    re.MULTILINE,
                )
                self.assertIsNotNone(ready, result.stdout)
                assert ready is not None
                self.assertGreaterEqual(int(ready.group(1)), 45)

    def test_session_that_never_finishes_starting_is_named(self) -> None:
        budget = run_chromium_webgl.guest_budget(360, "virgl")
        result, elapsed, _ = self.run_launcher(
            "virgl", session_ready=True, splash_until=100000
        )
        self.assertEqual(result.returncode, 1, result.stderr)
        lines = result.stdout.strip().splitlines()
        self.assertEqual(
            lines[-1],
            "UML_CHROMIUM_ERROR: KDE X11 bridge did not finish starting by "
            f"guest uptime {budget.session_deadline}s",
        )
        self.assertIn("UML_SESSION_DIAGNOSTICS_BEGIN", lines)
        self.assertGreaterEqual(elapsed, budget.session_deadline)
        self.assertLess(elapsed, budget.session_deadline + FAKE_CLOCK_STEP)

    def test_compositor_that_never_answers_is_named(self) -> None:
        budget = run_chromium_webgl.guest_budget(360, "zink")
        result, elapsed, _ = self.run_launcher(
            "zink", session_ready=True, stubs={"qdbus6": "exit 1"}
        )
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(
            result.stdout.strip().splitlines()[-1],
            "UML_CHROMIUM_ERROR: KDE compositor did not answer on D-Bus by "
            f"guest uptime {budget.compositor_deadline}s",
        )
        self.assertGreaterEqual(elapsed, budget.compositor_deadline)
        self.assertLess(
            elapsed, budget.compositor_deadline + FAKE_CLOCK_STEP
        )

    def test_compositor_without_a_renderer_is_named(self) -> None:
        result, _, _ = self.run_launcher(
            "zink",
            session_ready=True,
            stubs={"qdbus6": "echo 'Compositing Type: none'"},
        )
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(
            "UML_CHROMIUM_ERROR: KDE compositor reported no OpenGL renderer",
            result.stdout.strip().splitlines()[-1],
        )

    def test_chromium_that_exits_without_a_report_is_named(self) -> None:
        for status, expected in (
            ("3", "Chromium exited with status 3 before Aquarium reported"),
            ("124", "Aquarium did not report by guest uptime 315s"),
        ):
            with self.subTest(status=status):
                result, _, armed = self.run_launcher(
                    "virgl",
                    session_ready=True,
                    stubs={"chromium": f"echo started\nexit {status}"},
                )
                self.assertEqual(
                    result.stdout.strip().splitlines()[-1],
                    "UML_CHROMIUM_ERROR: " + expected,
                )
                deadline = re.search(r"const pageDeadline = (\d+);", armed)
                self.assertIsNotNone(deadline)
                assert deadline is not None
                remaining = int(deadline.group(1)) / 1000 - time.time()
                # The fake clock stands at 5 s of a 315 s launcher budget.
                self.assertGreater(remaining, 315 - 5 - 10 - 30)
                self.assertLess(remaining, 315 - 5 - 10 + 30)

    def test_starved_chromium_start_is_named_and_stopped(self) -> None:
        result, _, _ = self.run_launcher(
            "virgl",
            session_ready=True,
            stubs={"chromium": chromium_stub(STARVED_START)},
        )
        lines = result.stdout.strip().splitlines()
        self.assertEqual(lines.count(STARVED_ERROR), 1)
        # Stopped by the launcher at once, not by its deadline, and named once.
        self.assertEqual(lines[-1], "UML_CHROMIUM_EXIT|STATUS=143")
        self.assertEqual(
            [line for line in lines if line.startswith("UML_CHROMIUM_ERROR:")],
            [STARVED_ERROR],
        )

    def test_chromium_that_recovers_is_not_stopped(self) -> None:
        for name, output in (
            ("other child timeout only", CHILD_TIMEOUT_ONLY),
            (
                "network service lost after the page started",
                CHILD_TIMEOUT_ONLY + NETWORK_SERVICE_LOST,
            ),
        ):
            with self.subTest(name):
                result, _, _ = self.run_launcher(
                    "virgl",
                    session_ready=True,
                    stubs={"chromium": chromium_stub(output, seconds=2)},
                )
                self.assertNotIn(STARVED_ERROR, result.stdout)
                # Nothing stopped it, so the stub ran out its own bound.
                self.assertEqual(
                    result.stdout.strip().splitlines()[-1],
                    "UML_CHROMIUM_ERROR: Chromium exited with status 3 "
                    "before Aquarium reported",
                )

    def test_named_guest_failures_reach_the_runner_error(self) -> None:
        text = (
            "noise\n[  12.5] uml-chromium-aquarium[504]: "
            "UML_CHROMIUM_ERROR: KDE Wayland session did not become ready by "
            "guest uptime 150s\n"
        )
        self.assertEqual(
            webgl_launcher.guest_failure(
                text, run_chromium_webgl.GUEST_FAILURE_MARKERS
            ),
            "UML_CHROMIUM_ERROR: KDE Wayland session did not become ready by "
            "guest uptime 150s",
        )
        page = (
            '[1:1:INFO:CONSOLE:130] "UML_AQUARIUM_ERROR|document load did '
            'not finish", source: chrome-extension://x/verify.js (130)'
        )
        self.assertEqual(
            webgl_launcher.guest_failure(
                page, run_chromium_webgl.GUEST_FAILURE_MARKERS
            ),
            "UML_AQUARIUM_ERROR|document load did not finish",
        )
        budget = run_chromium_webgl.guest_budget(360, "zink")
        self.assertIn(
            "did not start the Aquarium launcher",
            run_chromium_webgl.describe_missing_report("boot", 360, budget),
        )
        self.assertIn(
            "guest itself is unresponsive",
            run_chromium_webgl.describe_missing_report(
                "UML_AQUARIUM_LAUNCHER|UPTIME=12.1|DEADLINE=315", 360, budget
            ),
        )


class ExitingProcess:
    """A guest harness that is running until the runner waits for it, and
    then exits with status 1."""

    def __init__(self) -> None:
        self.waited = False

    def poll(self) -> int | None:
        return 1 if self.waited else None

    def wait(self, timeout: float) -> int:
        del timeout
        self.waited = True
        return 1


class AquariumResourceMetricTests(unittest.TestCase):
    def test_guest_identity_and_measurement_window_are_parsed(self) -> None:
        transcript = (
            "UML_AQUARIUM_GUEST_IDENTITY|PAGE_SIZE=4096|CPUS=4"
            "|MEM_TOTAL_KIB=1520944\n"
            + guest_measurement_transcript()
        )
        self.assertEqual(
            run_chromium_webgl.parse_guest_identity(transcript),
            {"page_size": 4096, "cpus": 4, "memory_total_kib": 1520944},
        )
        measurement = run_chromium_webgl.parse_guest_snapshots(transcript, 30)
        self.assertEqual(len(measurement["snapshots"]), 32)
        self.assertAlmostEqual(measurement["wall_seconds"], 31.0)
        self.assertGreater(measurement["active_cpu_seconds"], 0)
        self.assertGreater(measurement["average_cpu_cores"], 0)
        self.assertEqual(measurement["minimum_available_memory_kib"], 899969)

    def test_incomplete_guest_window_is_rejected(self) -> None:
        with self.assertRaises(RuntimeError):
            run_chromium_webgl.parse_guest_snapshots(
                guest_measurement_transcript().replace(
                    "UML_AQUARIUM_GUEST_END", "UML_AQUARIUM_GUEST_ERROR"
                ),
                30,
            )

    def test_native_frame_and_shutdown_contracts_fail_closed(self) -> None:
        frames = run_chromium_webgl.cts.parse_native_display_final(
            "Native display: direct Metal texture presentation\n"
            "Native display final: guest 120 presented 240 dropped 1 "
            "cpu-fallback 0"
        )
        self.assertEqual(frames["cpu_fallback_frames"], 0)
        clean = (
            "UML_AQUARIUM_GUEST_POWEROFF|RESULT=pass\n"
            "Darwin UML stopped: result=0 exitcode=0"
        )
        run_chromium_webgl.validate_native_shutdown(clean)
        with self.assertRaises(RuntimeError):
            run_chromium_webgl.validate_native_shutdown(
                clean + "\nMemory manager not clean during takedown."
            )
        with self.assertRaises(RuntimeError):
            run_chromium_webgl.validate_native_shutdown(
                "Darwin UML stopped: result=0 exitcode=0"
            )

    def test_host_summary_retains_raw_samples(self) -> None:
        begin = {
            "pids": [10, 11],
            "cpu_seconds": 2.0,
            "rss_kib": 100,
            "monotonic_seconds": 50.0,
        }
        end = {
            "pids": [10, 11],
            "cpu_seconds": 5.0,
            "rss_kib": 120,
            "monotonic_seconds": 52.0,
        }
        result = run_chromium_webgl.summarize_host_measurement(
            begin, end, 125, [begin, end]
        )
        self.assertEqual(result["active_cpu_seconds"], 3.0)
        self.assertEqual(result["average_cpu_cores"], 1.5)
        self.assertEqual(result["snapshots"], [begin, end])

    EMULATOR_DEATH = (
        "Darwin POSIX fault signal=11 pc=0x1002897b0 far=0x11a1471b0 "
        "esr=0x92000007\n"
        "[   76.186628] Kernel panic - not syncing: Kernel tried to access "
        "user memory at addr 0x11a1471b0, ip 0x1002897b0\n"
        "Darwin UML panic: Kernel tried to access user memory at addr "
        "0x11a1471b0, ip 0x1002897b0\n"
        "run_gpu_guest.py: emulator exited with status -6\n"
    )

    def test_emulator_death_is_named_from_the_console(self) -> None:
        death = run_chromium_webgl.describe_emulator_death(
            "UML_AQUARIUM_OK|AVG_FPS=27.000\n" + self.EMULATOR_DEATH, 1
        )
        self.assertIsNotNone(death)
        self.assertIn("killed by signal 6 (SIGABRT)", death)
        self.assertIn("far=0x11a1471b0", death)
        self.assertIn("Kernel panic", death)
        self.assertIsNone(
            run_chromium_webgl.describe_emulator_death("UML_AQUARIUM_OK|\n", 0)
        )
        self.assertIsNone(
            run_chromium_webgl.describe_emulator_death(
                "UML_AQUARIUM_OK|\n", None
            )
        )
        self.assertIn(
            "harness exited with status 2",
            run_chromium_webgl.describe_emulator_death("boot\n", 2),
        )

    def test_accounting_that_moves_backwards_names_the_dead_emulator(
        self,
    ) -> None:
        """An emulator that dies while the result marker reaches the console
        leaves the measured process tree with less CPU than it began with;
        the runner reports the death rather than the accounting."""
        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "console.log"
            log_path.write_text(
                "UML_AQUARIUM_MEASURE_BEGIN|\n", encoding="utf-8"
            )
            snapshots = [
                {
                    "pids": [10, 11],
                    "cpu_seconds": 50.0,
                    "rss_kib": 100,
                    "monotonic_seconds": 1.0,
                },
                {
                    "pids": [10],
                    "cpu_seconds": 1.0,
                    "rss_kib": 10,
                    "monotonic_seconds": 2.0,
                },
            ]

            def snapshot(pid: int) -> dict[str, object]:
                del pid
                if len(snapshots) == 2:
                    with log_path.open("a", encoding="utf-8") as log:
                        log.write(
                            "UML_AQUARIUM_OK|AVG_FPS=27.000\n"
                            + self.EMULATOR_DEATH
                        )
                return snapshots.pop(0)

            process = ExitingProcess()
            with mock.patch.object(
                run_chromium_webgl, "process_tree_snapshot", snapshot
            ), mock.patch.object(run_chromium_webgl.time, "sleep"):
                with self.assertRaisesRegex(
                    RuntimeError,
                    r"emulator died during the measurement: the emulator "
                    r"was killed by signal 6 \(SIGABRT\); console: Darwin "
                    r"POSIX fault",
                ):
                    run_chromium_webgl.wait_for_log(
                        process,
                        log_path,
                        run_chromium_webgl.GUEST_RESULT_MARKERS,
                        60,
                        host_process_pid=10,
                    )
            self.assertTrue(process.waited)

    def test_running_emulator_with_a_clean_console_is_not_waited_for(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "console.log"
            log_path.write_text("UML_AQUARIUM_OK|\n", encoding="utf-8")
            process = ExitingProcess()
            self.assertIsNone(
                run_chromium_webgl.emulator_death(
                    process, log_path, settle=False
                )
            )
            self.assertFalse(process.waited)
            log_path.write_text(
                "UML_AQUARIUM_OK|\n" + self.EMULATOR_DEATH, encoding="utf-8"
            )
            self.assertIn(
                "signal 6",
                run_chromium_webgl.emulator_death(
                    process, log_path, settle=False
                ),
            )
            self.assertTrue(process.waited)

    def test_guest_that_exits_early_names_the_emulator_death(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "console.log"
            log_path.write_text(
                "booting\n" + self.EMULATOR_DEATH, encoding="utf-8"
            )
            process = mock.Mock()
            process.poll.return_value = 1
            process.returncode = 1
            with self.assertRaisesRegex(
                RuntimeError,
                r"guest exited before Chromium verification \(status 1\): "
                r"the emulator was killed by signal 6",
            ):
                run_chromium_webgl.wait_for_log(
                    process,
                    log_path,
                    run_chromium_webgl.GUEST_RESULT_MARKERS,
                    60,
                )


class AquariumCaptureTests(unittest.TestCase):
    def test_varied_scene_and_raster_marker_are_detected(self) -> None:
        width = 1280
        height = 720
        pixels = bytearray(width * height * 3)
        for y in range(height):
            for x in range(width):
                offset = (y * width + x) * 3
                pixels[offset] = (x * 3) & 255
                pixels[offset + 1] = (y * 5) & 255
                pixels[offset + 2] = ((x + y) * 7) & 255
        for y in range(90, 110):
            for x in range(width - 260, width - 240):
                offset = (y * width + x) * 3
                pixels[offset : offset + 3] = b"\x00\xff\x00"

        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "aquarium.ppm"
            capture.write_bytes(
                f"P6\n{width} {height}\n255\n".encode("ascii") + pixels
            )
            self.assertTrue(
                run_chromium_webgl.raster_marker_visible(capture)
            )
            self.assertTrue(
                run_chromium_webgl.aquarium_scene_visible(capture)
            )


class AuthenticatedRootfsValidationTests(unittest.TestCase):
    """The erofs branch of validate_rootfs shells out to tools/rootfs/verify.py.

    It reaches for `subprocess` and `sys`, which the module did not import, so
    every erofs-backed runner (run_chromium_webgl, run_webgl_samples) died with
    NameError before it ever launched a guest. The ext4 branch returns earlier
    and hid it.
    """

    def test_erofs_rootfs_validation_invokes_the_image_verifier(self) -> None:
        common = run_chromium_webgl.kernarium_common
        contract = mock.Mock(filesystem_type="erofs")
        with tempfile.TemporaryDirectory() as directory:
            rootfs = Path(directory) / "rootfs.erofs"
            rootfs.write_bytes(b"\x00" * 64)
            identity = {"rootfs": {"size": rootfs.stat().st_size}}
            with mock.patch.object(common.subprocess, "run") as run:
                common.validate_rootfs(rootfs, identity, contract)
            run.assert_called_once()
            argv = run.call_args.args[0]
            self.assertEqual(argv[0], common.sys.executable)
            self.assertIn("--image", argv)
            self.assertIn(str(rootfs), argv)

    def test_erofs_rootfs_of_the_wrong_size_is_refused(self) -> None:
        common = run_chromium_webgl.kernarium_common
        contract = mock.Mock(filesystem_type="erofs")
        with tempfile.TemporaryDirectory() as directory:
            rootfs = Path(directory) / "rootfs.erofs"
            rootfs.write_bytes(b"\x00" * 64)
            identity = {"rootfs": {"size": 65}}
            with mock.patch.object(common.subprocess, "run") as run:
                with self.assertRaises(Exception):
                    common.validate_rootfs(rootfs, identity, contract)
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
