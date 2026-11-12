#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build, install and drive the audio interrupter on a connected iOS device.

AudioInterrupter is test support: a small application that plays digital
silence on a non-mixable audio session, so that a run can watch what
Kernarium does when another application takes the audio session from it
(Kernarium mutes its guest's audio and takes the session back when the
interrupter lets go). It needs no UI automation: this script installs it and
launches and ends it with `devicectl`.

  audio-interrupter.py build
  audio-interrupter.py --device UDID install
  audio-interrupter.py --device UDID launch --hold 60 --end quiet --console
  audio-interrupter.py --device UDID terminate --pid PID

`launch` starts the interrupter in the foreground, which iOS always lets take
the session, so Kernarium goes to the background and is interrupted; bring it
back with `devicectl device process launch --activate` on its bundle
identifier. `launch --background` starts it behind Kernarium instead, where iOS
may refuse a non-mixable session (the interrupter logs the refusal); `--mode
switch` then plays on a mixable session first and takes the session once other
audio plays.

The interrupter ends itself after `--hold` seconds of owning the session:
`--end quiet` deactivates without telling the interrupted application (it
learns from the hint that other audio stopped), `--end notify` deactivates with
the notify-others option (it receives a resumption recommendation), and
`--end stay` keeps the session until `terminate`. Every decision is printed as
an AUDIO_INTERRUPTER line, which `--console` shows.

The bundle identifier comes from the Kernarium project: the interrupter's
identifier is the Kernarium identifier followed by AudioInterrupter. The
signing team is the one Kernarium builds with, from UML_DEVELOPMENT_TEAM or
from the ignored GuestHost/Config/Signing.local.xcconfig; automatic signing
registers the interrupter under it.
"""

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

SCRIPTS = pathlib.Path(__file__).resolve().parent
PROJECT = SCRIPTS.parent / "AudioInterrupter" / "AudioInterrupter.xcodeproj"
KERNARIUM_PROJECT = SCRIPTS.parent / "Kernarium.xcodeproj" / "project.pbxproj"
SIGNING = SCRIPTS.parents[1] / "GuestHost" / "Config" / "Signing.local.xcconfig"
TEAM_VARIABLE = "UML_DEVELOPMENT_TEAM"
DEFAULT_OUTPUT = SCRIPTS.parents[1] / ".build" / "audio-interrupter"
SUFFIX = "AudioInterrupter"
MODES = ("direct", "switch")
ENDS = ("quiet", "notify", "stay")


def signing_team(environment: dict[str, str] | None = None, signing: pathlib.Path = SIGNING) -> str:
    """The development team: UML_DEVELOPMENT_TEAM, else the local signing xcconfig."""
    environment = os.environ if environment is None else environment
    team = environment.get(TEAM_VARIABLE, "").strip()
    if not team and signing.is_file():
        teams = re.findall(r"^\s*DEVELOPMENT_TEAM\s*=\s*([^\s/]+)", signing.read_text(), re.M)
        team = teams[-1] if teams else ""
    if not re.fullmatch(r"[A-Z0-9]{10}", team):
        raise SystemExit(
            f"no development team: set {TEAM_VARIABLE}, or copy {signing.name}.example to "
            f"{signing.name} in {signing.parent.parent.name}/{signing.parent.name} and name your team there")
    return team


def kernarium_identifier(project: pathlib.Path = KERNARIUM_PROJECT) -> str:
    """The Kernarium bundle identifier."""
    identifiers = set(re.findall(r"\bPRODUCT_BUNDLE_IDENTIFIER = ([A-Za-z0-9.-]+);", project.read_text()))
    if not identifiers:
        raise SystemExit(f"{project}: no PRODUCT_BUNDLE_IDENTIFIER")
    return min(identifiers, key=len)


def build_command(args: argparse.Namespace) -> list[str]:
    team, identifier = signing_team(), kernarium_identifier()
    command = [
        "xcodebuild", "-project", str(PROJECT), "-scheme", SUFFIX,
        "-configuration", args.configuration, "-destination", "generic/platform=iOS",
        "-derivedDataPath", str(args.derived_data),
    ]
    if not args.no_provisioning_updates:
        command.append("-allowProvisioningUpdates")
    command += [
        f"DEVELOPMENT_TEAM={team}",
        f"KERNARIUM_BUNDLE_IDENTIFIER={identifier}",
        f"CURRENT_PROJECT_VERSION={args.build_version}",
        "build",
    ]
    return command


def built_app(args: argparse.Namespace) -> pathlib.Path:
    return args.derived_data / "Build" / "Products" / f"{args.configuration}-iphoneos" / f"{SUFFIX}.app"


def output_app(args: argparse.Namespace) -> pathlib.Path:
    return args.output / f"{SUFFIX}.app"


def launch_command(args: argparse.Namespace) -> tuple[list[str], dict[str, str]]:
    """The devicectl launch command and the environment that carries the settings."""
    identifier = kernarium_identifier()
    environment = {
        "DEVICECTL_CHILD_AUDIO_INTERRUPTER_MODE": args.mode,
        "DEVICECTL_CHILD_AUDIO_INTERRUPTER_HOLD": str(args.hold),
        "DEVICECTL_CHILD_AUDIO_INTERRUPTER_END": args.end,
        "DEVICECTL_CHILD_AUDIO_INTERRUPTER_MAX": str(args.max),
        "DEVICECTL_CHILD_AUDIO_INTERRUPTER_RETRY": str(args.retry),
    }
    command = [
        "xcrun", "devicectl", "device", "process", "launch", "--device", args.device,
        "--terminate-existing", "--no-activate" if args.background else "--activate",
    ]
    if args.console:
        command.append("--console")
    command.append(identifier + SUFFIX)
    return command, environment


def run(command: list[str], dry_run: bool, environment: dict[str, str] | None = None) -> int:
    shown = " ".join(f"{k}={v}" for k, v in (environment or {}).items())
    print(("+ " + shown + " " if shown else "+ ") + " ".join(command), flush=True)
    if dry_run:
        return 0
    env = dict(os.environ)
    env.update(environment or {})
    return subprocess.run(command, stdin=subprocess.DEVNULL, env=env, check=False).returncode


def require_device(args: argparse.Namespace) -> None:
    if not args.device:
        raise SystemExit(f"audio-interrupter.py {args.command}: --device is required")


def bundle_version(app: pathlib.Path) -> str:
    done = subprocess.run(
        ["/usr/libexec/PlistBuddy", "-c", "Print :CFBundleVersion", str(app / "Info.plist")],
        capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL)
    return done.stdout.strip()


def do_build(args: argparse.Namespace) -> int:
    code = run(build_command(args), args.dry_run)
    if code != 0 or args.dry_run:
        return code
    source = built_app(args)
    if not source.is_dir():
        print(f"build produced no {source}", file=sys.stderr)
        return 1
    destination = output_app(args)
    args.output.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(source, destination, symlinks=True)
    code = run(["codesign", "--verify", "--deep", "--strict", str(destination)], False)
    if code != 0:
        return code
    print(json.dumps({"app": str(destination), "bundle_identifier": kernarium_identifier() + SUFFIX,
                      "version": bundle_version(destination)}))
    return 0


def do_install(args: argparse.Namespace) -> int:
    require_device(args)
    app = args.app or output_app(args)
    if not args.dry_run and not app.is_dir():
        raise SystemExit(f"{app}: not built; run `build` first")
    code = run(["codesign", "--verify", "--deep", "--strict", str(app)], args.dry_run)
    if code != 0:
        return code
    return run(["xcrun", "devicectl", "device", "install", "app", "--device", args.device, str(app),
                "--timeout", str(args.timeout)], args.dry_run)


def do_launch(args: argparse.Namespace) -> int:
    require_device(args)
    command, environment = launch_command(args)
    return run(command, args.dry_run, environment)


def do_terminate(args: argparse.Namespace) -> int:
    require_device(args)
    return run(["xcrun", "devicectl", "device", "process", "terminate", "--device", args.device,
                "--pid", str(args.pid)], args.dry_run)


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    root.add_argument("--device", help="device identifier, UDID or name")
    root.add_argument("--dry-run", action="store_true", help="print the commands without running them")
    commands = root.add_subparsers(dest="command", required=True)

    build = commands.add_parser("build", help="build and sign the application")
    build.add_argument("--configuration", choices=("Debug", "Release"), default="Release")
    build.add_argument("--derived-data", type=pathlib.Path, default=DEFAULT_OUTPUT / "derived")
    build.add_argument("--output", type=pathlib.Path, default=DEFAULT_OUTPUT)
    build.add_argument("--build-version", default="1", help="CFBundleVersion, to tell builds apart on a device")
    build.add_argument("--no-provisioning-updates", action="store_true")
    build.set_defaults(handler=do_build)

    install = commands.add_parser("install", help="install the built application")
    install.add_argument("--app", type=pathlib.Path)
    install.add_argument("--output", type=pathlib.Path, default=DEFAULT_OUTPUT)
    install.add_argument("--timeout", type=int, default=300)
    install.set_defaults(handler=do_install)

    launch = commands.add_parser("launch", help="launch the application")
    launch.add_argument("--mode", choices=MODES, default="direct")
    launch.add_argument("--hold", type=float, default=60.0, help="seconds of owning the session before it ends")
    launch.add_argument("--end", choices=ENDS, default="quiet")
    launch.add_argument("--max", type=float, default=300.0, help="seconds of life before it exits")
    launch.add_argument("--retry", type=float, default=5.0, help="seconds between takeover attempts")
    launch.add_argument("--background", action="store_true", help="launch behind the frontmost application")
    launch.add_argument("--console", action="store_true", help="stay attached and print its console")
    launch.set_defaults(handler=do_launch)

    terminate = commands.add_parser("terminate", help="terminate a running instance by pid")
    terminate.add_argument("--pid", type=int, required=True)
    terminate.set_defaults(handler=do_terminate)
    return root


def main() -> int:
    args = parser().parse_args()
    return args.handler(args)


if __name__ == "__main__":
    raise SystemExit(main())
