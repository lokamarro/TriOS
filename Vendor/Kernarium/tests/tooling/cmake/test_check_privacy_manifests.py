#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that the privacy manifest check finds each problem it lists.

Each case builds a small iOS framework with a real binary, so the check reads
the symbols and the Objective-C selectors a build produces.
"""

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
STAT = (
    "#include <sys/stat.h>\n"
    "int fixture(const char *path) { struct stat s; return stat(path, &s); }\n"
)
STATVFS = (
    "#include <sys/statvfs.h>\n"
    "int fixture(const char *path) { struct statvfs s; return statvfs(path, &s); }\n"
)
UPTIME_SELECTOR = (
    "#import <Foundation/Foundation.h>\n"
    "double fixture(void) { return [[NSProcessInfo processInfo] systemUptime]; }\n"
)
DEFAULTS_CLASS = (
    "#import <Foundation/Foundation.h>\n"
    "id fixture(void) { return [NSUserDefaults standardUserDefaults]; }\n"
)


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=False,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=TIMEOUT_SECONDS,
    )


def compile_library(root: Path, name: str, source: str, output: Path) -> None:
    extension = "m" if "#import" in source else "c"
    source_path = root / f"{name}.{extension}"
    source_path.write_text(source, encoding="utf-8")
    built = run([
        "xcrun", "--sdk", "iphoneos", "clang", "-target", "arm64-apple-ios27.0",
        "-dynamiclib", "-fobjc-arc", "-framework", "Foundation",
        str(source_path), "-o", str(output),
    ])
    if built.returncode != 0:
        raise SystemExit(f"could not build {name}:\n{built.stdout}")


def manifest(*apis: tuple[str, list[str]], tracking: bool = False) -> dict:
    return {
        "NSPrivacyTracking": tracking,
        "NSPrivacyTrackingDomains": [],
        "NSPrivacyCollectedDataTypes": [],
        "NSPrivacyAccessedAPITypes": [
            {
                "NSPrivacyAccessedAPIType": f"NSPrivacyAccessedAPICategory{name}",
                "NSPrivacyAccessedAPITypeReasons": reasons,
            }
            for name, reasons in apis
        ],
    }


def info(**overrides: object) -> dict:
    keys: dict[str, object] = {
        "CFBundleIdentifier": "org.example.Fixture",
        "CFBundleExecutable": "Fixture",
        "CFBundleName": "Fixture",
        "CFBundlePackageType": "FMWK",
        "CFBundleShortVersionString": "1.2",
        "CFBundleVersion": "3",
        "MinimumOSVersion": "27.0",
    }
    keys.update(overrides)
    return {key: value for key, value in keys.items() if value is not None}


def make_framework(
    root: Path, name: str, library: Path, plist: dict, privacy: dict | None
) -> Path:
    framework = root / name / "Fixture.framework"
    framework.mkdir(parents=True)
    shutil.copyfile(library, framework / "Fixture")
    (framework / "Info.plist").write_bytes(plistlib.dumps(plist))
    if privacy is not None:
        (framework / "PrivacyInfo.xcprivacy").write_bytes(plistlib.dumps(privacy))
    return framework


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checker", type=Path, required=True)
    options = parser.parse_args()

    failures: list[str] = []

    def check(path: Path, *, passes: bool, expect: str = "") -> None:
        result = run([sys.executable, "-B", str(options.checker), str(path)])
        label = f"{path.parent.name}/{path.name}"
        if (result.returncode == 0) != passes:
            failures.append(f"{label}: expected {'a pass' if passes else 'a failure'}:\n{result.stdout}")
        elif expect and expect not in result.stdout:
            failures.append(f"{label}: output lacks {expect!r}:\n{result.stdout}")

    with tempfile.TemporaryDirectory(prefix="uml-privacy-manifests-") as directory:
        root = Path(directory)
        libraries = {}
        for name, source in (
            ("clean", CLEAN), ("stat", STAT), ("statvfs", STATVFS),
            ("uptime", UPTIME_SELECTOR), ("defaults", DEFAULTS_CLASS),
        ):
            libraries[name] = root / f"lib{name}.dylib"
            compile_library(root, name, source, libraries[name])

        file_time = ("FileTimestamp", ["C617.1"])
        disk = ("DiskSpace", ["E174.1", "85F4.1"])
        boot = ("SystemBootTime", ["35F9.1"])
        defaults = ("UserDefaults", ["CA92.1"])

        # A manifest that declares each category the binary uses passes, and
        # so does one that declares more than the binary uses.
        check(make_framework(root, "stat-ok", libraries["stat"], info(), manifest(file_time)),
              passes=True)
        check(make_framework(root, "statvfs-ok", libraries["statvfs"], info(), manifest(disk)),
              passes=True)
        check(make_framework(root, "defaults-ok", libraries["defaults"], info(), manifest(defaults)),
              passes=True)
        check(make_framework(root, "clean-ok", libraries["clean"], info(), manifest()), passes=True)
        check(make_framework(root, "clean-extra", libraries["clean"], info(), manifest(disk)),
              passes=True)

        # A used API with no declaration, a wrong category or no manifest.
        check(make_framework(root, "stat-undeclared", libraries["stat"], info(), manifest()),
              passes=False, expect="symbol stat")
        check(make_framework(root, "stat-wrong-category", libraries["stat"], info(), manifest(disk)),
              passes=False, expect="symbol stat")
        check(make_framework(root, "statvfs-undeclared", libraries["statvfs"], info(), manifest(file_time)),
              passes=False, expect="symbol statvfs")
        check(make_framework(root, "defaults-undeclared", libraries["defaults"], info(), manifest()),
              passes=False, expect="NSUserDefaults")
        check(make_framework(root, "no-manifest", libraries["clean"], info(), None),
              passes=False, expect="PrivacyInfo.xcprivacy")

        # systemUptime is an Objective-C selector: no symbol names it.
        check(make_framework(root, "uptime-undeclared", libraries["uptime"], info(), manifest()),
              passes=False, expect="selector systemUptime")
        check(make_framework(root, "uptime-declared", libraries["uptime"], info(), manifest(boot)),
              passes=True)

        # The manifest itself: Apple's reason codes, no tracking, no data.
        check(make_framework(root, "bad-reason", libraries["stat"], info(),
                             manifest(("FileTimestamp", ["35F9.1"]))),
              passes=False, expect="no reason code")
        check(make_framework(root, "no-reason", libraries["stat"], info(),
                             manifest(("FileTimestamp", []))),
              passes=False, expect="declares no reason")
        check(make_framework(root, "unknown-category", libraries["clean"], info(),
                             manifest(("Calendar", ["C617.1"]))),
              passes=False, expect="unknown API category")
        check(make_framework(root, "tracking", libraries["clean"], info(), manifest(tracking=True)),
              passes=False, expect="NSPrivacyTracking")
        collects = manifest()
        collects["NSPrivacyCollectedDataTypes"] = [{"NSPrivacyCollectedDataType": "x"}]
        check(make_framework(root, "collects", libraries["clean"], info(), collects),
              passes=False, expect="NSPrivacyCollectedDataTypes")

        # The Info.plist of an embedded bundle: names, versions, minimum OS.
        check(make_framework(root, "empty-version", libraries["clean"],
                             info(CFBundleShortVersionString=""), manifest()),
              passes=False, expect="CFBundleShortVersionString")
        check(make_framework(root, "empty-build", libraries["clean"],
                             info(CFBundleVersion=""), manifest()),
              passes=False, expect="CFBundleVersion")
        check(make_framework(root, "empty-name", libraries["clean"],
                             info(CFBundleName=""), manifest()),
              passes=False, expect="CFBundleName")
        check(make_framework(root, "no-minimum", libraries["clean"],
                             info(MinimumOSVersion=None), manifest()),
              passes=False, expect="MinimumOSVersion")

        # An XCFramework and a directory of bundles are searched through.
        slice_root = root / "Fixture.xcframework" / "ios-arm64"
        shutil.copytree(root / "stat-ok" / "Fixture.framework", slice_root / "Fixture.framework")
        check(root / "Fixture.xcframework", passes=True, expect="checked 1 bundle")
        shutil.copyfile(root / "stat-undeclared" / "Fixture.framework" / "PrivacyInfo.xcprivacy",
                        slice_root / "Fixture.framework" / "PrivacyInfo.xcprivacy")
        check(root / "Fixture.xcframework", passes=False, expect="symbol stat")

        # An application bundle is checked like a framework, nested ones too.
        app = root / "Fixture.app"
        app.mkdir()
        shutil.copyfile(libraries["clean"], app / "Fixture")
        (app / "Info.plist").write_bytes(plistlib.dumps(info(CFBundlePackageType="APPL")))
        (app / "PrivacyInfo.xcprivacy").write_bytes(plistlib.dumps(manifest()))
        (app / "Frameworks").mkdir()
        shutil.copytree(root / "stat-ok" / "Fixture.framework", app / "Frameworks" / "Fixture.framework")
        check(app, passes=True, expect="checked 2 bundle")
        (app / "Frameworks" / "Fixture.framework" / "PrivacyInfo.xcprivacy").unlink()
        check(app, passes=False, expect="PrivacyInfo.xcprivacy")

        # A path that does not exist, and one holding no bundle, fail.
        missing = run([sys.executable, "-B", str(options.checker), str(root / "absent")])
        if missing.returncode == 0:
            failures.append("a missing path passed")
        empty = root / "empty"
        empty.mkdir()
        nothing = run([sys.executable, "-B", str(options.checker), str(empty)])
        if nothing.returncode == 0:
            failures.append("a directory without a bundle passed")

    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
