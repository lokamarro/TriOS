#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a built Andarium application against the AppStore configuration.

The App Store configuration (Andarium/README.md#configurations) builds an
application that must:

  - declare no non-exempt encryption, require arm64 and Metal, target iPhone
    and iPad, and have no background mode;
  - carry no automation knob: no ANDARIUM_, KERNARIUM_, UML_ or DARWIN_UML_
    environment name in the host code of the application or of any embedded
    framework (the guest kernel image inside a runtime is not host code);
  - carry none of the harness's console lines (ConsoleReport in the
    application: the statistics, thermal state, runtime and focus lines),
    which the configuration compiles out;
  - carry no debugging or automation support in the host code of the
    application or of a framework: the frame mirror, console input, the
    guest sampler, the footprint log, the virtual Jetsam, the activity logs,
    the census and histogram, the automation entries and the harness's
    leftovers (DEBUG_SYMBOLS and DEBUG_STRINGS name them; the stand-ins that
    keep the runtime API's layout, runtimeSetFrameSink and
    runtimeSendConsoleInput, are allowed), and contain no test bundle and no
    XCTest or Testing framework;
  - carry no trace of the machine that built it: no path under a user's home
    directory (/Users/<name>/), on a mounted volume (/Volumes/<name>/) or in
    the per-user temporary directories (/var/folders/) in the host code of an
    executable, and no debug stabs in an embedded framework (the linker's
    debug map, whose N_OSO entries name the object files of the build host's
    checkout and dependency trees; dsymutil has read it into the dSYM by then,
    and strip -S removes it and keeps the UUID). The strings of those stabs
    are not searched for a path. Xcode strips the application's own
    executable when it archives, and strips nothing it embeds. A path that a
    dependency compiles in from its prefix is the dependencies' to avoid
    (dependencies/README.md, "No build-host paths"); and
  - pass scripts/check-privacy-manifests.py.

The knob scan reads bytes, so it does not see a Swift string of up to 15 bytes
(ANDARIUM_START is one): the compiler keeps those in instruction immediates.
The launch names stay out of the application because the configuration
compiles Live/LaunchEnvironment.swift out, and the scan covers the longer names.
The console lines are long enough to be text in the binary, so a build whose
code still holds one fails, whether or not anything calls it.

The Xcode project runs it as the last build phase of the App Store
configuration, so a build that breaks one of these rules fails.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import plistlib
import re
import struct
import subprocess
import sys
from pathlib import Path

KNOB = re.compile(rb"\b(?:ANDARIUM|KERNARIUM|DARWIN_UML|UML)_[A-Z][A-Z0-9_]{2,}\b")
# What ConsoleReport (Shared/ConsoleReport.swift) prints, as far as its text is
# the same in every line; the configuration compiles the type out.
HARNESS_LINES = re.compile(
    rb"Andarium stats|Andarium thermal state|Andarium: runtime returned|Andarium: application ")
# What a path names about the machine that built the code: the account whose
# home directory held the checkout, the volume that held the checkout or the
# dependency prefixes, and the build's temporary directories. A bare
# "/Volumes/" or a format such as "/Volumes/%s" is no path of a build and
# does not match.
HOST_PATHS = (
    ("a path under a home directory",
     re.compile(rb"/Users/[A-Za-z0-9._-]+/[\x21-\x7e]*")),
    ("a path on a mounted volume",
     re.compile(rb"/Volumes/[A-Za-z0-9._ -]+/[\x21-\x7e]*")),
    ("a path in a temporary build directory",
     re.compile(rb"/(?:private/)?var/folders/[\x21-\x7e]+")),
)
# Sections that hold the guest, not the host: the Linux kernel image a runtime
# carries, and the boot files of a Linux runtime.
GUEST_SECTIONS = {("__TEXT", "__uml_elf"), ("__TEXT", "__uml_boot_base"), ("__TEXT", "__uml_boot_init")}

MACHO_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
CPU_TYPE_ARM64 = 0x0100000C
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
N_STAB = 0xE0
N_OSO = 0x66
CHECKER = Path(__file__).resolve().parents[2] / "scripts" / "check-privacy-manifests.py"


def thin_offset(data: bytes) -> int | None:
    if len(data) < 8:
        return None
    if struct.unpack_from(">I", data, 0)[0] == FAT_MAGIC:
        count = struct.unpack_from(">I", data, 4)[0]
        for index in range(min(count, 16)):
            cpu, _sub, offset, _size, _align = struct.unpack_from(
                ">IIIII", data, 8 + 20 * index)
            if cpu == CPU_TYPE_ARM64:
                return offset
        return None
    return 0 if struct.unpack_from("<I", data, 0)[0] == MACHO_64 else None


def guest_ranges(data: bytes, base: int) -> list[tuple[int, int]]:
    """File ranges of the sections that hold the guest."""
    found = []
    count = struct.unpack_from("<I", data, base + 16)[0]
    position = base + 32
    for _ in range(count):
        command, size = struct.unpack_from("<II", data, position)
        if command == LC_SEGMENT_64:
            sections = struct.unpack_from("<I", data, position + 64)[0]
            entry = position + 72
            for _ in range(sections):
                name = data[entry:entry + 16].rstrip(b"\0").decode()
                segment = data[entry + 16:entry + 32].rstrip(b"\0").decode()
                length, offset = struct.unpack_from("<QI", data, entry + 40)
                if (segment, name) in GUEST_SECTIONS:
                    found.append((base + offset, base + offset + length))
                entry += 80
        position += size
    return found


def host_code(path: Path) -> tuple[bytes, int] | None:
    """The bytes of one Mach-O file with the guest's sections blanked, and the
    offset of its arm64 slice; None for a file that is not an arm64 Mach-O."""
    data = path.read_bytes()
    base = thin_offset(data[:4096])
    if base is None:
        return None
    buffer = bytearray(data)
    for start, end in guest_ranges(data, base):
        buffer[start:end] = bytes(end - start)
    return bytes(buffer), base


def knobs(path: Path) -> list[str]:
    """The automation knob names in the host code of one Mach-O file."""
    code = host_code(path)
    if code is None:
        return []
    return sorted({match.decode() for match in KNOB.findall(code[0])})


def harness_lines(path: Path) -> list[str]:
    """The harness's console lines in the host code of one Mach-O file."""
    code = host_code(path)
    if code is None:
        return []
    return sorted({match.decode().strip() for match in HARNESS_LINES.findall(code[0])})


def symbol_table(data: bytes, base: int) -> tuple[int, int, int, int] | None:
    """The symbol table of a Mach-O: the offset and count of its struct
    nlist_64 entries, and the offset and size of its string table."""
    count = struct.unpack_from("<I", data, base + 16)[0]
    position = base + 32
    for _ in range(count):
        command, size = struct.unpack_from("<II", data, position)
        if command == LC_SYMTAB:
            offset, symbols, strings, strings_size = struct.unpack_from(
                "<IIII", data, position + 8)
            return base + offset, symbols, base + strings, strings_size
        position += size
    return None


def debug_stabs(data: bytes, base: int) -> tuple[int, int]:
    """The debug stabs in a Mach-O's symbol table, and how many of them are
    N_OSO, each of which names an object file."""
    table = symbol_table(data, base)
    if table is None:
        return 0, 0
    offset, symbols, _strings, _size = table
    # struct nlist_64 is 16 bytes and n_type is its fifth.
    kinds = data[offset + 4:offset + 16 * symbols:16]
    return sum(1 for kind in kinds if kind & N_STAB), kinds.count(N_OSO)


def without_debug_map(data: bytes, base: int) -> bytes:
    """The Mach-O with the strings of its debug stabs blanked: the names of
    the objects, sources and functions the linker recorded. A framework that
    carries them fails for that, and Xcode strips them from the application's
    own executable when it archives, so neither is a path the build host left
    in the code."""
    table = symbol_table(data, base)
    if table is None:
        return data
    offset, symbols, strings, size = table
    buffer = bytearray(data)
    for entry in range(symbols):
        string, kind = struct.unpack_from("<IB", data, offset + 16 * entry)
        if kind & N_STAB and 0 < string < size:
            end = data.find(b"\0", strings + string, strings + size)
            if end > 0:
                buffer[strings + string:end] = bytes(end - strings - string)
    return bytes(buffer)


def build_host_traces(path: Path, debug_map: bool = True) -> list[str]:
    """What one Mach-O file keeps of the machine that built it. `debug_map`
    says whether its debug stabs count: they do for a framework, whose binary
    ships as linked. The strings of the stabs are never searched for a path:
    a framework with a debug map fails on the map, and the application's own
    executable loses it when Xcode archives."""
    code = host_code(path)
    if code is None:
        return []
    data, base = code
    found = []
    stabs, objects = debug_stabs(data, base) if debug_map else (0, 0)
    if stabs:
        found.append(
            f"carries the linker's debug map ({stabs} stabs, {objects} N_OSO "
            "object paths); strip -S it after dsymutil")
    data = without_debug_map(data, base)
    for label, pattern in HOST_PATHS:
        paths = sorted({match.decode() for match in pattern.findall(data)})
        if paths:
            found.append(
                f"names {label} ({len(paths)}): "
                f"{paths[0][:80]}{' ...' if len(paths) > 1 else ''}")
    return found


def info_problems(info: dict) -> list[str]:
    found = []
    if info.get("ITSAppUsesNonExemptEncryption") is not False:
        found.append("Info.plist: ITSAppUsesNonExemptEncryption must be false")
    capabilities = info.get("UIRequiredDeviceCapabilities")
    for capability in ("arm64", "metal"):
        if not isinstance(capabilities, list) or capability not in capabilities:
            found.append(f"Info.plist: UIRequiredDeviceCapabilities lacks {capability}")
    if info.get("UIDeviceFamily") != [1, 2]:
        found.append("Info.plist: UIDeviceFamily must be [1, 2], iPhone and iPad")
    if "UIBackgroundModes" in info:
        found.append("Info.plist: the application has no background mode")
    return found


def executables_of(bundle: Path) -> list[Path]:
    """The executable of one bundle, if its Info.plist names one."""
    try:
        with (bundle / "Info.plist").open("rb") as handle:
            name = plistlib.load(handle).get("CFBundleExecutable")
    except (OSError, plistlib.InvalidFileException, ValueError):
        return []
    return [bundle / name] if isinstance(name, str) and (bundle / name).is_file() else []


def executables(app: Path) -> list[Path]:
    """The executables of the application and of its embedded frameworks."""
    return [
        binary
        for bundle in [app, *sorted((app / "Frameworks").glob("*.framework"))]
        for binary in executables_of(bundle)
    ]


# The debugging and automation support an App Store runtime and application
# leave out (Andarium/README.md#configurations). Each entry is a kind and the
# fragments that name its machinery: mangled symbol names (C++ and Swift write
# a name's length before it) and the text its log lines hold. The stand-ins that
# keep the runtime API table's layout (runtimeSetFrameSink and
# runtimeSendConsoleInput return -ENOTSUP) are not on the list. The scan
# reads the host code, so a Swift name of up to 15 bytes in an instruction is
# not seen; the symbol fragments are what catch the application's Swift.
DEBUG_SYMBOLS = (
    ("frame mirror", (b"16FrameTapRegistry", b"11setFrameTap", b"14renderTapFrame",
                      b"7tapLoop", b"kernarium_runtime_loader_set_frame_sink")),
    ("console input", (b"13InputRegistry", b"7HostEnd4send",
                       b"kernarium_runtime_loader_send_console_input")),
    ("guest sampler", (b"11diagnostics12_GLOBAL__N_17Sampler",)),
    ("footprint log", (b"17FootprintReporter",)),
    ("virtual Jetsam", (b"13VirtualJetsam7observe", b"format_virtual_jetsam_line",
                        b"sample_process_footprint")),
    ("activity log", (b"18DisplayActivityLog", b"5video11ActivityLog")),
    ("census and histogram", (b"6census", b"execution_histogram_note",
                              b"execution_histogram_forget")),
    ("automation", (b"kernarium_runtime_automation_",
                    b"kernarium_runtime_loader_automation_", b"7harness",
                    b"14AutomationBoot")),
    ("test tooling", (b"5vsock9Forwarder", b"17UnixSocketChannel", b"_rfbGetScreen",
                      b"_rfbInitServer")),
    ("launch harness", (b"13ConsoleReport", b"15environmentName", b"13applyDeferred",
                        b"16printsStatistics", b"13launchApplied")),
)
DEBUG_STRINGS = (
    ("frame mirror", (b"uml-frame-tap",)),
    ("footprint log", (b"Kernarium: footprint", b"Kernarium: memory %s")),
    ("virtual Jetsam", (b"virtual Jetsam", b"uml-virtual-jetsam")),
    ("activity log", (b"uml-display %.3f", b"uml-video %.3f")),
    ("guest sampler", (b"# guest code samples",)),
    ("automation", (b"kernarium_runtime_automation_", b"automation boot with",
                    b"ADB authorization is off", b"removed the boot payload")),
    ("remote framebuffer", (b"RFB 003.00",)),
)
# What runs tests: a test bundle, and the frameworks and libraries that load it
# (XCTest, XCUIAutomation and the rest, Swift Testing). They are in an
# application that `xcodebuild test` hosted.
TEST_SUPPORT = re.compile(
    r"^(?:.*\.xctest|XC[A-Za-z]*\.framework|libXC[A-Za-z]*\.dylib"
    r"|_?Testing(?:_[A-Za-z0-9]+)?\.framework|lib_?Testing[A-Za-z0-9_]*\.dylib)$")


def debug_support(path: Path) -> list[str]:
    """The debugging and automation support in the host code of one Mach-O
    file, as `kind (fragment, ...)` for each kind found."""
    code = host_code(path)
    if code is None:
        return []
    found = []
    for table in (DEBUG_SYMBOLS, DEBUG_STRINGS):
        for kind, fragments in table:
            names = sorted(
                fragment.decode() for fragment in fragments if fragment in code[0])
            if names:
                found.append(f"{kind} ({', '.join(names)})")
    return found


def test_support(app: Path) -> list[str]:
    """The test bundles and test frameworks anywhere in the application."""
    found = []
    for directory, directories, files in os.walk(app):
        for name in list(directories):
            if TEST_SUPPORT.match(name):
                found.append(str((Path(directory) / name).relative_to(app)))
                directories.remove(name)
        found.extend(
            str((Path(directory) / name).relative_to(app))
            for name in files if TEST_SUPPORT.match(name))
    return sorted(found)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("app", type=Path)
    parser.add_argument("--privacy-checker", type=Path, default=CHECKER)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--runtime", action="store_true",
        help="the path is one runtime framework: check its automation knobs, "
             "its debugging support and the traces of its build host")
    mode.add_argument(
        "--host-traces", action="store_true",
        help="the path is one framework: check the traces of its build host alone")
    options = parser.parse_args()

    app = options.app
    if options.runtime or options.host_traces:
        found = []
        for binary in executables_of(app):
            if options.runtime:
                found.extend(f"{binary.name}: carries automation knob {name}" for name in knobs(binary))
                found.extend(f"{binary.name}: carries debugging support: {item}"
                             for item in debug_support(binary))
            found.extend(f"{binary.name}: {trace}" for trace in build_host_traces(binary))
        for line in found:
            print(line, file=sys.stderr)
        return 1 if found else 0
    problems: list[str] = []
    try:
        with (app / "Info.plist").open("rb") as handle:
            info = plistlib.load(handle)
    except (OSError, plistlib.InvalidFileException, ValueError):
        print(f"{app}: has no readable Info.plist", file=sys.stderr)
        return 1
    problems.extend(f"{app.name}: {problem}" for problem in info_problems(info))

    for binary in executables(app):
        names = knobs(binary)
        if names:
            problems.append(
                f"{binary.relative_to(app)}: carries automation knob(s) "
                f"{', '.join(names[:6])}{' ...' if len(names) > 6 else ''}")
        lines = harness_lines(binary)
        if lines:
            problems.append(
                f"{binary.relative_to(app)}: carries the harness's console line(s) "
                f"{', '.join(repr(line) for line in lines)}")
        problems.extend(
            f"{binary.relative_to(app)}: carries debugging support: {item}"
            for item in debug_support(binary))
        # The application's own executable is stripped when Xcode archives it.
        problems.extend(
            f"{binary.relative_to(app)}: {trace}"
            for trace in build_host_traces(binary, debug_map=binary.parent != app))

    problems.extend(
        f"{path}: a test bundle or test framework; build the application "
        "without its tests" for path in test_support(app))

    privacy = subprocess.run(
        [sys.executable, "-B", str(options.privacy_checker), str(app)],
        check=False, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, timeout=600)
    if privacy.returncode != 0:
        problems.extend(line for line in privacy.stderr.splitlines() if line)

    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print(f"{app.name}: ready for the App Store")
    return 0


if __name__ == "__main__":
    sys.exit(main())
