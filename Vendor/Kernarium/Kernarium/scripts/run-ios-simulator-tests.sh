#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build KernariumTests and KernariumUITests for the iOS Simulator and run them
# in one iPad Simulator.
#
# Two things stand in for what a device or a Mac run provides. The Simulator's
# application accessibility preference is turned on each run (it does not
# survive a reboot), because the hosted keyboard and ended-run tests walk the
# accessibility tree, which UIKit publishes only while it is on. And the
# accepted KDE desktop image (tools/rootfs/desktop/images.json) is linked into
# the application's container and selected for the test host, because the
# hosted GuestRunEndTests stop the guest the application boots as it launches
# and HostGraphicsStackTests select the Linux runtime, which a guest of another
# flavor would have taken; the Simulator reads the image where it lies. The
# Simulator has no hardware video encoder, so HostVideoEncoderTests skip.
#
# The slices must be packaged first (README: Simulator): the preset
# kernarium-ios-simulator and its Android runtime preset, then
# package-ios-frameworks.sh --sdk simulator. The destination is a concrete
# Simulator, which builds the Simulator's one architecture, arm64.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
DEVICE_NAME='iPad Pro 11-inch (M5)'
BUNDLE_ID=com.imxieyi.Kernarium
DERIVED_DATA="$ROOT_DIR/.build/kernarium-simulator-derived"
JOBS=4
ONLY_TESTING=

fail()
{
	printf '%s\n' "Kernarium/scripts/run-ios-simulator-tests.sh: $*" >&2
	exit 1
}

usage()
{
	printf '%s\n' \
		"Usage: Kernarium/scripts/run-ios-simulator-tests.sh [options]" \
		"" \
		"Options:" \
		"  --device NAME  Simulator device name (default: $DEVICE_NAME);" \
		"                 its newest iOS runtime is used" \
		"  --only-testing TEST  Run TEST instead of both test targets;" \
		"                 repeatable, e.g. KernariumTests/SerialConsoleTests" \
		"  --jobs NUMBER  Parallel build jobs (default: $JOBS)" \
		"  -h, --help      Show this help"
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--device)
		[ "$#" -ge 2 ] || fail "--device requires a name"
		DEVICE_NAME=$2
		shift 2
		;;
	--only-testing)
		[ "$#" -ge 2 ] || fail "--only-testing requires a test"
		ONLY_TESTING="$ONLY_TESTING
$2"
		shift 2
		;;
	--jobs)
		[ "$#" -ge 2 ] || fail "--jobs requires a number"
		JOBS=$2
		shift 2
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		usage >&2
		fail "unknown option: $1"
		;;
	esac
done

# The newest iOS runtime's device of that name.
UDID=$(xcrun simctl list devices available --json | python3 -B -c '
import json
import re
import sys

name = sys.argv[1]
best = None
for runtime, devices in json.load(sys.stdin)["devices"].items():
    match = re.search(r"\.iOS-(\d+(?:-\d+)*)$", runtime)
    if not match:
        continue
    version = tuple(int(part) for part in match.group(1).split("-"))
    for device in devices:
        if device["name"] == name and device["isAvailable"]:
            if best is None or version > best[0]:
                best = (version, device["udid"])
if best is None:
    sys.exit(1)
print(best[1])
' "$DEVICE_NAME") || fail "no available iOS Simulator device named $DEVICE_NAME"

BOOTED_HERE=0
if ! xcrun simctl list devices | grep "$UDID" | grep -q Booted; then
	xcrun simctl boot "$UDID"
	BOOTED_HERE=1
fi
cleanup()
{
	if [ "$BOOTED_HERE" -eq 1 ]; then
		xcrun simctl shutdown "$UDID" || true
	fi
}
trap cleanup EXIT HUP INT TERM
xcrun simctl bootstatus "$UDID" -b > /dev/null
xcrun simctl spawn "$UDID" defaults write com.apple.Accessibility \
	ApplicationAccessibilityEnabled -bool true

CDPATH= cd -- "$ROOT_DIR"
xcodebuild build-for-testing \
	-project Kernarium/Kernarium.xcodeproj \
	-scheme Kernarium \
	-destination "id=$UDID" \
	-derivedDataPath "$DERIVED_DATA" \
	-jobs "$JOBS" \
	CODE_SIGNING_ALLOWED=NO

# The accepted KDE image, which Kernarium selects when its settings name none
# (RootfsImage.defaultFilename).
ROOTFS=$(python3 -B - "$ROOT_DIR/tools/rootfs/desktop/images.json" <<'PYTHON'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    accepted = json.load(handle)["images"]["kde"]["accepted"]
print(f"{accepted['release']}/{accepted['file']}")
PYTHON
) || fail "cannot read the accepted KDE image from images.json"
ROOTFS="$ROOT_DIR/.build/rootfs/$ROOTFS"
[ -f "$ROOTFS" ] || fail "rootfs image is unavailable: $ROOTFS"
ROOTFS_FILENAME=$(basename -- "$ROOTFS")

xcrun simctl install "$UDID" \
	"$DERIVED_DATA/Build/Products/Debug-iphonesimulator/Kernarium.app"
CONTAINER=$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data)
mkdir -p -- "$CONTAINER/Documents"
ln -sf "$ROOTFS" "$CONTAINER/Documents/$ROOTFS_FILENAME"

# xcodebuild passes each TEST_RUNNER_-prefixed variable to the test host with
# the prefix removed.
set --
OLD_IFS=$IFS
IFS='
'
for test in $ONLY_TESTING; do
	[ -z "$test" ] || set -- "$@" "-only-testing:$test"
done
IFS=$OLD_IFS
TEST_RUNNER_KERNARIUM_ROOTFS=$ROOTFS_FILENAME xcodebuild test-without-building \
	-project Kernarium/Kernarium.xcodeproj \
	-scheme Kernarium \
	-destination "id=$UDID" \
	-derivedDataPath "$DERIVED_DATA" \
	-parallel-testing-enabled NO \
	"$@" \
	CODE_SIGNING_ALLOWED=NO
