# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared serial-console guest harness for integration and performance runners."""

from __future__ import annotations
# Imported support must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import dataclasses
import functools
import os
import pathlib
import pty
import re
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid
from collections.abc import Mapping, Sequence
from typing import BinaryIO


ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
BOOT_TIMEOUT_SECONDS = 90
CONSOLE_WRITE_TIMEOUT_SECONDS = 30
# A failing session prints this much of its transcript's end.
FAILURE_TRANSCRIPT_CHARACTERS = 16384
SIZE_VALUE = re.compile(r"^(\d+)([KMGT]?)$", re.IGNORECASE)
SWAP_IMAGE = re.compile(r"^uml-ltp-swap-(\d+)-.*\.img$")
STAGING_PREFIX = "uml-guest-files-"
STAGING_DIRECTORY = re.compile(rf"^{STAGING_PREFIX}(\d+)-.+$")
REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
# The launcher boots every guest through its base archive and a per-run
# initramfs (tools/rootfs/boot-ramdisk/README.md); this session prepares the
# shell the harness drives.
SESSION_SCRIPT = REPOSITORY / "tools" / "rootfs" / "boot-ramdisk" / "session"
SESSION_GUEST_PATH = "/usr/local/libexec/uml-session"
SESSION_PARAMETERS_GUEST_PATH = "/etc/uml-session.env"
SESSION_READY_MARKER = "UML_SESSION_READY:"
SWAP_DEVICE = "/dev/ubdb"
# The session mounts fresh filesystems over these, and the kernel owns the
# rest, so a payload placed there would never be seen.
HIDDEN_PAYLOAD_ROOTS = ("/tmp", "/run", "/dev", "/proc", "/sys")
# The directories an emulator makes for its run, named after its process: the
# guest console's and the launcher's initrd scratch.
EMULATOR_RUN_DIRECTORIES = (
    re.compile(r"^uml-kern(\d+)$"),
    re.compile(r"^uml-guest-initrd-(\d+)-.+$"),
)
# What a kernel prints once when a self-check turns a host fast path off for
# the rest of its run. The run stays correct and only slows down, so a boot
# that prints one fails here instead of passing unnoticed: today the CPU read
# through a carrier's thread-specific data (kernel/common/README.md).
DISABLED_FAST_PATH_WARNINGS = ("thread-specific data is not where expected",)


def require_host_fast_paths(transcript: str) -> None:
    """Refuse a boot whose kernel turned a host fast path off."""
    for line in transcript.splitlines():
        if any(warning in line for warning in DISABLED_FAST_PATH_WARNINGS):
            raise RuntimeError(
                f"the guest kernel turned a host fast path off: {line.strip()}"
            )


@dataclasses.dataclass(frozen=True)
class CommandResult:
    return_code: int
    output: str


class CommandTimeout(TimeoutError):
    """A command's end marker did not arrive in time.

    ``output`` holds what the command printed before the deadline."""

    def __init__(self, message: str, output: str) -> None:
        super().__init__(message)
        self.output = output


CORE_CLASSES = ("performance", "efficiency")
TASKPOLICY = "/usr/sbin/taskpolicy"


def core_class_prefix(core_class: str) -> list[str]:
    """Return the launcher prefix that selects a host core class.

    Apple's scheduler places a process-wide background QoS clamp on the
    efficiency cores, so ``taskpolicy -c background`` is how a measurement runs
    on the core type that dominates iOS.  The clamp is a task ceiling, so the
    emulator's vCPU threads inherit it even though they are created later.
    """
    if core_class not in CORE_CLASSES:
        raise ValueError(f"unknown core class {core_class!r}")
    if core_class == "performance":
        return []
    if sys.platform != "darwin" or not os.path.exists(TASKPOLICY):
        raise RuntimeError(
            "efficiency-core measurement requires Darwin's taskpolicy"
        )
    return [TASKPOLICY, "-c", "background"]


def jit_tier_arguments(engine: str) -> list[str]:
    """``--jit-tier`` for an x86-64 JIT guest when UML_TEST_JIT_TIER names a
    threshold, so a runner exercises the tiered engine unchanged."""
    threshold = os.environ.get("UML_TEST_JIT_TIER", "")
    if engine != "jit" or not threshold:
        return []
    if not threshold.isdecimal():
        raise ValueError(f"UML_TEST_JIT_TIER is not a count: {threshold!r}")
    return [f"--jit-tier={threshold}"]


def emulator_arguments(
    emulator: pathlib.Path,
    rootfs: pathlib.Path,
    memory: str,
    kernel_arguments: Sequence[str] = (),
    swap_path: pathlib.Path | None = None,
    init_path: str | None = None,
    engine: str = "jit",
    core_class: str = "performance",
    guest_files: Sequence[tuple[str, pathlib.Path]] = (),
) -> list[str]:
    """The launcher command for one guest.

    The launcher gives a guest min(physical cores, 8) vCPUs unless a
    ``kernel_arguments`` entry is ``ncpus=N``, so a test states the count it
    runs with.

    ``guest_files`` become the per-run initramfs the launcher builds, whose
    /init mounts the image. A guest that boots straight into an init on its
    EROFS image names it with ``init_path`` instead, and the launcher then
    leaves the boot to the kernel."""
    if init_path is not None and guest_files:
        raise ValueError("a guest boots either its own init or guest files")
    direct_boot = (
        ("root=/dev/ubda", "rootfstype=erofs", f"init={init_path}")
        if init_path is not None
        else ()
    )
    arguments = [
        *core_class_prefix(core_class),
        str(emulator),
        f"--execution-engine={engine}",
        *jit_tier_arguments(engine),
        *(f"--guest-file={guest}={host}" for guest, host in guest_files),
        f"ubd0c={rootfs}",
        *direct_boot,
        f"mem={memory}",
    ]
    if swap_path is not None:
        arguments.insert(-1, f"ubd1={swap_path}")
    arguments.extend(kernel_arguments)
    return arguments


@dataclasses.dataclass(frozen=True)
class GuestFile:
    """A file the per-run initramfs installs in the guest before it starts."""

    contents: bytes
    mode: int = 0o644


def validate_payload_path(path: str) -> None:
    """Payload paths are absolute, clean and outside what the session hides."""
    parts = path.split("/")
    if (
        not path.startswith("/")
        or any(part in ("", ".", "..") for part in parts[1:])
        or "=" in path
    ):
        raise ValueError(f"invalid guest payload path {path!r}")
    for hidden in HIDDEN_PAYLOAD_ROOTS:
        if path == hidden or path.startswith(hidden + "/"):
            raise ValueError(
                f"guest payload {path!r} would be hidden by {hidden}"
            )


def session_parameters(
    *,
    overlay_shell: bool,
    workspace: str,
    swap: bool,
    cgroup1: str | None,
    cgroup2_nsdelegate: bool,
    network: str = "user-network",
) -> str:
    """The session file for a harness shell; see tools/rootfs/boot-ramdisk.

    ``network`` is how an overlay shell gets its address and resolver: the
    SLiRP lease by default, or whatever the image configures itself, which
    for a guest with no network is nothing."""
    if network not in ("user-network", "image"):
        raise ValueError(f"unknown network {network!r}")
    if workspace not in ("tmpfs", "ext2"):
        raise ValueError(f"unknown workspace {workspace!r}")
    if workspace != "tmpfs" and not overlay_shell:
        raise ValueError("a workspace needs the overlay shell environment")
    lines = ["UML_SESSION_ENTRY=/bin/sh"]
    if overlay_shell:
        lines += [
            f"UML_SESSION_RESOLVER={network}",
            "UML_SESSION_RUNTIME_MOUNTS=1",
            f"UML_SESSION_NETWORK={network}",
            f"UML_SESSION_WORKSPACE={workspace}",
        ]
    if swap:
        lines.append(f"UML_SESSION_SWAP={SWAP_DEVICE}")
    if cgroup2_nsdelegate:
        lines.append("UML_SESSION_CGROUP=v2-nsdelegate")
    elif cgroup1 is not None:
        lines.append(f"UML_SESSION_CGROUP=v1:{cgroup1}")
    return "".join(f"{line}\n" for line in lines)


def stage_guest_files(
    directory: pathlib.Path,
    session: str,
    payloads: Mapping[str, GuestFile],
    root_type: str | None = None,
) -> list[tuple[str, pathlib.Path]]:
    """Write a harness session's initramfs inputs and return their pairs.

    The root on ``/dev/ubda`` is an EROFS image unless ``root_type`` says it
    is an unpartitioned ext4 filesystem."""
    if root_type is not None and root_type not in ("erofs", "ext4"):
        raise ValueError(f"unknown root type {root_type!r}")
    boot = directory / "boot.env"
    boot_lines = [f"UML_BOOT_ENTRY={SESSION_GUEST_PATH}\n"]
    if root_type is not None:
        boot_lines.append(f"UML_BOOT_ROOT_TYPE={root_type}\n")
    boot.write_text("".join(boot_lines))
    parameters = directory / "session.env"
    parameters.write_text(session)
    files = [
        ("/boot.env", boot),
        (f"/payload{SESSION_GUEST_PATH}", SESSION_SCRIPT),
        (f"/payload{SESSION_PARAMETERS_GUEST_PATH}", parameters),
    ]
    payload_directory = directory / "payload"
    for index, (path, payload) in enumerate(sorted(payloads.items())):
        validate_payload_path(path)
        if path in (SESSION_GUEST_PATH, SESSION_PARAMETERS_GUEST_PATH):
            raise ValueError(f"guest payload {path!r} replaces the session")
        payload_directory.mkdir(exist_ok=True)
        host = payload_directory / f"{index:04d}"
        host.write_bytes(payload.contents)
        host.chmod(payload.mode)
        files.append((f"/payload{path}", host))
    return files


def size_bytes(value: str) -> int:
    """Parse a memory or disk size using binary K/M/G/T suffixes."""
    match = SIZE_VALUE.fullmatch(value)
    if match is None:
        raise ValueError(f"invalid size {value!r}")
    suffix_shift = {"": 0, "K": 10, "M": 20, "G": 30, "T": 40}
    size = int(match.group(1)) << suffix_shift[match.group(2).upper()]
    if size < 4096:
        raise ValueError(f"size must be at least 4096 bytes: {value!r}")
    return size


def abandoned_host_files(pattern: re.Pattern[str]) -> list[pathlib.Path]:
    """Return the temporary files ``pattern`` names whose process has exited.

    A harness names each per-run host file after its process. One killed by a
    signal it cannot handle runs no cleanup, so later harnesses remove what it
    left, as the launcher does for its initrd scratch. A process that is alive,
    or that this user may not signal, keeps its files."""
    abandoned = []
    for path in pathlib.Path(tempfile.gettempdir()).glob("uml-*"):
        match = pattern.fullmatch(path.name)
        if match is None:
            continue
        try:
            os.kill(int(match.group(1)), 0)
        except ProcessLookupError:
            abandoned.append(path)
        except (PermissionError, OverflowError):
            pass
    return abandoned


def cleanup_stale_swap_images() -> None:
    """Remove sparse swap backings left by terminated harness processes."""
    for path in abandoned_host_files(SWAP_IMAGE):
        path.unlink(missing_ok=True)


def cleanup_stale_guest_files() -> None:
    """Remove the staged guest files of terminated harness processes."""
    for path in abandoned_host_files(STAGING_DIRECTORY):
        if path.is_dir() and not path.is_symlink():
            shutil.rmtree(path, ignore_errors=True)


@functools.lru_cache(maxsize=None)
def darwin_user_temporary_directory() -> pathlib.Path | None:
    """Darwin's per-user temporary directory, which tempfile never consults."""
    try:
        result = subprocess.run(
            ["/usr/bin/getconf", "DARWIN_USER_TEMP_DIR"],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=10,
            check=True,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    directory = result.stdout.strip()
    return pathlib.Path(directory) if directory.startswith("/") else None


def remove_emulator_run_directories(process_id: int) -> None:
    """Remove the run directories of an emulator that has exited.

    An emulator removes them itself on every exit it observes, and a later
    emulator removes those of runs since killed outright. A harness that kills
    one removes them at once, so its test leaves nothing behind. They are in
    $TMPDIR, or in Darwin's per-user temporary directory when $TMPDIR is unset.
    Nothing is removed while the process exists."""
    try:
        os.kill(process_id, 0)
        return
    except ProcessLookupError:
        pass
    except (PermissionError, OverflowError):
        return
    directories = {pathlib.Path(tempfile.gettempdir())}
    if (user := darwin_user_temporary_directory()) is not None:
        directories.add(user)
    for directory in directories:
        for path in directory.glob(f"uml-*{process_id}*"):
            for pattern in EMULATOR_RUN_DIRECTORIES:
                match = pattern.fullmatch(path.name)
                if (
                    match is not None
                    and int(match.group(1)) == process_id
                    and path.is_dir()
                    and not path.is_symlink()
                ):
                    shutil.rmtree(path, ignore_errors=True)


def create_swap_image(value: str) -> pathlib.Path:
    """Create a per-guest sparse raw disk without allocating its data blocks."""
    descriptor, name = tempfile.mkstemp(
        prefix=f"uml-ltp-swap-{os.getpid()}-", suffix=".img"
    )
    try:
        os.ftruncate(descriptor, size_bytes(value))
    except BaseException:
        pathlib.Path(name).unlink(missing_ok=True)
        raise
    finally:
        os.close(descriptor)
    return pathlib.Path(name)


# The kernel prints its messages on the console the shell writes to, each a
# whole timestamped line written at once. The shell's output can stop short of
# a line: the terminal driver writes a line's text and its line end
# separately, and the console line writes only what the host accepts and
# queues the rest, which a kernel message then overtakes. So a kernel line can
# land between any two bytes the shell prints, protocol markers included.
KERNEL_LINE = rb"\[ *\d+\.\d+\][^\r\n]*\r?\n"
_KERNEL_LINES = rb"(?:" + KERNEL_LINE + rb")*"


def console_text(text: str) -> bytes:
    """Return a pattern for ``text`` with kernel lines anywhere inside it."""
    return _KERNEL_LINES.join(re.escape(bytes((byte,))) for byte in text.encode())


def marker_pattern(marker: str) -> re.Pattern[bytes]:
    """Match a protocol marker and its status line as the console shows it.

    Kernel lines may interleave anywhere in the marker, its status and before
    its line end, and a tty ioctl may have enabled case conversion. Read the
    status with ``marker_status()``.
    """
    return re.compile(
        console_text(f"{marker}:")
        + _KERNEL_LINES
        + rb"(\d(?:"
        + _KERNEL_LINES
        + rb"\d)*)"
        + _KERNEL_LINES
        + rb"\r?\n",
        re.IGNORECASE,
    )


def marker_status(match: re.Match[bytes]) -> int:
    """Return the status a ``marker_pattern()`` match carries."""
    return int(re.sub(KERNEL_LINE, b"", match.group(1)))


# A console prompt does not always begin a line with CRLF. When bash execs a
# replacement shell it first emits its bracketed-paste reset, and the terminal
# then returns the carriage without a line feed, so the sequence reaching the
# transcript is `\r\n<ESC>[?2004l\r# `. Anchoring on `\r?\n` alone cannot match
# that prompt at all, because `[^\r\n]*` may not cross the bare carriage
# return, so `exec sudo -n /bin/sh` timed out with the prompt already on screen.
# Accept a lone `\r` as a line start as well.
LINE_START = rb"(?:^|\r?\n|\r)"
# The prompt of the session's root shell, which is the image's own /bin/sh: dash
# prints `# ` alone, and BusyBox's ash puts the working directory before it
# (`~ # `, `/ # `). A prompt ends the transcript so far, and the search starts
# after the session's ready marker, so only the shell's own lines can precede it.
ROOT_PROMPT = LINE_START + rb"[^\r\n]*# $"

# The watchdog behind every emulator the harness starts. It holds only the
# read end of the harness's lifeline and waits for whichever comes first: the
# emulator's exit, after which it has nothing to do, or the lifeline's end.
# A harness that closes its guest writes one byte first; a lifeline that ends
# without it belongs to a harness that died, so the watchdog kills the
# emulator. The kqueue process filter follows the emulator itself, not its
# process ID, so a process that later reuses the ID is never killed, and one
# that exited before the watchdog looked ends the watchdog at once.
LIFELINE_WATCHDOG = """\
import os, select, signal, sys
emulator = int(sys.argv[1])
queue = select.kqueue()
exit_event = select.kevent(emulator, select.KQ_FILTER_PROC, select.KQ_EV_ADD,
                           select.KQ_NOTE_EXIT)
try:
    queue.control([exit_event, select.kevent(0)], 0)
except ProcessLookupError:
    sys.exit()
def exited(events):
    return any(event.filter == select.KQ_FILTER_PROC for event in events)
if exited(queue.control(None, 2)) or os.read(0, 1):
    sys.exit()
if not exited(queue.control(None, 2, 0)):
    try:
        os.kill(emulator, signal.SIGKILL)
    except ProcessLookupError:
        pass
"""
LIFELINE_STAND_DOWN = b"."


def start_lifeline(process_id: int) -> tuple[int, subprocess.Popen[bytes]]:
    """Start the watchdog of emulator ``process_id`` in this process's group
    and return the lifeline's write end, which no child inherits, with it."""
    read_end, write_end = os.pipe()
    try:
        watchdog = subprocess.Popen(
            [sys.executable, "-I", "-S", "-c", LIFELINE_WATCHDOG, str(process_id)],
            stdin=read_end,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            close_fds=True,
        )
    except BaseException:
        os.close(write_end)
        raise
    finally:
        os.close(read_end)
    return write_end, watchdog


def exit_queue(process_id: int) -> select.kqueue | None:
    """Return a kqueue that turns readable when ``process_id`` exits, or
    ``None`` when it has exited already."""
    queue = select.kqueue()
    try:
        queue.control(
            [
                select.kevent(
                    process_id,
                    select.KQ_FILTER_PROC,
                    select.KQ_EV_ADD,
                    select.KQ_NOTE_EXIT,
                )
            ],
            0,
        )
    except ProcessLookupError:
        queue.close()
        return None
    except BaseException:
        queue.close()
        raise
    return queue


class ConsoleProcess:
    """One emulator process driven through its serial console.

    Output goes to a PTY, which keeps it terminal-shaped and unbuffered;
    input goes through a pipe, because Darwin can miss PTY readiness while
    every guest CPU is busy and a pipe stays pollable at guest timer
    boundaries. Writes have their own deadline. Commands are wrapped in
    unique markers and read back from the ANSI-normalized transcript.

    The harness holds the PTY's terminal end until it closes, so everything
    the emulator wrote before it exited stays readable however late the
    harness reads it: Darwin discards a terminal's unread output at its last
    close, at once when that descriptor is non-blocking, as the guest
    kernel's console makes it, and otherwise after 0.6 s. The PTY therefore
    reports no end when the emulator exits, and the console loops wait on
    the exit itself as well.

    The emulator shares this harness's session and process group, and a
    watchdog in that group kills it if the harness dies without closing it,
    so no way of ending the harness leaves the guest running.

    The emulator's stderr shares the PTY unless ``separate_host_output`` is
    set. Shared, a host diagnostic can land inside a guest line, and the
    kernel's console makes the PTY non-blocking, so a diagnostic written
    while the PTY is full is dropped. Separate, stderr goes to a pipe of its
    own that the harness drains with the console, ``output()`` holds only
    what the guest wrote, and ``host_output()`` holds the diagnostics.
    """

    # Without separate_host_output there is no host stream. These defaults
    # also hold for an instance a test builds without __init__.
    _separate_host_output = False
    _host: int | None = None
    _host_transcript: bytes | bytearray = b""
    _terminal: int | None = None
    _exit_queue: select.kqueue | None = None

    # How long a guest has to exit after SIGTERM before it is killed.
    STOP_GRACE_SECONDS = 2.0
    KILL_WAIT_SECONDS = 30.0
    # How long a watchdog has to exit once its lifeline is closed.
    WATCHDOG_WAIT_SECONDS = 30.0

    def __init__(
        self,
        arguments: Sequence[str],
        *,
        environment: Mapping[str, str] | None = None,
        pass_fds: Sequence[int] = (),
        console_sink: BinaryIO | None = None,
        separate_host_output: bool = False,
    ) -> None:
        self._transcript = bytearray()
        self._host_transcript = bytearray()
        self._separate_host_output = separate_host_output
        # The read end of the emulator's stderr until it reaches its end.
        self._host = None
        # Receives every console byte as the harness reads it, so a caller
        # can stream a long command's output instead of waiting for its end.
        self._console_sink = console_sink
        self.first_output_at: float | None = None
        master, slave = pty.openpty()
        self._master = master
        # The terminal end stays open here until close(), so the emulator's
        # exit is never the terminal's last close.
        self._terminal = slave
        os.set_blocking(master, False)
        host_write: int | None = None
        try:
            if separate_host_output:
                self._host, host_write = os.pipe()
                os.set_blocking(self._host, False)
            self._process = subprocess.Popen(
                list(arguments),
                env=None if environment is None else dict(environment),
                stdin=subprocess.PIPE,
                stdout=slave,
                stderr=slave if host_write is None else host_write,
                close_fds=True,
                # Descriptors a kernel argument names, such as an external
                # vhost-user backend's end of a socket pair.
                pass_fds=tuple(pass_fds),
            )
        except BaseException:
            self._close_console()
            self._close_host()
            raise
        finally:
            if host_write is not None:
                os.close(host_write)
        assert self._process.stdin is not None
        self._input = self._process.stdin
        try:
            self._exit_queue = exit_queue(self._process.pid)
            self._lifeline, self._watchdog = start_lifeline(self._process.pid)
        except BaseException:
            self._process.kill()
            self._process.wait()
            self._input.close()
            self._close_console()
            self._close_host()
            raise
        os.set_blocking(self._input.fileno(), False)

    def __enter__(self) -> ConsoleProcess:
        return self

    def __exit__(self, exception_type, exception, traceback) -> None:
        """Print the transcript's end when an exception propagates, then close."""
        try:
            if exception_type is not None:
                print(
                    self.output()[-FAILURE_TRANSCRIPT_CHARACTERS:],
                    file=sys.stderr,
                )
                if self._separate_host_output:
                    print(
                        self.host_output()[-FAILURE_TRANSCRIPT_CHARACTERS:],
                        file=sys.stderr,
                    )
        finally:
            self.close()

    @property
    def process_id(self) -> int:
        return self._process.pid

    @property
    def return_code(self) -> int | None:
        """Return the emulator status once it exits, otherwise ``None``."""
        return self._process.poll()

    def terminate(self) -> None:
        """Force a guest worker to leave a blocking console operation."""
        self._signal(signal.SIGKILL)

    def _signal(self, guest_signal: int) -> None:
        """Signal the emulator, which shares this harness's process group.

        The emulator is started attached rather than in a session of its own,
        and the kernel keeps it there, so whatever kills the harness's group
        takes the guest with it. A harness that dies alone, killed by its
        process ID or leaving through os._exit(), closes its lifeline without
        standing the watchdog down, and the watchdog kills the guest. That is
        the property worth having: a harness killed outright runs no cleanup
        code, and a guest that survives one boots to idle and still burns one
        to two host cores indefinitely, quietly oversubscribing every later
        benchmark on the same host.
        """
        if self._process.poll() is None:
            try:
                self._process.send_signal(guest_signal)
            except ProcessLookupError:
                pass

    def run_command(
        self,
        command: str,
        timeout_seconds: float = 30,
        *,
        preamble: str = "",
        single_write: bool = False,
        stdin: str = "",
    ) -> CommandResult:
        """Run a shell command through the serial console and capture it.

        ``preamble`` is sent ahead of the command as part of the same input;
        its output is not captured. ``stdin`` follows the command line, so the
        command reads it from the console. ``single_write`` delivers all of
        that input in one host write, so the guest receives it as one burst
        rather than as the pipe accepts it. A command still running after
        ``timeout_seconds`` raises CommandTimeout with what it printed.
        """
        token = uuid.uuid4().hex
        begin = f"__UML_COMMAND_BEGIN_{token}__"
        end = f"__UML_COMMAND_END_{token}__"
        wrapped = (
            f"{preamble}printf '\\n{begin}\\n'; {command}; rc=$?; "
            f"printf '\\n{end}:%s\\n' \"$rc\"\n"
        )
        start = len(self._transcript)
        if single_write:
            self._write_once(wrapped + stdin)
        else:
            self._write(wrapped + stdin)

        begin_pattern = re.compile(console_text(begin), re.IGNORECASE)

        def output(section: bytes) -> str:
            begin_end = None
            for found in begin_pattern.finditer(section):
                begin_end = found.end()
            if begin_end is not None:
                section = section[begin_end:]
            return self._normal_output(section)

        try:
            match = self._read_until(marker_pattern(end), timeout_seconds, start)
        except TimeoutError as error:
            raise CommandTimeout(
                str(error), output(bytes(self._transcript[start:]))
            ) from error
        return CommandResult(
            marker_status(match),
            output(bytes(self._transcript[start : match.start()])),
        )

    def check_command(
        self,
        command: str,
        timeout_seconds: float = 30,
        *,
        expected: str | None = None,
    ) -> str:
        """Run a command, raise unless it exits zero, and return its output.

        With ``expected``, the stripped output must also equal it."""
        result = self.run_command(command, timeout_seconds)
        if result.return_code != 0:
            raise RuntimeError(
                f"guest command failed ({result.return_code}): {command}\n"
                f"{result.output}"
            )
        if expected is not None and result.output.strip() != expected:
            raise RuntimeError(
                f"guest command {command!r} returned {result.output!r}, "
                f"expected {expected!r}"
            )
        return result.output

    def wait_for_exit(self, timeout_seconds: float) -> bool:
        """Keep reading the console until the emulator exits or time runs out."""
        deadline = time.monotonic() + timeout_seconds
        while self._process.poll() is None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            readable, _, _ = select.select(
                self._readable(), [], [], min(remaining, 0.25)
            )
            if readable:
                self._drain()
        self._drain()
        return True

    def close(self) -> None:
        """Stop the emulator if it still runs, release the console and remove
        what an emulator killed outright left."""
        try:
            if self._process.poll() is None:
                self._signal(signal.SIGTERM)
                if not self.wait_for_exit(self.STOP_GRACE_SECONDS):
                    self._signal(signal.SIGKILL)
                    self._process.wait(timeout=self.KILL_WAIT_SECONDS)
        except KeyboardInterrupt:
            if self._process.poll() is None:
                self._signal(signal.SIGKILL)
            try:
                self._process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
            raise
        finally:
            try:
                self._input.close()
            except OSError:
                pass
            finally:
                self._close_console()
                self._close_host()
                self._release_lifeline()
                if self._process.poll() is not None:
                    remove_emulator_run_directories(self._process.pid)

    def _release_lifeline(self) -> None:
        """Stand the watchdog down if the emulator has exited, close the
        lifeline and reap the watchdog. An emulator still running, because
        stopping it failed, is left to the watchdog to kill."""
        lifeline: int | None = self._lifeline
        self._lifeline = None
        if lifeline is None:
            return
        try:
            if self._process.poll() is not None:
                os.write(lifeline, LIFELINE_STAND_DOWN)
        except OSError:
            pass  # the watchdog saw the exit first and has gone
        finally:
            os.close(lifeline)
        try:
            self._watchdog.wait(timeout=self.WATCHDOG_WAIT_SECONDS)
        except subprocess.TimeoutExpired:
            self._watchdog.kill()
            self._watchdog.wait()

    def output(self) -> str:
        return self._normal_output(bytes(self._transcript))

    def host_output(self) -> str:
        """The emulator's own diagnostics, with ``separate_host_output``."""
        return self._normal_output(bytes(self._host_transcript))

    def _readable(self) -> list[int]:
        """The console, the host output while it lasts, and the queue that
        turns readable when the emulator exits."""
        descriptors = [self._master]
        if self._host is not None:
            descriptors.append(self._host)
        if self._exit_queue is not None:
            descriptors.append(self._exit_queue.fileno())
        return descriptors

    def _exited_without_output(self) -> None:
        """Explain an emulator that exited before the awaited output.

        Raise a more specific error than the generic exit status; the default
        adds nothing."""

    def _write(self, text: str) -> None:
        payload = memoryview(text.encode())
        input_descriptor = self._input.fileno()
        offset = 0
        deadline = time.monotonic() + CONSOLE_WRITE_TIMEOUT_SECONDS
        while offset < len(payload):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("guest console write exceeded 30s")
            try:
                written = os.write(input_descriptor, payload[offset:])
            except InterruptedError:
                continue
            except BlockingIOError:
                written = 0
            if written > 0:
                offset += written
                continue
            if self._process.poll() is not None:
                raise BrokenPipeError("guest exited during console write")
            readable, writable, _ = select.select(
                self._readable(), [input_descriptor], [], min(remaining, 0.25)
            )
            if readable:
                self._drain()
            if not writable:
                continue

    def _write_once(self, text: str) -> None:
        payload = text.encode()
        try:
            written = os.write(self._input.fileno(), payload)
        except BlockingIOError:
            written = 0
        if written != len(payload):
            raise RuntimeError(
                f"guest console accepted {written} of {len(payload)} bytes "
                "in one write"
            )

    def _read_until(
        self,
        pattern: re.Pattern[bytes],
        timeout_seconds: float,
        search_from: int = 0,
    ) -> re.Match[bytes]:
        deadline = time.monotonic() + timeout_seconds
        while True:
            match = pattern.search(self._transcript, search_from)
            if match is not None:
                return match
            if self._process.poll() is not None:
                self._drain()
                match = pattern.search(self._transcript, search_from)
                if match is not None:
                    return match
                self._exited_without_output()
                raise RuntimeError(
                    f"guest exited with status {self._process.returncode}"
                )
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    f"guest operation exceeded {timeout_seconds:g}s"
                )
            readable, _, _ = select.select(
                self._readable(), [], [], min(remaining, 0.25)
            )
            if readable:
                self._drain()

    def _drain(self) -> None:
        self._drain_host()
        while True:
            try:
                chunk = os.read(self._master, 65536)
            except (BlockingIOError, OSError):
                return
            if not chunk:
                return
            if self.first_output_at is None:
                self.first_output_at = time.monotonic()
            self._transcript.extend(chunk)
            if self._console_sink is not None:
                self._console_sink.write(chunk)
                self._console_sink.flush()

    def _drain_host(self) -> None:
        while self._host is not None:
            try:
                chunk = os.read(self._host, 65536)
            except BlockingIOError:
                return
            except OSError:
                chunk = b""
            if not chunk:
                # A pipe at its end stays readable, so it leaves the select.
                self._close_host()
                return
            self._host_transcript.extend(chunk)

    def _close_console(self) -> None:
        """Close the PTY and the exit queue. The master goes first, because a
        terminal end closed last with output unread while its master is open
        waits 0.6 s for a reader."""
        for descriptor in (self._master, self._terminal):
            if descriptor is not None:
                try:
                    os.close(descriptor)
                except OSError:
                    pass
        self._terminal = None
        if self._exit_queue is not None:
            self._exit_queue.close()
            self._exit_queue = None

    def _close_host(self) -> None:
        if self._host is not None:
            try:
                os.close(self._host)
            except OSError:
                pass
            self._host = None

    @staticmethod
    def _normal_output(output: bytes) -> str:
        decoded = output.decode("utf-8", errors="replace").replace("\r", "")
        return ANSI_ESCAPE.sub("", decoded).strip()


class Guest(ConsoleProcess):
    """A harness shell on the unified image, booted through the ramdisk.

    The launcher's per-run initramfs mounts the image beneath a tmpfs
    overlay, installs ``payloads`` and the session, and the session starts a
    root shell on the console. ``overlay_shell`` gives that shell what an
    init would: runtime mounts, the SLiRP address and resolver, and the
    ``workspace`` under /tmp. ``init_shell`` gives a bare shell on the same
    overlay. Swap and cgroup hierarchies are ready before the prompt.

    The root is an EROFS image on /dev/ubda; ``root_type="ext4"`` makes it
    an ext4 filesystem instead, which the overlay still mounts read-only.
    ``network="image"`` leaves the guest without the SLiRP address and
    resolver, as an image without ``ip`` or a measurement that must not touch
    a network needs.
    """

    def __init__(
        self,
        emulator: pathlib.Path,
        rootfs: pathlib.Path,
        memory: str,
        workspace: str,
        kernel_arguments: Sequence[str] = (),
        swap_size: str | None = None,
        cgroup1: str | None = None,
        cgroup2_nsdelegate: bool = False,
        init_shell: bool = False,
        overlay_shell: bool = False,
        engine: str = "jit",
        core_class: str = "performance",
        pass_fds: Sequence[int] = (),
        console_sink: BinaryIO | None = None,
        payloads: Mapping[str, GuestFile] = {},
        root_type: str | None = None,
        network: str = "user-network",
        environment: Mapping[str, str] | None = None,
        separate_host_output: bool = False,
    ) -> None:
        if init_shell == overlay_shell:
            raise ValueError(
                "a guest boots exactly one of init_shell or overlay_shell"
            )
        self._swap_path: pathlib.Path | None = None
        self._staging: pathlib.Path | None = None
        self._core_class = core_class
        try:
            session = session_parameters(
                overlay_shell=overlay_shell,
                workspace=workspace,
                swap=swap_size is not None,
                cgroup1=cgroup1,
                cgroup2_nsdelegate=cgroup2_nsdelegate,
                network=network,
            )
            self._swap_path = create_swap_image(swap_size) if swap_size else None
            cleanup_stale_guest_files()
            self._staging = pathlib.Path(
                tempfile.mkdtemp(prefix=f"{STAGING_PREFIX}{os.getpid()}-")
            )
            guest_files = stage_guest_files(
                self._staging, session, payloads, root_type
            )
            super().__init__(
                emulator_arguments(
                    emulator,
                    rootfs,
                    memory,
                    (*kernel_arguments, "console=tty0", "console=ttyS0"),
                    self._swap_path,
                    engine=engine,
                    core_class=core_class,
                    guest_files=guest_files,
                ),
                environment=environment,
                pass_fds=pass_fds,
                console_sink=console_sink,
                separate_host_output=separate_host_output,
            )
        except BaseException:
            self._remove_host_files()
            raise

    def boot_to_shell(self, timeout_seconds: float = BOOT_TIMEOUT_SECONDS) -> None:
        """Reach the root shell the session starts."""
        ready = self._read_until(
            re.compile(
                console_text(f"{SESSION_READY_MARKER}/bin/sh")
                + _KERNEL_LINES
                + rb"\r?\n"
            ),
            timeout_seconds,
        )
        # The search starts on the marker's line feed: a pattern's ``^`` does
        # not match where a search starts.
        self._read_until(
            re.compile(ROOT_PROMPT), timeout_seconds, ready.end() - 1
        )
        # Every vCPU's carrier has come up by the time the session runs.
        require_host_fast_paths(self.output())
        require_host_fast_paths(self.host_output())

    def boot_and_login(self, timeout_seconds: float = BOOT_TIMEOUT_SECONDS) -> None:
        self.boot_to_shell(timeout_seconds)

    def restart_to_shell(
        self,
        timeout_seconds: float = BOOT_TIMEOUT_SECONDS,
        command: str = "reboot -f",
    ) -> None:
        """Restart the guest and reach the root shell of its next boot.

        The launcher boots a guest that restarts itself again in the same
        emulator process, so the console, the process and its ID carry on and
        only the guest is new."""
        start = len(self._transcript)
        self._write(f"sync; {command}\n")
        ready = self._read_until(
            re.compile(
                console_text(f"{SESSION_READY_MARKER}/bin/sh")
                + _KERNEL_LINES
                + rb"\r?\n"
            ),
            timeout_seconds,
            start,
        )
        self._read_until(
            re.compile(ROOT_PROMPT), timeout_seconds, ready.end() - 1
        )

    def shutdown(self, timeout_seconds: float = 30) -> bool:
        """Request an orderly halt and wait for the emulator to exit."""
        if self._process.poll() is not None:
            return True
        try:
            self._write("sync; poweroff -f\n")
            return self.wait_for_exit(timeout_seconds)
        except (BrokenPipeError, OSError):
            return self._process.poll() is not None

    def close(self) -> None:
        try:
            try:
                self.shutdown(5)
            finally:
                super().close()
        finally:
            self._remove_host_files()

    def _remove_host_files(self) -> None:
        if self._swap_path is not None:
            self._swap_path.unlink(missing_ok=True)
            self._swap_path = None
        if self._staging is not None:
            shutil.rmtree(self._staging, ignore_errors=True)
            self._staging = None
