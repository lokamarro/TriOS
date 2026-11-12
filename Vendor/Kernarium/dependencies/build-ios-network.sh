#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the authenticated GLib/libslirp stack for arm64 iOS.
# The script is offline: it accepts only the already verified dependency
# sources maintained by dependencies/prepare.sh.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE_ROOT=${UML_DEPENDENCY_SOURCE_ROOT:-"$ROOT_DIR/external/sources"}
PREFIX=${UML_IOS_NETWORK_DEPENDENCY_PREFIX:-}
WORK_ROOT=${UML_IOS_NETWORK_DEPENDENCY_BUILD_ROOT:-"$ROOT_DIR/.build/dependency-build/ios-network"}
VERIFY_SCRIPT="$ROOT_DIR/dependencies/cmake/VerifyIosNetworkPrefix.cmake"
JOBS=${UML_IOS_NETWORK_DEPENDENCY_JOBS:-8}
DEPLOYMENT_TARGET=27.0
PLATFORM=ios-simulator
VERIFY_ONLY=0

fail()
{
	printf '%s\n' "dependencies/build-ios-network.sh: $*" >&2
	exit 1
}

usage()
{
	printf '%s\n' \
		"Usage: dependencies/build-ios-network.sh [options]" \
		"" \
		"Options:" \
		"  --platform NAME    ios-simulator (default) or ios" \
		"  --source-root PATH  Verified dependency source trees" \
		"  --prefix PATH       Output prefix" \
		"  --work-root PATH    Temporary build parent" \
		"  --jobs NUMBER       Parallel build jobs (default: 8)" \
		"  --verify-only       Verify an existing prefix" \
		"  -h, --help          Show this help"
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--platform)
		[ "$#" -ge 2 ] || fail "--platform requires ios or ios-simulator"
		PLATFORM=$2
		shift 2
		;;
	--source-root)
		[ "$#" -ge 2 ] || fail "--source-root requires a path"
		SOURCE_ROOT=$2
		shift 2
		;;
	--prefix)
		[ "$#" -ge 2 ] || fail "--prefix requires a path"
		PREFIX=$2
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
[ "$(uname -s)" = Darwin ] || fail "the network build requires macOS"
case "$(uname -m)" in
arm64|aarch64) ;;
*) fail "the iOS network build requires an arm64 host" ;;
esac
case "$PLATFORM" in
ios-simulator)
	SDK_NAME=iphonesimulator
	TARGET=arm64-apple-ios${DEPLOYMENT_TARGET}-simulator
	SUBSYSTEM=ios-simulator
	CMAKE_SYSROOT=iphonesimulator
	RECEIPT_NAME=ios-simulator-network-arm64.tsv
		DEFAULT_PREFIX="$ROOT_DIR/.build/dependencies/ios-simulator-arm64/network"
	;;
ios)
	SDK_NAME=iphoneos
	TARGET=arm64-apple-ios${DEPLOYMENT_TARGET}
	SUBSYSTEM=ios
	CMAKE_SYSROOT=iphoneos
	RECEIPT_NAME=ios-device-network-arm64.tsv
		DEFAULT_PREFIX="$ROOT_DIR/.build/dependencies/ios-device-arm64/network"
	;;
*) fail "platform must be ios or ios-simulator" ;;
esac
RECEIPT="$ROOT_DIR/dependencies/platforms/$RECEIPT_NAME"
if [ -z "$PREFIX" ]; then
	PREFIX=$DEFAULT_PREFIX
fi
[ -r "$RECEIPT" ] || fail "platform receipt is unavailable: $RECEIPT"

PREFIX_PARENT=$(dirname -- "$PREFIX")
# Verification writes nothing, not even the parent of an absent prefix, and a
# build creates that parent only once its inputs have verified.
if [ -e "$PREFIX" ] || [ -L "$PREFIX" ]; then
	PREFIX_PARENT=$(CDPATH= cd -- "$PREFIX_PARENT" && pwd -P)
	PREFIX="$PREFIX_PARENT/$(basename -- "$PREFIX")"
elif [ "$VERIFY_ONLY" -eq 1 ]; then
	fail "prefix to verify does not exist: $PREFIX"
fi

verify_prefix()
{
	cmake -DPREFIX="$1" -DSOURCE_ROOT="$ROOT_DIR" \
		-DPLATFORM="$PLATFORM" -P "$VERIFY_SCRIPT"
}

if [ "$VERIFY_ONLY" -eq 1 ]; then
	verify_prefix "$PREFIX"
	printf '%s\n' "verified iOS network dependency prefix: $PREFIX"
	exit 0
fi

if [ -e "$PREFIX" ] || [ -L "$PREFIX" ]; then
	verify_prefix "$PREFIX" ||
		fail "existing prefix is invalid and was left untouched: $PREFIX"
	printf '%s\n' "reusing verified iOS network dependency prefix: $PREFIX"
	exit 0
fi

"$ROOT_DIR/dependencies/prepare.sh" --product ios-network \
	--root "$SOURCE_ROOT" --verify

. "$ROOT_DIR/dependencies/build-common.sh"

find_tool CMAKE cmake
if [ -x "$ROOT_DIR/.build/gpu-tools-venv/bin/meson" ]; then
	PATH="$ROOT_DIR/.build/gpu-tools-venv/bin:$PATH"
	export PATH
	MESON="$ROOT_DIR/.build/gpu-tools-venv/bin/meson"
else
	find_tool MESON meson
fi
find_tool NINJA ninja
find_tool MAKE gmake make
find_tool PKG_CONFIG pkg-config
find_tool RSYNC rsync
find_tool XCRUN xcrun
find_tool AUTORECONF autoreconf
find_tool PYTHON python3

SDK_ROOT=$($XCRUN --sdk "$SDK_NAME" --show-sdk-path)
[ -d "$SDK_ROOT" ] || fail "xcrun returned an invalid $PLATFORM SDK"
CC=$($XCRUN --sdk "$SDK_NAME" --find clang)
CXX=$($XCRUN --sdk "$SDK_NAME" --find clang++)
AR=$($XCRUN --sdk "$SDK_NAME" --find ar)
NM=$($XCRUN --sdk "$SDK_NAME" --find nm)
RANLIB=$($XCRUN --sdk "$SDK_NAME" --find ranlib)
STRIP=$($XCRUN --sdk "$SDK_NAME" --find strip)

mkdir -p -- "$WORK_ROOT" "$PREFIX_PARENT"
WORK_ROOT=$(CDPATH= cd -- "$WORK_ROOT" && pwd -P)
PREFIX_PARENT=$(CDPATH= cd -- "$PREFIX_PARENT" && pwd -P)
PREFIX="$PREFIX_PARENT/$(basename -- "$PREFIX")"
TEMP_ROOT=$(mktemp -d "$WORK_ROOT/source-build.XXXXXX")
STAGE=$(mktemp -d "$PREFIX_PARENT/.ios-network-prefix.XXXXXX")
cleanup()
{
	if [ -n "${TEMP_ROOT:-}" ] && [ -d "$TEMP_ROOT" ]; then
		rm -rf -- "$TEMP_ROOT"
	fi
	if [ -n "${STAGE:-}" ] && [ -d "$STAGE" ]; then
		rm -rf -- "$STAGE"
	fi
}
trap cleanup EXIT HUP INT TERM

BUILDS="$TEMP_ROOT/builds"
GLIB_SOURCE="$TEMP_ROOT/glib"
LIBFFI_SOURCE="$TEMP_ROOT/libffi"
CROSS_FILE="$BUILDS/meson-$PLATFORM.cross"
mkdir -p -- "$BUILDS" "$GLIB_SOURCE/subprojects/proxy-libintl-0.5" \
	"$GLIB_SOURCE/subprojects/gvdb" "$LIBFFI_SOURCE" \
	"$STAGE/share/uml-dependencies"
$RSYNC -a --exclude .git "$SOURCE_ROOT/glib-ios/" "$GLIB_SOURCE/"
$RSYNC -a --exclude .git "$SOURCE_ROOT/libffi/" "$LIBFFI_SOURCE/"
$RSYNC -a --exclude .git "$SOURCE_ROOT/proxy-libintl/" \
	"$GLIB_SOURCE/subprojects/proxy-libintl-0.5/"
$RSYNC -a --exclude .git "$SOURCE_ROOT/gvdb/" \
	"$GLIB_SOURCE/subprojects/gvdb/"

printf '%s\n' "building PCRE2 10.49 for $PLATFORM"
"$CMAKE" -S "$SOURCE_ROOT/pcre2" -B "$BUILDS/pcre2" -G Ninja \
	-DCMAKE_MAKE_PROGRAM="$NINJA" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$STAGE" \
	-DCMAKE_INSTALL_LIBDIR=lib \
	-DCMAKE_SYSTEM_NAME=iOS \
	-DCMAKE_OSX_SYSROOT="$CMAKE_SYSROOT" \
	-DCMAKE_OSX_ARCHITECTURES=arm64 \
	-DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
	-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
	-DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
	-DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=OFF \
	-DPCRE2_BUILD_PCRE2_32=OFF -DPCRE2_BUILD_PCRE2GREP=OFF \
	-DPCRE2_BUILD_TESTS=OFF -DPCRE2_SUPPORT_LIBZ=OFF \
	-DPCRE2_SUPPORT_LIBBZ2=OFF -DPCRE2_SUPPORT_LIBREADLINE=OFF \
	-DPCRE2_SUPPORT_LIBEDIT=OFF
"$CMAKE" --build "$BUILDS/pcre2" --parallel "$JOBS"
"$CMAKE" --install "$BUILDS/pcre2"

printf '%s\n' "building libffi 3.8.0 for $PLATFORM"
(
	CDPATH= cd -- "$LIBFFI_SOURCE"
	./autogen.sh
)
mkdir -p -- "$BUILDS/libffi"
(
	CDPATH= cd -- "$BUILDS/libffi"
	env CC="$CC -target $TARGET -isysroot $SDK_ROOT" \
		CXX="$CXX -target $TARGET -isysroot $SDK_ROOT" \
		AR="$AR" NM="$NM" RANLIB="$RANLIB" \
		CFLAGS="-O2 -fPIC" LDFLAGS="-target $TARGET -isysroot $SDK_ROOT" \
		"$LIBFFI_SOURCE/configure" \
		--build=arm-apple-darwin --host=aarch64-apple-darwin \
		--prefix="$STAGE" --libdir="$STAGE/lib" \
		--disable-shared --enable-static --disable-docs \
		--disable-multi-os-directory
	"$MAKE" -j "$JOBS"
	"$MAKE" install
)

cat > "$CROSS_FILE" <<EOF
[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-I$STAGE/include']
cpp_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-I$STAGE/include']
objc_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-I$STAGE/include']
objcpp_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-I$STAGE/include']
c_link_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-L$STAGE/lib']
cpp_link_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-L$STAGE/lib']
objc_link_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-L$STAGE/lib']
objcpp_link_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-L$STAGE/lib']

[binaries]
c = ['$CC']
cpp = ['$CXX']
objc = ['$CC']
objcpp = ['$CXX']
ar = ['$AR']
nm = ['$NM']
ranlib = ['$RANLIB']
strip = ['$STRIP', '-x']
pkg-config = ['$PKG_CONFIG']

[host_machine]
system = 'darwin'
kernel = 'xnu'
subsystem = '$SUBSYSTEM'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
EOF

meson_setup()
{
	PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$STAGE/lib/pkgconfig" \
		PKG_CONFIG_SYSROOT_DIR= \
		"$MESON" setup "$@" --cross-file "$CROSS_FILE"
}

# GLib compiles its locale, character-set alias, module, libexec and state
# directories in from its prefix, so it is configured with the neutral one and
# installed into an image that is merged into the stage. The libraries that
# are linked into the runtime hold no path of this build.
printf '%s\n' "building GLib 2.90.1 for $PLATFORM"
meson_setup "$BUILDS/glib" "$GLIB_SOURCE" \
	--prefix "$NO_HOST_PREFIX" --libdir=lib --buildtype=release \
	--default-library=static --wrap-mode=nodownload \
	--force-fallback-for=intl \
	-Dtests=false -Dinstalled_tests=false -Dnls=disabled \
	-Dintrospection=disabled -Ddocumentation=false -Dman-pages=disabled \
	-Dselinux=disabled -Dlibmount=disabled -Dsysprof=disabled \
	-Dxattr=false -Ddtrace=disabled -Dsystemtap=disabled \
	-Dlibelf=disabled -Dglib_debug=disabled -Dbsymbolic_functions=false
"$MESON" compile -C "$BUILDS/glib" -j "$JOBS"
GLIB_IMAGE="$TEMP_ROOT/glib-image"
DESTDIR="$GLIB_IMAGE" "$MESON" install -C "$BUILDS/glib"
merge_neutral_image "$GLIB_IMAGE" "$STAGE" "$STAGE"

printf '%s\n' "building libslirp 4.9.7 for $PLATFORM"
meson_setup "$BUILDS/libslirp" "$SOURCE_ROOT/libslirp" \
	--prefix "$STAGE" --libdir=lib --buildtype=release \
	--default-library=static --wrap-mode=nofallback \
	-Dstatic=false -Doss-fuzz=false -Dllvm-fuzz=false \
	-Dfuzz-reproduce=false
"$MESON" compile -C "$BUILDS/libslirp" -j "$JOBS"
"$MESON" install -C "$BUILDS/libslirp"

cp "$ROOT_DIR/dependencies/manifest.tsv" \
	"$STAGE/share/uml-dependencies/manifest.tsv"
cp "$RECEIPT" \
	"$STAGE/share/uml-dependencies/$RECEIPT_NAME"
printf 'source\ttree\nglib-ios\t%s\nlibslirp\t%s\n' \
	"$(git -C "$SOURCE_ROOT/glib-ios" rev-parse 'HEAD^{tree}')" \
	"$(git -C "$SOURCE_ROOT/libslirp" rev-parse 'HEAD^{tree}')" \
	> "$STAGE/share/uml-dependencies/source-trees.tsv"
publish_verified "$STAGE" "$PREFIX" verify_prefix \
	"iOS network dependency prefix"
