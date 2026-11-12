# SPDX-License-Identifier: GPL-2.0-or-later
#
# The shared body of the iOS static XCFramework builders. A builder sets
# ROOT_DIR, SCRIPT, LABEL, PRODUCT, FRAMEWORK, SOURCE_NAME, LIBRARY, OUTPUT,
# WORK_ROOT, VERIFY_SCRIPT and JOBS, defines check_source, build_library,
# install_headers and record_provenance, and then sources this file, which
# parses the builder's arguments and builds, verifies and publishes the
# XCFramework with an arm64 device and an arm64 Simulator slice.
#
# build_library runs once per slice with platform, sdk_name, target, sdk_root,
# ar, ranlib, build_dir and library set, and leaves the static archive at
# $library. install_headers and record_provenance each receive the directory
# to fill.

SOURCE_ROOT=${UML_DEPENDENCY_SOURCE_ROOT:-"$ROOT_DIR/external/sources"}
DEPLOYMENT_TARGET=27.0
VERIFY_ONLY=0

fail()
{
	printf '%s\n' "$SCRIPT: $*" >&2
	exit 1
}

usage()
{
	printf '%s\n' \
		"Usage: $SCRIPT [options]" \
		"" \
		"Options:" \
		"  --source-root PATH  Verified dependency source trees" \
		"  --output PATH       Output XCFramework prefix" \
		"  --work-root PATH    Temporary build parent" \
		"  --jobs NUMBER       Parallel build jobs (default: 8)" \
		"  --verify-only       Verify an existing XCFramework prefix" \
		"  -h, --help          Show this help"
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--source-root)
		[ "$#" -ge 2 ] || fail "--source-root requires a path"
		SOURCE_ROOT=$2
		shift 2
		;;
	--output)
		[ "$#" -ge 2 ] || fail "--output requires a path"
		OUTPUT=$2
		shift 2
		;;
	--work-root)
		[ "$#" -ge 2 ] || fail "--work-root requires a path"
		WORK_ROOT=$2
		shift 2
		;;
	--jobs)
		[ "$#" -ge 2 ] || fail "--jobs requires a positive integer"
		JOBS=$2
		shift 2
		;;
	--verify-only)
		VERIFY_ONLY=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

case "$JOBS" in
''|*[!0-9]*) fail "jobs must be a positive integer" ;;
esac
[ "$JOBS" -gt 0 ] || fail "jobs must be a positive integer"
[ "$(uname -s)" = Darwin ] || fail "the iOS $LABEL build requires macOS"
case "$(uname -m)" in
arm64|aarch64) ;;
*) fail "the iOS $LABEL build requires an arm64 host" ;;
esac

. "$ROOT_DIR/dependencies/build-common.sh"

find_tool CMAKE cmake
find_tool MAKE gmake make
find_tool XCODEBUILD xcodebuild
find_tool XCRUN xcrun

OUTPUT_PARENT=$(dirname -- "$OUTPUT")
# Verification writes nothing, not even the parent of an absent prefix, and a
# build creates that parent only once its inputs have verified.
if [ -e "$OUTPUT" ] || [ -L "$OUTPUT" ]; then
	OUTPUT_PARENT=$(CDPATH= cd -- "$OUTPUT_PARENT" && pwd -P)
	OUTPUT="$OUTPUT_PARENT/$(basename -- "$OUTPUT")"
elif [ "$VERIFY_ONLY" -eq 1 ]; then
	fail "prefix to verify does not exist: $OUTPUT"
fi

verify_output()
{
	"$CMAKE" -DPREFIX="$1" -DSOURCE_ROOT="$ROOT_DIR" -P "$VERIFY_SCRIPT"
}

if [ "$VERIFY_ONLY" -eq 1 ]; then
	verify_output "$OUTPUT"
	printf '%s\n' "verified iOS $LABEL XCFramework: $OUTPUT"
	exit 0
fi

if [ -e "$OUTPUT" ] || [ -L "$OUTPUT" ]; then
	verify_output "$OUTPUT" ||
		fail "existing output is invalid and was left untouched: $OUTPUT"
	printf '%s\n' "reusing verified iOS $LABEL XCFramework: $OUTPUT"
	exit 0
fi

find_tool PYTHON python3
"$ROOT_DIR/dependencies/prepare.sh" --product "$PRODUCT" \
	--root "$SOURCE_ROOT" --verify
SOURCE="$SOURCE_ROOT/$SOURCE_NAME"
check_source

mkdir -p -- "$WORK_ROOT" "$OUTPUT_PARENT"
WORK_ROOT=$(CDPATH= cd -- "$WORK_ROOT" && pwd -P)
OUTPUT_PARENT=$(CDPATH= cd -- "$OUTPUT_PARENT" && pwd -P)
OUTPUT="$OUTPUT_PARENT/$(basename -- "$OUTPUT")"
TEMP_ROOT=$(mktemp -d "$WORK_ROOT/source-build.XXXXXX")
STAGE=$(mktemp -d "$OUTPUT_PARENT/.ios-$LABEL-prefix.XXXXXX")
cleanup()
{
	if [ -n "${TEMP_ROOT:-}" ] && [ -d "$TEMP_ROOT" ]; then
		"$CMAKE" -E remove_directory "$TEMP_ROOT"
	fi
	if [ -n "${STAGE:-}" ] && [ -d "$STAGE" ]; then
		"$CMAKE" -E remove_directory "$STAGE"
	fi
}
trap cleanup EXIT HUP INT TERM

build_slice()
{
	platform=$1
	sdk_name=$2
	target=$3
	build_dir="$TEMP_ROOT/build-$platform"
	library="$build_dir/$LIBRARY"
	framework="$TEMP_ROOT/framework-$platform/$FRAMEWORK.framework"
	sdk_root=$($XCRUN --sdk "$sdk_name" --show-sdk-path)
	ar=$($XCRUN --sdk "$sdk_name" --find ar)
	ranlib=$($XCRUN --sdk "$sdk_name" --find ranlib)

	build_library
	mkdir -p -- "$framework/Headers" "$framework/Modules"
	cp "$library" "$framework/$FRAMEWORK"
	install_headers "$framework/Headers"
	cp "$ROOT_DIR/dependencies/$SOURCE_NAME/module.modulemap" \
		"$framework/Modules/module.modulemap"
	"$CMAKE" -DFRAMEWORK_NAME="$FRAMEWORK" \
		-DOUTPUT="$framework/Info.plist" \
		-P "$ROOT_DIR/dependencies/cmake/ConfigureStaticFrameworkPlist.cmake"
}

build_slice ios iphoneos "arm64-apple-ios$DEPLOYMENT_TARGET"
build_slice ios-simulator iphonesimulator \
	"arm64-apple-ios$DEPLOYMENT_TARGET-simulator"

"$XCODEBUILD" -create-xcframework \
	-framework "$TEMP_ROOT/framework-ios/$FRAMEWORK.framework" \
	-framework "$TEMP_ROOT/framework-ios-simulator/$FRAMEWORK.framework" \
	-output "$STAGE/$FRAMEWORK.xcframework"

PROVENANCE="$STAGE/share/uml-dependencies"
CONTRACT=${PRODUCT#ios-}
mkdir -p -- "$PROVENANCE"
cp "$ROOT_DIR/dependencies/manifest.tsv" "$PROVENANCE/manifest.tsv"
cp "$ROOT_DIR/dependencies/platforms/ios-device-$CONTRACT-arm64.tsv" \
	"$PROVENANCE/ios-device-$CONTRACT-arm64.tsv"
cp "$ROOT_DIR/dependencies/platforms/ios-simulator-$CONTRACT-arm64.tsv" \
	"$PROVENANCE/ios-simulator-$CONTRACT-arm64.tsv"
record_provenance "$PROVENANCE"

publish_verified "$STAGE" "$OUTPUT" verify_output "iOS $LABEL XCFramework"
