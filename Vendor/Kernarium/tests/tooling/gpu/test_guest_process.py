#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import io
import json
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

INTEGRATION = Path(__file__).resolve().parents[2] / "integration/gpu"
sys.path.insert(0, str(INTEGRATION))
import guest_process as owned  # noqa: E402
import run_gpu_guest as guest  # noqa: E402
import run_graphics_cts_campaign as campaign  # noqa: E402
import run_vk_gl_cts as common  # noqa: E402

TOKEN = "a" * 32

# How long a test waits for a child process or a worker thread it started. The
# wait is a hard bound that only a failure reaches, so it outlasts a host under
# many times its normal load instead of being sized to the time a test needs.
WAIT_SECONDS = 60


def setting(path: Path) -> str:
    return json.dumps({"path": str(path), "token": TOKEN})


def wait_for(path: Path) -> str:
    deadline = time.monotonic() + WAIT_SECONDS
    while time.monotonic() < deadline:
        if path.exists() and (value := path.read_text()):
            return value
        time.sleep(0.01)
    raise TimeoutError(f"test-owned process did not publish {path.name}")


def wait_for_exit(process_id: int) -> None:
    """Wait for a process to exit without reaping it."""
    queue = select.kqueue()
    try:
        queue.control([select.kevent(process_id, select.KQ_FILTER_PROC,
                                     select.KQ_EV_ADD, select.KQ_NOTE_EXIT)], 0)
        if not queue.control(None, 1, WAIT_SECONDS):
            raise TimeoutError(f"test-owned process {process_id} did not exit")
    except ProcessLookupError:
        pass  # it has exited already
    finally:
        queue.close()


class GuestProcessTests(unittest.TestCase):
    def test_cleanup_is_armed_during_popen_and_preserves_interrupt(self) -> None:
        process = mock.Mock(pid=1002)
        process.wait.return_value = 0
        cleanup = mock.Mock()
        stop = mock.Mock()

        def spawn(*_args, **_kwargs):
            os.kill(os.getpid(), signal.SIGINT)
            os.kill(os.getpid(), signal.SIGTERM)
            return process

        previous = {number: signal.getsignal(number)
                    for number in (signal.SIGINT, signal.SIGTERM)}
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / owned.JOURNAL_NAME
            with mock.patch.object(owned.subprocess, "Popen", side_effect=spawn), self.assertRaises(KeyboardInterrupt):
                with owned.managed_child(["fixture"], stop=stop, cleanup=cleanup,
                                         journal_setting=setting(path)):
                    self.fail("deferred launch interrupt was lost")
            stop.assert_called_once_with(process)
            cleanup.assert_called_once_with()
            owned.validate_journal(path, TOKEN, required=True)
        self.assertEqual(previous, {number: signal.getsignal(number) for number in previous})

    def test_interrupt_is_raised_once_per_child(self) -> None:
        for deferred in (False, True):
            with self.subTest(deferred=deferred), owned.Interrupts() as interrupts:
                if deferred:
                    interrupts.handle(signal.SIGINT, None)
                    with self.assertRaises(KeyboardInterrupt):
                        interrupts.running()
                else:
                    interrupts.running()
                    with self.assertRaises(KeyboardInterrupt):
                        interrupts.handle(signal.SIGINT, None)
                try:
                    interrupts.handle(signal.SIGTERM, None)
                    interrupts.handle(signal.SIGINT, None)
                except KeyboardInterrupt:
                    self.fail("a repeated signal raised a second interrupt")

    @unittest.skipUnless(hasattr(signal, "pthread_kill"), "requires POSIX thread signals")
    def test_repeated_signal_during_unwinding_cannot_skip_cleanup(self) -> None:
        process = mock.Mock(pid=1002)
        process.wait.return_value = -signal.SIGINT
        cleanup = mock.Mock()
        stop = mock.Mock()
        numbers = {signal.SIGINT, signal.SIGTERM}
        previous = {number: signal.getsignal(number) for number in numbers}
        go_read, go_write = os.pipe()
        done_read, done_write = os.pipe()

        def deliver() -> None:
            # Both C-level handlers run on this thread in one unblock. The main
            # thread then runs SIGINT at its next check, and SIGTERM stays due
            # while that interrupt unwinds into the context manager's exit.
            if os.read(go_read, 1):
                signal.pthread_sigmask(signal.SIG_BLOCK, numbers)
                for number in sorted(numbers):
                    signal.pthread_kill(threading.get_ident(), number)
                signal.pthread_sigmask(signal.SIG_UNBLOCK, numbers)
            os.write(done_write, b"x")

        helper = threading.Thread(target=deliver)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / owned.JOURNAL_NAME
            try:
                with mock.patch.object(owned.subprocess, "Popen", return_value=process):
                    try:
                        with owned.managed_child(["fixture"], stop=stop, cleanup=cleanup,
                                                 journal_setting=setting(path)):
                            helper.start()
                            os.write(go_write, b"x")
                            select.select([done_read], [], [], 5)
                            self.fail("running interrupt was not delivered")
                    except KeyboardInterrupt:
                        # The live exception still references every unwound frame,
                        # so generator finalization cannot supply a skipped cleanup.
                        stop.assert_called_once_with(process)
                        cleanup.assert_called_once_with()
                        owned.validate_journal(path, TOKEN, required=True)
                self.assertEqual(previous, {number: signal.getsignal(number) for number in numbers})
            finally:
                os.close(go_write)
                if helper.ident is not None:
                    helper.join(timeout=WAIT_SECONDS)
                for descriptor in (go_read, done_read, done_write):
                    os.close(descriptor)
                for number, handler in previous.items():
                    signal.signal(number, handler)
            self.assertFalse(helper.is_alive())

    def test_body_error_is_not_changed_to_success_by_reap_ack(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / owned.JOURNAL_NAME
            process = mock.Mock(pid=1002)
            process.wait.return_value = -signal.SIGKILL
            with mock.patch.object(owned.subprocess, "Popen", return_value=process), self.assertRaisesRegex(OSError, "setup failure"):
                with owned.managed_child(["fixture"], stop=mock.Mock(), journal_setting=setting(path)):
                    raise OSError("setup failure")
            owned.validate_journal(path, TOKEN, required=True)

    def test_spawn_failure_is_acknowledged_only_as_not_started(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / owned.JOURNAL_NAME
            cleanup = mock.Mock()
            with mock.patch.object(owned.subprocess, "Popen", side_effect=OSError("spawn failure")), self.assertRaises(OSError):
                with owned.managed_child(["fixture"], stop=mock.Mock(), cleanup=cleanup,
                                         journal_setting=setting(path)):
                    self.fail("spawn should fail")
            cleanup.assert_called_once_with()
            owned.validate_journal(path, TOKEN)
            with self.assertRaises(owned.OwnershipError):
                owned.validate_journal(path, TOKEN, required=True)

    def test_close_failure_reaps_child_but_cannot_acknowledge_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / owned.JOURNAL_NAME
            process = mock.Mock(pid=1002)
            process.wait.return_value = 0
            stop = mock.Mock()
            with mock.patch.object(owned.subprocess, "Popen", return_value=process), self.assertRaisesRegex(OSError, "close failure"):
                with owned.managed_child(["fixture"], stop=stop, journal_setting=setting(path),
                                         cleanup=mock.Mock(side_effect=OSError("close failure"))):
                    pass
            stop.assert_called_once_with(process)
            process.wait.assert_called_once_with(timeout=5)
            with self.assertRaises(owned.OwnershipError):
                owned.validate_journal(path, TOKEN)

    def test_journal_rejects_wrong_identity_duplicates_truncation_and_bounds(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / owned.JOURNAL_NAME
            valid = [
                {"event": "starting", "token": TOKEN, "owner_pid": 1001},
                {"event": "started", "pid": 1002},
                {"event": "reaped", "pid": 1002, "return_code": 0},
            ]
            payload = "".join(json.dumps(row) + "\n" for row in valid)
            path.write_text(payload)
            owned.validate_journal(path, TOKEN, required=True)
            for invalid in (payload[:-1], payload + payload.splitlines()[2] + "\n",
                            payload.replace(TOKEN, "b" * 32),
                            payload.replace('"return_code": 0', '"return_code": false'),
                            payload.replace('"return_code": 0', '"return_code": 1, "return_code": 0'),
                            payload.replace('"pid": 1002', '"pid": 1003', 1),
                            "x" * (owned.MAX_JOURNAL_BYTES + 1), "\u00ff\n"):
                path.write_text(invalid)
                with self.subTest(invalid=invalid[:60]), self.assertRaises(owned.OwnershipError):
                    owned.validate_journal(path, TOKEN)
            path.unlink()
            owned.validate_journal(path, TOKEN)
            with self.assertRaises(owned.OwnershipError):
                owned.validate_journal(path, TOKEN, required=True)

    def test_existing_journal_cannot_be_replaced(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / owned.JOURNAL_NAME
            path.write_text("retain this unrelated file")
            with mock.patch.object(owned.subprocess, "Popen") as spawn, self.assertRaises(FileExistsError):
                with owned.managed_child(["fixture"], stop=mock.Mock(), journal_setting=setting(path)):
                    self.fail("exclusive creation should fail")
            spawn.assert_not_called()
            self.assertEqual(path.read_text(), "retain this unrelated file")

    def test_pool_requires_ack_even_when_owned_group_is_gone(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / owned.JOURNAL_NAME
            path.write_text(json.dumps({"event": "starting", "token": TOKEN, "owner_pid": 1001}) + "\n")
            process = mock.Mock(pid=1001)
            process.poll.return_value = 0
            with mock.patch.object(campaign, "group_exists", return_value=False), self.assertRaisesRegex(common.CampaignError, "incomplete"):
                campaign.stop_owned_group(process, ownership=(path, TOKEN))

    def test_pool_grace_contains_complete_nested_cleanup_budget(self) -> None:
        self.assertGreater(owned.LEAF_CLEANUP_GRACE,
                           sum(timeout for _, timeout in owned.HARNESS_STOP) + 5)
        self.assertGreater(owned.HARNESS_STOP[1][1],
                           sum(timeout for _, timeout in owned.EMULATOR_STOP))

    def test_successful_leaf_cannot_release_slot_without_journal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / owned.JOURNAL_NAME
            process = mock.Mock(pid=1001, returncode=0)
            process.poll.return_value = 0
            with mock.patch.object(campaign, "group_exists", return_value=False), self.assertRaisesRegex(common.CampaignError, "missing"):
                campaign.stop_owned_group(process, ownership=(path, TOKEN))

    def test_socket_cleanup_preserves_replacement_and_unobserved_nodes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "artifact.sock"
            tracker = owned.SocketOwnership([f"--virtio-serial-socket={path}"])
            with socket.socket(socket.AF_UNIX) as endpoint:
                endpoint.bind(str(path))
                path.chmod(0o600)
                with self.assertRaises(owned.OwnershipError):
                    tracker.close()
                tracker.observe()
                path.unlink()
                path.write_text("unrelated replacement")
                with self.assertRaises(owned.OwnershipError):
                    tracker.close()
                self.assertEqual(path.read_text(), "unrelated replacement")
            with self.assertRaises(owned.OwnershipError):
                owned.SocketOwnership([f"--virtio-serial-socket={path}"])

    def test_exact_observed_socket_is_removed_after_owner_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "artifact.sock"
            tracker = owned.SocketOwnership([f"--virtio-serial-socket={path}"])
            with socket.socket(socket.AF_UNIX) as endpoint:
                endpoint.bind(str(path))
                path.chmod(0o600)
                tracker.observe()
            tracker.close()
            self.assertFalse(path.exists())

    def test_post_spawn_console_setup_error_still_reaps_real_child(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            spawned = []
            real_spawn = subprocess.Popen

            def spawn(*args, **kwargs):
                process = real_spawn(*args, **kwargs)
                spawned.append(process)
                return process

            with mock.patch.object(owned.subprocess, "Popen", side_effect=spawn), mock.patch.object(
                guest.os, "set_blocking", side_effect=OSError("console setup failure")
            ), self.assertRaisesRegex(OSError, "console setup failure"):
                guest.run_guest([sys.executable, "-c", "import time; time.sleep(30)"],
                                root / "console.log", 2, False)
            self.assertEqual(len(spawned), 1)
            self.assertIsNotNone(spawned[0].poll())

    def test_serial_close_error_cannot_skip_real_child_reap(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            spawned = []
            real_spawn = subprocess.Popen
            real_close = guest.SerialConsoleSocket.close

            def spawn(*args, **kwargs):
                process = real_spawn(*args, **kwargs)
                spawned.append(process)
                return process

            def fail_close(endpoint):
                real_close(endpoint)
                raise OSError("serial close failure")

            with mock.patch.object(owned.subprocess, "Popen", side_effect=spawn), mock.patch.object(
                guest.SerialConsoleSocket, "close", fail_close
            ), self.assertRaisesRegex(OSError, "serial close failure"):
                guest.run_guest([sys.executable, "-c", "import time; time.sleep(30)"],
                                root / "console.log", 0.05, False, root / "serial.sock")
            self.assertIsNotNone(spawned[0].poll())
            self.assertFalse((root / "serial.sock").exists())

    def test_a_late_console_read_keeps_the_guest_verdict(self) -> None:
        # The runner reads nothing until the emulator has exited, as one on a
        # loaded host can, and the emulator's console is non-blocking, as the
        # guest kernel's console makes it.
        emulator = (
            "import fcntl, os, sys\n"
            "flags = fcntl.fcntl(1, fcntl.F_GETFL)\n"
            "fcntl.fcntl(1, fcntl.F_SETFL, flags | os.O_NONBLOCK)\n"
            "os.write(1, sys.argv[1].encode() + b'\\n')\n"
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            spawned = []
            real_spawn = subprocess.Popen
            real_select = select.select

            def spawn(*args, **kwargs):
                process = real_spawn(*args, **kwargs)
                spawned.append(process)
                return process

            def select_after_exit(*args, **kwargs):
                wait_for_exit(spawned[0].pid)
                return real_select(*args, **kwargs)

            with mock.patch.object(owned.subprocess, "Popen", side_effect=spawn), mock.patch.object(
                guest.select, "select", side_effect=select_after_exit
            ), contextlib.redirect_stdout(io.TextIOWrapper(io.BytesIO())):
                result = guest.run_guest(
                    [sys.executable, "-I", "-S", "-c", emulator, guest.PASS_MARKER],
                    root / "console.log", WAIT_SECONDS, False)
            self.assertEqual(result, (0, True, False))
            self.assertIn(guest.PASS_MARKER, (root / "console.log").read_text())

    def test_a_killed_emulator_leaves_no_run_directories(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scratch = root / "tmp"
            scratch.mkdir()
            marker = root / "emulator.pid"
            # An emulator that ignores SIGTERM, as a wedged one does, so the
            # stop ends it with SIGKILL and no handler of its own runs.
            emulator = (
                "trap '' TERM; "
                'mkdir "$TMPDIR/uml-kern$$" "$TMPDIR/uml-guest-initrd-$$-x"; '
                'echo $$ > "$1"; exec sleep 30'
            )
            # Another run's directory is that run's.
            other = scratch / f"uml-kern{os.getpid()}"
            other.mkdir()
            with mock.patch.dict(os.environ, {"TMPDIR": str(scratch)}), mock.patch.object(
                tempfile, "tempdir", str(scratch)
            ), mock.patch.object(owned, "EMULATOR_STOP", ((signal.SIGTERM, 0.3), (signal.SIGKILL, 5))):
                process = subprocess.Popen(["/bin/sh", "-c", emulator, "emulator", str(marker)])
                try:
                    pid = int(wait_for(marker))
                    self.assertTrue((scratch / f"uml-kern{pid}").is_dir())
                    guest.terminate_process(process)
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.wait(timeout=WAIT_SECONDS)
            self.assertEqual(process.returncode, -signal.SIGKILL)
            self.assertEqual(sorted(path.name for path in scratch.iterdir()), [other.name])

    @unittest.skipUnless(os.name == "posix", "requires POSIX process ownership")
    def test_real_nested_cancellation_reaps_separate_session_with_repeated_signals(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            journal = root / owned.JOURNAL_NAME
            marker = root / "emulator.json"
            endpoint = root / "artifact.sock"
            emulator = (
                "import json,os,pathlib,signal,socket,sys,time; os.setsid(); "
                "signal.signal(signal.SIGINT,signal.SIG_IGN); "
                "signal.signal(signal.SIGTERM,signal.SIG_IGN); "
                "s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); os.chmod(sys.argv[1],0o600); "
                "pathlib.Path(sys.argv[2]).write_text(json.dumps({'pid':os.getpid(),'pgid':os.getpgrp()})); "
                "time.sleep(30)"
            )
            harness = (
                f"import sys,signal; sys.dont_write_bytecode=True; sys.path.insert(0,{str(INTEGRATION)!r}); "
                "import run_gpu_guest as g; "
                "g.guest_process.EMULATOR_STOP=((signal.SIGTERM,0.3),(signal.SIGKILL,2)); "
                f"g.run_guest({[sys.executable, '-c', emulator, str(endpoint), str(marker), '--virtio-serial-socket=' + str(endpoint)]!r}, "
                f"__import__('pathlib').Path({str(root / 'console.log')!r}),30,False)"
            )
            leaf = (
                f"import sys,signal,time; sys.dont_write_bytecode=True; sys.path.insert(0,{str(INTEGRATION)!r}); "
                "import run_vk_gl_cts as c; "
                "c.guest_process.HARNESS_STOP=((signal.SIGINT,3),(signal.SIGTERM,3),(signal.SIGKILL,2));\n"
                f"with c.guest_process.managed_child({[sys.executable, '-B', '-c', harness]!r},stop=c.stop_guest) as child:\n"
                " child.wait(timeout=30)\n"
            )
            unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"], start_new_session=True)
            process = subprocess.Popen([sys.executable, "-B", "-c", leaf], start_new_session=True,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                       env={**os.environ, owned.OWNERSHIP_ENV: setting(journal)})
            child_pid = None
            try:
                record = json.loads(wait_for(marker))
                child_pid = record["pid"]
                self.assertEqual(child_pid, record["pgid"])
                self.assertNotEqual(child_pid, process.pid)

                def repeat_interrupts() -> None:
                    for _ in range(4):
                        time.sleep(0.04)
                        try:
                            os.killpg(process.pid, signal.SIGINT)
                            os.killpg(process.pid, signal.SIGTERM)
                        except ProcessLookupError:
                            return

                repeats = threading.Thread(target=repeat_interrupts)
                repeats.start()
                campaign.stop_owned_group(process, grace=5, term_grace=3, kill_grace=2,
                                          ownership=(journal, TOKEN))
                repeats.join(timeout=WAIT_SECONDS)
                self.assertFalse(repeats.is_alive())
                owned.validate_journal(journal, TOKEN, required=True)
                self.assertIsNotNone(process.poll())
                self.assertFalse(campaign.group_exists(process.pid))
                with self.assertRaises(ProcessLookupError):
                    os.kill(child_pid, 0)
                child_pid = None
                self.assertFalse(endpoint.exists())
                self.assertIsNone(unrelated.poll())
            finally:
                # Exact test-owned handles/record only; never scan for emulators.
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=WAIT_SECONDS)
                if campaign.group_exists(process.pid):
                    os.killpg(process.pid, signal.SIGKILL)
                if child_pid is not None:
                    try:
                        os.kill(child_pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                unrelated.kill()
                unrelated.wait(timeout=WAIT_SECONDS)


if __name__ == "__main__":
    unittest.main()
