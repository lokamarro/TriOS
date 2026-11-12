#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check what the App Store bundle check refuses and what it accepts."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT_SECONDS = 120

CLEAN = "int fixture(int value) { return value + 1; }\n"
KNOB = (
    "#include <stdlib.h>\n"
    'const char *fixture(void) { return getenv("KERNARIUM_HOST_FORWARD"); }\n'
)
LAUNCH_KNOB = (
    "#include <stdlib.h>\n"
    'const char *fixture(void) { return getenv("ANDARIUM_START"); }\n'
)
# The harness's console lines (ConsoleReport), which the configuration
# compiles out: what the check reports for each, and a full line.
REPORT_LINES = (
    ("Andarium stats", "Andarium stats: cpu=1% fps=2 memory-used=3 memory=4 held=5 boot=6"),
    ("Andarium thermal state", "Andarium thermal state: phase=start state=nominal uptime=0.000"),
    ("Andarium: runtime returned", "Andarium: runtime returned 0"),
    ("Andarium: application", "Andarium: application active"),
)
HOME_PATH = (
    "const char *fixture(void) { return \"/Users/NAME/work/project/file.c\"; }\n"
)
# The debugging and automation support the App Store runtime and application
# leave out: the kind the check reports, and a library that holds a symbol or a
# string of it.
DEBUG_SUPPORT = (
    ("frame mirror", "int _ZN3uml3gpu11setFrameTapEPFvPvS1_ES1_(void) { return 1; }\n"),
    ("frame mirror", 'const char *fixture(void) { return "uml-frame-tap"; }\n'),
    ("frame mirror", "int kernarium_runtime_loader_set_frame_sink(void) { return 1; }\n"),
    ("console input", "int _ZN10kernarium7console13InputRegistry4sendEPKvm(void) { return 1; }\n"),
    ("console input", "int kernarium_runtime_loader_send_console_input(void) { return 1; }\n"),
    ("guest sampler", "int _ZN3uml3cpu11diagnostics12_GLOBAL__N_17Sampler3addEv(void) { return 1; }\n"),
    ("guest sampler", 'const char *fixture(void) { return "# guest code samples"; }\n'),
    ("footprint log", "int _ZN17FootprintReporterD1Ev(void) { return 1; }\n"),
    ("footprint log", 'const char *fixture(void) { return "Kernarium: footprint peak"; }\n'),
    ("virtual Jetsam", "int _ZN3uml7runtime13VirtualJetsam7observeEyy(void) { return 1; }\n"),
    ("virtual Jetsam", 'const char *fixture(void) { return "uml-virtual-jetsam"; }\n'),
    ("activity log", "int _ZN3uml3gpu18DisplayActivityLog6recordEPKcz(void) { return 1; }\n"),
    ("activity log", "int _ZN3uml5video11ActivityLog6openedEjPKcS3_(void) { return 1; }\n"),
    ("activity log", 'const char *fixture(void) { return "uml-video %.3f stream=%u"; }\n'),
    ("census and histogram", "int _ZN3uml3cpu6census4noteEv(void) { return 1; }\n"),
    ("automation", "int kernarium_runtime_automation_job_at_start(void) { return 1; }\n"),
    ("automation", 'const char *fixture(void) { return "removed the boot payload"; }\n'),
    ("test tooling", "int _rfbGetScreen(void) { return 1; }\n"),
    ("launch harness", "int _s8Andarium8AppEntryV13applyDeferred2toyyF(void) { return 1; }\n"),
)
# What an App Store runtime keeps so that the runtime API table's layout and
# version stay: its entries answer -ENOTSUP, and a virtual Jetsam that never
# starts. They are not the machinery.
STAND_INS = (
    "int runtimeSetFrameSink(void) { return -45; }\n"
    "int runtimeSendConsoleInput(void) { return -45; }\n"
    "int _ZN12_GLOBAL__N_118runtimeSetFrameSinkEPFvPvS1_Ejj(void) { return -45; }\n"
    "int _ZN12_GLOBAL__N_121runtimeSendConsoleInputEPKvm(void) { return -45; }\n"
    "int _ZN3uml7runtime24start_virtual_jetsam_watchEm(void) { return 0; }\n"
    "int _ZN3uml7runtime18VirtualJetsamWatchD1Ev(void) { return 0; }\n"
)
# What a dependency compiles in from the prefix it was built under: a locale
# directory on the volume that held the dependency prefixes, and a path in the
# per-user temporary directory that held a build.
VOLUME_PATH = (
    "const char *fixture(void) { return \"/Volumes/NAME/prefix/share/locale\"; }\n"
)
VOLUME_NAME_WITH_SPACE = (
    "const char *fixture(void) { return \"/Volumes/NAME 2/prefix/lib\"; }\n"
)
TEMPORARY_PATH = (
    "const char *fixture(void) {"
    ' return "/private" "/var/folders/NAME/T/source-build.abc123/stage"; }\n'
)
# Strings that are no path of a build: the volumes directory alone, a format
# for one, a different directory of a temporary tree, and the neutral
# directory the dependency builds compile in.
HARMLESS_PATHS = (
    "const char *const fixture[] = {\n"
    '  "/Volumes/", "/Volumes/%s", "/var/folders/", "/tmp/mesa_%s",\n'
    '  "/nonexistent/share/locale",\n'
    "};\n"
)
MANIFEST = {
    "NSPrivacyTracking": False,
    "NSPrivacyTrackingDomains": [],
    "NSPrivacyCollectedDataTypes": [],
    "NSPrivacyAccessedAPITypes": [],
}


def run(command: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        check=False,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=TIMEOUT_SECONDS,
    )


def build(root: Path, name: str, source: str, extra: list[str] | None = None) -> Path:
    source_path = root / f"{name}.c"
    source_path.write_text(source, encoding="utf-8")
    output = root / f"lib{name}.dylib"
    # A framework's install name is @rpath-relative; the default one is the
    # output path, which is the temporary directory of this test.
    built = run([
        "xcrun", "--sdk", "iphoneos", "clang", "-target", "arm64-apple-ios27.0",
        "-dynamiclib", str(source_path), "-o", str(output),
        f"-Wl,-install_name,@rpath/{output.name}", *(extra or []),
    ])
    if built.returncode != 0:
        raise SystemExit(f"could not build {name}:\n{built.stdout}")
    return output


def build_with_debug_map(root: Path, name: str, directory: str | None = None) -> Path:
    """A library linked from an object compiled with -g: the linker keeps a
    debug map whose N_OSO entry names the object file. The object sits in a
    directory that looks like a mounted volume, so that the map names a
    build-host path of the kind the check refuses elsewhere. `directory` is
    the compilation directory the debug map's N_SO entries name."""
    work = root / "Volumes" / "NAME"
    work.mkdir(parents=True, exist_ok=True)
    source_path = work / f"{name}.c"
    source_path.write_text(CLEAN, encoding="utf-8")
    objects = work / f"{name}.o"
    # A source named relative to the compilation directory is what puts that
    # directory in the debug map.
    compiled = run([
        "xcrun", "--sdk", "iphoneos", "clang", "-target", "arm64-apple-ios27.0",
        "-g", *([f"-fdebug-compilation-dir={directory}"] if directory else []),
        "-c", source_path.name if directory else str(source_path), "-o", str(objects),
    ], cwd=work)
    if compiled.returncode != 0:
        raise SystemExit(f"could not compile {name}:\n{compiled.stdout}")
    output = root / f"lib{name}.dylib"
    linked = run([
        "xcrun", "--sdk", "iphoneos", "clang", "-target", "arm64-apple-ios27.0",
        "-dynamiclib", str(objects), "-o", str(output),
        f"-Wl,-install_name,@rpath/{output.name}",
    ])
    if linked.returncode != 0:
        raise SystemExit(f"could not link {name}:\n{linked.stdout}")
    if b"/Volumes/NAME/" not in output.read_bytes():
        raise SystemExit(f"the debug map of {name} names no object under a volume")
    return output


def strip_debug_map(library: Path) -> Path:
    """The same library after `strip -S`, as the packaging leaves it."""
    stripped = library.with_name(f"{library.stem}-stripped.dylib")
    shutil.copyfile(library, stripped)
    done = run(["xcrun", "--sdk", "iphoneos", "strip", "-S", str(stripped)])
    if done.returncode != 0:
        raise SystemExit(f"could not strip {library.name}:\n{done.stdout}")
    return stripped


def app_info(**overrides: object) -> dict:
    keys: dict[str, object] = {
        "CFBundleIdentifier": "org.example.Fixture",
        "CFBundleExecutable": "Fixture",
        "CFBundleName": "Fixture",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "1.0",
        "CFBundleVersion": "1",
        "MinimumOSVersion": "27.0",
        "ITSAppUsesNonExemptEncryption": False,
        "UIRequiredDeviceCapabilities": ["arm64", "metal"],
        "UIDeviceFamily": [1, 2],
        "NSMicrophoneUsageDescription": "Records only while Microphone is on.",
    }
    keys.update(overrides)
    return {key: value for key, value in keys.items() if value is not None}


def make_app(
    root: Path, name: str, executable: Path, info: dict,
    framework: Path | None = None, manifest: bool = True,
) -> Path:
    app = root / name / "Fixture.app"
    app.mkdir(parents=True)
    shutil.copyfile(executable, app / "Fixture")
    (app / "Info.plist").write_bytes(plistlib.dumps(info))
    if manifest:
        (app / "PrivacyInfo.xcprivacy").write_bytes(plistlib.dumps(MANIFEST))
    if framework is not None:
        bundle = app / "Frameworks" / "Runtime.framework"
        bundle.mkdir(parents=True)
        shutil.copyfile(framework, bundle / "Runtime")
        (bundle / "Info.plist").write_bytes(plistlib.dumps({
            "CFBundleIdentifier": "org.example.Runtime",
            "CFBundleExecutable": "Runtime",
            "CFBundleName": "Runtime",
            "CFBundlePackageType": "FMWK",
            "CFBundleShortVersionString": "1.0",
            "CFBundleVersion": "1",
            "MinimumOSVersion": "27.0",
        }))
        (bundle / "PrivacyInfo.xcprivacy").write_bytes(plistlib.dumps(MANIFEST))
    return app


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checker", type=Path, required=True)
    options = parser.parse_args()

    failures: list[str] = []

    def check(app: Path, *, passes: bool, expect: str = "") -> None:
        result = run([sys.executable, "-B", str(options.checker), str(app)])
        label = app.parent.name
        if (result.returncode == 0) != passes:
            failures.append(f"{label}: expected {'a pass' if passes else 'a failure'}:\n{result.stdout}")
        elif expect and expect not in result.stdout:
            failures.append(f"{label}: output lacks {expect!r}:\n{result.stdout}")

    with tempfile.TemporaryDirectory(prefix="andarium-app-store-") as directory:
        root = Path(directory)
        clean = build(root, "clean", CLEAN)
        knob = build(root, "knob", KNOB)
        launch = build(root, "launch", LAUNCH_KNOB)
        reports = {
            index: build(root, f"report{index}", f'const char *fixture(void) {{ return "{line}"; }}\n')
            for index, (_name, line) in enumerate(REPORT_LINES)
        }
        # The guest kernel image rides in a section of its own; a knob name in
        # it belongs to the guest.
        guest = root / "guest.bin"
        guest.write_bytes(
            b"\0UML_ROOTFS\0KERNARIUM_GUEST_NOTE\0/Users/guest/built/here.c\0"
            b"/Volumes/NAME/built/here.c\0")
        carrier = build(
            root, "carrier", CLEAN,
            ["-Wl,-sectcreate,__TEXT,__uml_elf," + str(guest)])

        check(make_app(root, "ready", clean, app_info(), framework=clean),
              passes=True, expect="ready for the App Store")
        check(make_app(root, "guest-section", clean, app_info(), framework=carrier), passes=True)

        # Info.plist.
        check(make_app(root, "no-encryption-key", clean, app_info(ITSAppUsesNonExemptEncryption=None)),
              passes=False, expect="ITSAppUsesNonExemptEncryption")
        check(make_app(root, "uses-encryption", clean, app_info(ITSAppUsesNonExemptEncryption=True)),
              passes=False, expect="ITSAppUsesNonExemptEncryption")
        check(make_app(root, "no-metal", clean, app_info(UIRequiredDeviceCapabilities=["arm64"])),
              passes=False, expect="lacks metal")
        check(make_app(root, "no-arm64", clean, app_info(UIRequiredDeviceCapabilities=["metal"])),
              passes=False, expect="lacks arm64")
        check(make_app(root, "ipad-only", clean, app_info(UIDeviceFamily=[2])),
              passes=False, expect="UIDeviceFamily")
        check(make_app(root, "background", clean, app_info(UIBackgroundModes=["audio"])),
              passes=False, expect="background mode")

        # Automation knobs in the host code of the application or a framework.
        check(make_app(root, "app-knob", knob, app_info()),
              passes=False, expect="KERNARIUM_HOST_FORWARD")
        check(make_app(root, "launch-knob", launch, app_info()),
              passes=False, expect="ANDARIUM_START")
        knob_app = make_app(root, "framework-knob", clean, app_info(), framework=knob)
        check(knob_app, passes=False, expect="Runtime.framework/Runtime")

        # The harness's console lines, in the application's own code, whether
        # or not anything calls them.
        for index, (name, _line) in enumerate(REPORT_LINES):
            check(make_app(root, f"report-line-{index}", reports[index], app_info()),
                  passes=False, expect=f"'{name}'")

        # What the build host leaves in a binary: the linker's debug map, whose
        # N_OSO entries name the objects of the build, and a home-directory,
        # volume or temporary-directory path. The same library stripped with
        # `strip -S` passes.
        mapped = build_with_debug_map(root, "mapped")
        stripped = strip_debug_map(mapped)
        homed = build(root, "homed", HOME_PATH)
        # A checkout under a home directory: the N_SO entries of the debug map
        # name /Users/NAME/src.
        mapped_home = build_with_debug_map(root, "mapped-home", "/Users/NAME/src")
        volumed = build(root, "volumed", VOLUME_PATH)
        spaced = build(root, "spaced", VOLUME_NAME_WITH_SPACE)
        temporary = build(root, "temporary", TEMPORARY_PATH)
        harmless = build(root, "harmless", HARMLESS_PATHS)
        # Xcode strips the application's own executable when it archives, so
        # only an embedded framework's debug map is the check's concern, and
        # the object paths in the map of either are not searched for a path:
        # the map is reported once, as the map.
        check(make_app(root, "app-debug-map", mapped, app_info()), passes=True)
        check(make_app(root, "app-debug-map-home", mapped_home, app_info()),
              passes=True, expect="ready for the App Store")
        # A framework with such a map fails on the map, and the home path its
        # stabs name is not reported as a path.
        check(make_app(root, "framework-debug-map-home", clean, app_info(), framework=mapped_home),
              passes=False, expect="Runtime.framework/Runtime: carries the linker's debug map")
        home_result = run([sys.executable, "-B", str(options.checker),
                           str(root / "framework-debug-map-home" / "Fixture.app")])
        if "home directory" in home_result.stdout:
            failures.append(
                "a debug map's source directory was reported as a path:\n"
                + home_result.stdout)
        check(make_app(root, "framework-debug-map", clean, app_info(), framework=mapped),
              passes=False, expect="Runtime.framework/Runtime: carries the linker's debug map")
        mapped_result = run([sys.executable, "-B", str(options.checker),
                             str(root / "framework-debug-map" / "Fixture.app")])
        if "mounted volume" in mapped_result.stdout:
            failures.append(
                "a debug map's object paths were reported as paths:\n"
                + mapped_result.stdout)
        check(make_app(root, "stripped-debug-map", stripped, app_info(), framework=stripped),
              passes=True, expect="ready for the App Store")
        check(make_app(root, "app-home-path", homed, app_info()),
              passes=False, expect="/Users/NAME/work/project/file.c")
        check(make_app(root, "framework-home-path", clean, app_info(), framework=homed),
              passes=False, expect="names a path under a home directory")
        # A volume path is as much the build host's as a home directory: the
        # dependency prefixes live on the volume that built them.
        check(make_app(root, "app-volume-path", volumed, app_info()),
              passes=False, expect="/Volumes/NAME/prefix/share/locale")
        check(make_app(root, "framework-volume-path", clean, app_info(), framework=volumed),
              passes=False, expect="Runtime.framework/Runtime: names a path on a mounted volume")
        check(make_app(root, "framework-volume-space", clean, app_info(), framework=spaced),
              passes=False, expect="/Volumes/NAME 2/prefix/lib")
        check(make_app(root, "app-temporary-path", temporary, app_info()),
              passes=False, expect="names a path in a temporary build directory")
        check(make_app(root, "framework-temporary-path", clean, app_info(), framework=temporary),
              passes=False, expect="/var/folders/NAME/T/source-build.abc123/stage")
        check(make_app(root, "harmless-paths", harmless, app_info(), framework=harmless),
              passes=True, expect="ready for the App Store")

        # A runtime framework alone (what package-ios-frameworks.sh --app-store
        # checks): its host code, and not the guest kernel image.
        for name, passes, expect in (
            ("framework-knob", False, "KERNARIUM_HOST_FORWARD"),
            ("framework-debug-map", False, "debug map"),
            ("framework-home-path", False, "home directory"),
            ("framework-volume-path", False, "mounted volume"),
            ("framework-temporary-path", False, "temporary build directory"),
            ("harmless-paths", True, ""),
            ("stripped-debug-map", True, ""),
            ("guest-section", True, ""),
            ("ready", True, ""),
        ):
            framework = root / name / "Fixture.app" / "Frameworks" / "Runtime.framework"
            result = run([sys.executable, "-B", str(options.checker), "--runtime", str(framework)])
            if (result.returncode == 0) != passes or expect not in result.stdout:
                failures.append(f"--runtime {name}: unexpected result ({result.returncode}):\n{result.stdout}")

        # The GPU frameworks are checked for the build host's traces alone: a
        # knob-like name is no concern of theirs.
        for name, passes, expect in (
            ("framework-knob", True, ""),
            ("framework-debug-map", False, "debug map"),
            ("framework-home-path", False, "home directory"),
            ("framework-volume-path", False, "mounted volume"),
            ("framework-temporary-path", False, "temporary build directory"),
            ("harmless-paths", True, ""),
            ("stripped-debug-map", True, ""),
        ):
            framework = root / name / "Fixture.app" / "Frameworks" / "Runtime.framework"
            result = run([sys.executable, "-B", str(options.checker), "--host-traces", str(framework)])
            if (result.returncode == 0) != passes or expect not in result.stdout:
                failures.append(f"--host-traces {name}: unexpected result ({result.returncode}):\n{result.stdout}")

        # Debugging and automation support: in the application's executable, in
        # an embedded framework and in a runtime checked alone, whichever way
        # the library holds it. The stand-ins of the runtime API pass.
        for index, (kind, source) in enumerate(DEBUG_SUPPORT):
            library = build(root, f"debug{index}", source)
            expect = f"carries debugging support: {kind}"
            check(make_app(root, f"debug-app-{index}", library, app_info()),
                  passes=False, expect=expect)
            debug_app = make_app(root, f"debug-framework-{index}", clean, app_info(), framework=library)
            check(debug_app, passes=False, expect=expect)
            runtime_only = run([
                sys.executable, "-B", str(options.checker), "--runtime",
                str(debug_app / "Frameworks" / "Runtime.framework")])
            if runtime_only.returncode == 0 or expect not in runtime_only.stdout:
                failures.append(f"--runtime debug {index} ({kind}): unexpected result "
                                f"({runtime_only.returncode}):\n{runtime_only.stdout}")
        stand_ins = build(root, "stand-ins", STAND_INS)
        check(make_app(root, "stand-ins-app", stand_ins, app_info(), framework=stand_ins),
              passes=True, expect="ready for the App Store")
        stand_in_runtime = run([
            sys.executable, "-B", str(options.checker), "--runtime",
            str(root / "stand-ins-app" / "Fixture.app" / "Frameworks" / "Runtime.framework")])
        if stand_in_runtime.returncode != 0:
            failures.append(f"--runtime stand-ins: unexpected result:\n{stand_in_runtime.stdout}")

        # Test bundles and the frameworks that run them are not in an
        # application that goes to the store.
        for name, relative, is_directory in (
            ("test-bundle", "PlugIns/FixtureTests.xctest", True),
            ("xctest-framework", "Frameworks/XCTest.framework", True),
            ("ui-automation-framework", "Frameworks/XCUIAutomation.framework", True),
            ("testing-framework", "Frameworks/Testing.framework", True),
            ("testing-foundation-framework", "Frameworks/_Testing_Foundation.framework", True),
            ("xctest-library", "Frameworks/libXCTestSwiftSupport.dylib", False),
        ):
            tested = make_app(root, name, clean, app_info(), framework=clean)
            member = tested / relative
            if is_directory:
                member.mkdir(parents=True)
                (member / "Info.plist").write_bytes(plistlib.dumps({"CFBundleIdentifier": "org.example.Test"}))
            else:
                shutil.copyfile(clean, member)
            check(tested, passes=False, expect=f"{relative}: a test bundle or test framework")

        # The privacy manifests are checked too.
        check(make_app(root, "no-manifest", clean, app_info(), manifest=False),
              passes=False, expect="PrivacyInfo.xcprivacy")

        missing = run([sys.executable, "-B", str(options.checker), str(root / "absent.app")])
        if missing.returncode == 0:
            failures.append("a missing application passed")

    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
