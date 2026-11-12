#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the Apple GPU product -- Mesa (Zink on KosmicKrisp), libepoxy and
# virglrenderer -- from sources authenticated by dependencies/prepare.sh.
#
# The ios-simulator platform is the product's software variant: the iOS
# Simulator has no Metal 4 device, so KosmicKrisp cannot run there. It builds
# the same pinned Mesa, libepoxy and virglrenderer as CPU rasterizers
# (MESA_SOFTWARE_DRIVERS) with no Vulkan driver and no Venus. llvmpipe, the
# fast one, JITs through LLVM, which dependencies/build-apple-gpu-llvm.sh
# builds from its authenticated source archive and Mesa links statically; the
# macOS and iOS device builds neither build nor link it.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MANIFEST="$ROOT_DIR/dependencies/manifest.tsv"
SOURCE_ROOT=${UML_DEPENDENCY_SOURCE_ROOT:-"$ROOT_DIR/external/sources"}
VERIFY_SCRIPT="$ROOT_DIR/dependencies/cmake/VerifyAppleGpuPrefix.cmake"
CACHE_ROOT=${UML_DEPENDENCY_CACHE:-"$ROOT_DIR/.build/dependency-cache"}
LLVM_ARCHIVE_MANIFEST="$ROOT_DIR/dependencies/llvm/archives.tsv"
PREFIX=${UML_APPLE_GPU_DEPENDENCY_PREFIX:-}
WORK_ROOT=${UML_APPLE_GPU_DEPENDENCY_BUILD_ROOT:-}
JOBS=${UML_APPLE_GPU_DEPENDENCY_JOBS:-8}
PLATFORM=macos
# The one Apple GPU product names the source matrix, the prefix and the receipt.
PRODUCT=apple-gpu-vulkan-experiment
VERIFY_ONLY=0

fail()
{
	printf '%s\n' "dependencies/build-apple-gpu.sh: $*" >&2
	exit 1
}

usage()
{
	printf '%s\n' \
		"Usage: dependencies/build-apple-gpu.sh [options]" \
		"" \
		"Options:" \
		"  --platform NAME      macos (default), ios or ios-simulator" \
		"  --source-root PATH   Verified dependency source trees" \
		"  --prefix PATH        Output prefix" \
		"  --work-root PATH     Build parent directory" \
		"  --jobs NUMBER        Parallel build jobs (default: 8)" \
		"  --verify-only        Verify an existing prefix" \
		"  -h, --help           Show this help" \
		"" \
		"The build performs no network access. Run dependencies/prepare.sh" \
		"--product apple-gpu-vulkan-experiment first to materialize the" \
		"pinned source matrix."
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--platform)
		[ "$#" -ge 2 ] || fail "--platform requires a value"
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
[ "$(uname -s)" = Darwin ] || fail "the Apple GPU build requires macOS"
case "$(uname -m)" in
arm64|aarch64) ;;
*) fail "the Apple GPU build requires an arm64 host" ;;
esac

# Every Apple product, and the build machine's own pass, builds at this floor.
DEPLOYMENT_TARGET=27.0

# The software variant has no Vulkan driver, no Venus protocol and no render
# server; the other platforms render on KosmicKrisp.
SOFTWARE=0
MESA_SOFTWARE_DRIVERS=
case "$PLATFORM" in
macos)
	SDK_NAME=macosx
	TARGET=arm64-apple-macos$DEPLOYMENT_TARGET
	EPOXY_SYSTEM=darwin
	SUBSYSTEM=macos
	CONTRACT_NAME=macos-gpu-vulkan-arm64.tsv
	MINIMUM_OS_KEY=LSMinimumSystemVersion
	PREFIX_PLATFORM=macos-arm64
	;;
ios)
	SDK_NAME=iphoneos
	TARGET=arm64-apple-ios$DEPLOYMENT_TARGET
	EPOXY_SYSTEM=ios
	SUBSYSTEM=ios
	CONTRACT_NAME=ios-device-gpu-vulkan-arm64.tsv
	MINIMUM_OS_KEY=MinimumOSVersion
	PREFIX_PLATFORM=ios-device-arm64
	;;
ios-simulator)
	SDK_NAME=iphonesimulator
	TARGET=arm64-apple-ios$DEPLOYMENT_TARGET-simulator
	EPOXY_SYSTEM=ios
	SUBSYSTEM=ios-simulator
	CONTRACT_NAME=ios-simulator-gpu-software-arm64.tsv
	MINIMUM_OS_KEY=MinimumOSVersion
	PREFIX_PLATFORM=ios-simulator-arm64
	SOFTWARE=1
	# The Gallium drivers the Simulator's host GL provider renders with: Mesa
	# prefers llvmpipe, and GALLIUM_DRIVER=softpipe selects the interpreter
	# that needs no JIT. The list is also the contract's gl_provider.
	MESA_SOFTWARE_DRIVERS=llvmpipe,softpipe
	;;
*) fail "platform must be macos, ios or ios-simulator" ;;
esac

# An iOS prefix is cross compiled: its binaries cannot run on the build
# machine, so Mesa's helper tools are built separately for macOS and the
# sysroot is carried by meson machine files rather than by SDKROOT.
if [ "$PLATFORM" = macos ]; then
	CROSS_BUILD=0
else
	CROSS_BUILD=1
fi
DEFAULT_PREFIX="$ROOT_DIR/.build/dependencies/$PRODUCT/$PREFIX_PLATFORM"

PLATFORM_CONTRACT="$ROOT_DIR/dependencies/platforms/$CONTRACT_NAME"
[ -r "$MANIFEST" ] || fail "source manifest is unavailable: $MANIFEST"
MESA_VERSION=$(awk -F'\t' '$1 == "mesa" { print $2 }' "$MANIFEST")
[ -r "$PLATFORM_CONTRACT" ] ||
	fail "platform contract is unavailable: $PLATFORM_CONTRACT"
if [ "$SOFTWARE" -eq 1 ]; then
	[ "$(awk -F '\t' '$1 == "gl_provider" { print $2 }' "$PLATFORM_CONTRACT")" = \
		"$MESA_SOFTWARE_DRIVERS" ] ||
		fail "the platform contract's gl_provider is not $MESA_SOFTWARE_DRIVERS"
fi
[ -n "$PREFIX" ] || PREFIX=$DEFAULT_PREFIX
[ -n "$WORK_ROOT" ] ||
	WORK_ROOT="$ROOT_DIR/.build/dependency-build/$PRODUCT/$PLATFORM"

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
	printf '%s\n' "verified $PLATFORM Apple GPU dependency prefix: $PREFIX"
	exit 0
fi

if [ -e "$PREFIX" ] || [ -L "$PREFIX" ]; then
	verify_prefix "$PREFIX" ||
		fail "existing prefix is invalid and was left untouched: $PREFIX"
	printf '%s\n' "reusing verified $PLATFORM Apple GPU prefix: $PREFIX"
	exit 0
fi

"$ROOT_DIR/dependencies/prepare.sh" --product "$PRODUCT" \
	--root "$SOURCE_ROOT" --verify
LLVM_ARCHIVE=
if [ "$SOFTWARE" -eq 1 ]; then
	"$ROOT_DIR/dependencies/prepare.sh" --archive-manifest \
		"$LLVM_ARCHIVE_MANIFEST" --cache "$CACHE_ROOT" --verify
	CACHE_ROOT=$(CDPATH= cd -- "$CACHE_ROOT" && pwd -P)
	LLVM_ARCHIVE=$(awk -F '\t' '$1 != "name" && $0 !~ /^#/ \
		{ print $3 "-" $1 }' "$LLVM_ARCHIVE_MANIFEST")
	[ -n "$LLVM_ARCHIVE" ] || fail "no LLVM archive in $LLVM_ARCHIVE_MANIFEST"
	LLVM_ARCHIVE="$CACHE_ROOT/archives/$LLVM_ARCHIVE"
fi
SOURCE_ROOT=$(CDPATH= cd -- "$SOURCE_ROOT" && pwd -P)
EPOXY_SOURCE="$SOURCE_ROOT/libepoxy"
VIRGL_SOURCE="$SOURCE_ROOT/virglrenderer"
MESA_SOURCE="$SOURCE_ROOT/mesa"
VULKAN_HEADERS_SOURCE="$SOURCE_ROOT/vulkan-headers"
VENUS_PROTOCOL_SOURCE="$SOURCE_ROOT/venus-protocol"

. "$ROOT_DIR/dependencies/build-common.sh"

if [ -x "$ROOT_DIR/.build/gpu-tools-venv/bin/meson" ]; then
	PATH="$ROOT_DIR/.build/gpu-tools-venv/bin:$PATH"
	export PATH
	MESON="$ROOT_DIR/.build/gpu-tools-venv/bin/meson"
else
	find_tool MESON meson
fi
find_tool RSYNC rsync
find_tool PKG_CONFIG pkg-config
find_tool XCRUN xcrun
find_tool INSTALL_NAME_TOOL install_name_tool
find_tool OTOOL otool
find_tool PLUTIL plutil
find_tool PYTHON "${UML_MESA_PYTHON:-python3}"
find_tool NINJA ninja
"$PYTHON" -c 'import mako, packaging, yaml' >/dev/null 2>&1 ||
	fail "KosmicKrisp requires Python modules mako, packaging, and yaml"
find_tool BISON bison
find_tool FLEX flex

SDK_ROOT=$($XCRUN --sdk "$SDK_NAME" --show-sdk-path)
[ -d "$SDK_ROOT" ] || fail "xcrun returned an invalid $PLATFORM SDK"
CC=$($XCRUN --sdk "$SDK_NAME" --find clang)
CXX=$($XCRUN --sdk "$SDK_NAME" --find clang++)
AR=$($XCRUN --sdk "$SDK_NAME" --find ar)
NM=$($XCRUN --sdk "$SDK_NAME" --find nm)
RANLIB=$($XCRUN --sdk "$SDK_NAME" --find ranlib)
STRIP=$($XCRUN --sdk "$SDK_NAME" --find strip)

# The build machine toolchain. On a native macOS build it is the same one; a
# cross build additionally compiles Mesa's helper tools and the Venus protocol
# generator, which run here rather than on the target.
NATIVE_SDK_ROOT=$($XCRUN --sdk macosx --show-sdk-path)
[ -d "$NATIVE_SDK_ROOT" ] || fail "xcrun returned an invalid macOS SDK"
NATIVE_TARGET=arm64-apple-macos$DEPLOYMENT_TARGET
NATIVE_CC=$($XCRUN --sdk macosx --find clang)
NATIVE_CXX=$($XCRUN --sdk macosx --find clang++)

mkdir -p -- "$WORK_ROOT" "$PREFIX_PARENT"
WORK_ROOT=$(CDPATH= cd -- "$WORK_ROOT" && pwd -P)
PREFIX_PARENT=$(CDPATH= cd -- "$PREFIX_PARENT" && pwd -P)
PREFIX="$PREFIX_PARENT/$(basename -- "$PREFIX")"
TEMP_ROOT=$(mktemp -d "$WORK_ROOT/source-build.XXXXXX")
STAGE=$(mktemp -d "$PREFIX_PARENT/.apple-gpu-prefix.XXXXXX")
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

SOURCES="$TEMP_ROOT/sources"
BUILDS="$TEMP_ROOT/builds"
EPOXY_CROSS="$BUILDS/meson-epoxy-$PLATFORM.cross"
VIRGL_CROSS="$BUILDS/meson-virgl-$PLATFORM.cross"
NATIVE_FILE="$BUILDS/meson-native-$PLATFORM.ini"
mkdir -p -- "$SOURCES/libepoxy" \
	"$SOURCES/virglrenderer" "$BUILDS" "$STAGE/include" \
	"$STAGE/lib" "$STAGE/Frameworks"
mkdir -p -- "$SOURCES/Mesa"

$RSYNC -a --exclude .git "$EPOXY_SOURCE/" "$SOURCES/libepoxy/"
$RSYNC -a --exclude .git "$VIRGL_SOURCE/" "$SOURCES/virglrenderer/"
$RSYNC -a --exclude .git "$MESA_SOURCE/" "$SOURCES/Mesa/"

APPLE_GPU_TEMP_DIR=${TMPDIR:-/tmp}
APPLE_GPU_TOOL_PATH=/usr/bin:/bin:/usr/sbin:/sbin

if [ "$CROSS_BUILD" -eq 1 ]; then
	OBJC_RUNTIME_LINK=", '-lobjc'"
	# Mesa and virglrenderer describe the target as Darwin with an iOS
	# subsystem, which is what makes KosmicKrisp compile its Objective-C
	# Metal bridge rather than the portable stubs. libepoxy keys its own
	# platform selection on system = 'ios' and keeps the other file.
	MESA_MACHINE_ARGS="--cross-file $VIRGL_CROSS --native-file $NATIVE_FILE"
else
	OBJC_RUNTIME_LINK=
	MESA_MACHINE_ARGS=
fi

cat > "$EPOXY_CROSS" <<EOF
[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-I$STAGE/include']
cpp_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-I$STAGE/include']
objc_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-I$STAGE/include']
objcpp_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-I$STAGE/include']
c_link_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-L$STAGE/lib'$OBJC_RUNTIME_LINK]
cpp_link_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-L$STAGE/lib'$OBJC_RUNTIME_LINK]
objc_link_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-L$STAGE/lib'$OBJC_RUNTIME_LINK]
objcpp_link_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-L$STAGE/lib'$OBJC_RUNTIME_LINK]

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
python = ['$PYTHON']

[host_machine]
system = '$EPOXY_SYSTEM'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
EOF

awk -v source_system="$EPOXY_SYSTEM" -v subsystem="$SUBSYSTEM" '
	BEGIN {
		quote = sprintf("%c", 39)
		expected = "system = " quote source_system quote
	}
	$0 == expected {
		print "system = " quote "darwin" quote
		print "kernel = " quote "xnu" quote
		print "subsystem = " quote subsystem quote
		next
	}
	{ print }
' "$EPOXY_CROSS" > "$VIRGL_CROSS"

# A cross build compiles helper tools for the build machine. Their compilers
# and sysroot come from this file; nothing may leak through the environment,
# because SDKROOT would point them at the target SDK as well.
cat > "$NATIVE_FILE" <<EOF
[binaries]
c = ['$NATIVE_CC']
cpp = ['$NATIVE_CXX']
objc = ['$NATIVE_CC']
objcpp = ['$NATIVE_CXX']
ar = ['$AR']
nm = ['$NM']
ranlib = ['$RANLIB']
strip = ['$STRIP', '-x']
pkg-config = ['$PKG_CONFIG']
python = ['$PYTHON']

[built-in options]
c_args = ['-arch', 'arm64', '-target', '$NATIVE_TARGET', '-isysroot', '$NATIVE_SDK_ROOT']
cpp_args = ['-arch', 'arm64', '-target', '$NATIVE_TARGET', '-isysroot', '$NATIVE_SDK_ROOT']
objc_args = ['-arch', 'arm64', '-target', '$NATIVE_TARGET', '-isysroot', '$NATIVE_SDK_ROOT']
objcpp_args = ['-arch', 'arm64', '-target', '$NATIVE_TARGET', '-isysroot', '$NATIVE_SDK_ROOT']
c_link_args = ['-arch', 'arm64', '-target', '$NATIVE_TARGET', '-isysroot', '$NATIVE_SDK_ROOT']
cpp_link_args = ['-arch', 'arm64', '-target', '$NATIVE_TARGET', '-isysroot', '$NATIVE_SDK_ROOT']
objc_link_args = ['-arch', 'arm64', '-target', '$NATIVE_TARGET', '-isysroot', '$NATIVE_SDK_ROOT']
objcpp_link_args = ['-arch', 'arm64', '-target', '$NATIVE_TARGET', '-isysroot', '$NATIVE_SDK_ROOT']
EOF
install_framework()
{
	framework_name=$1
	source_binary=$2
	bundle_identifier=$3
	framework_version=${4:-1.0}
	framework_dir="$STAGE/Frameworks/$framework_name.framework"
	mkdir -p -- "$framework_dir"
	install -m 0755 "$source_binary" "$framework_dir/$framework_name"
	"$INSTALL_NAME_TOOL" -id \
		"@rpath/$framework_name.framework/$framework_name" \
		"$framework_dir/$framework_name"
	cat > "$framework_dir/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>en</string>
	<key>CFBundleExecutable</key><string>$framework_name</string>
	<key>CFBundleIdentifier</key><string>$bundle_identifier</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>$framework_name</string>
	<key>CFBundlePackageType</key><string>FMWK</string>
	<key>CFBundleShortVersionString</key><string>$framework_version</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>$MINIMUM_OS_KEY</key><string>$DEPLOYMENT_TARGET</string>
</dict>
</plist>
EOF
	"$PLUTIL" -lint "$framework_dir/Info.plist" >/dev/null
}

# Mesa build steps. A native macOS build takes its sysroot from SDKROOT; a
# cross build must not export it, because the same variable would point the
# build machine compiler at the target SDK and meson's native compiler check
# then fails on binaries it cannot run. The cross sysroot lives in the meson
# machine files instead.
mesa_run()
{
	mesa_run_path=$1
	shift
	if [ "$CROSS_BUILD" -eq 0 ]; then
		env -i PATH="$mesa_run_path" TMPDIR="$APPLE_GPU_TEMP_DIR" \
			SDKROOT="$SDK_ROOT" \
			MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
			"$@"
	else
		env -i PATH="$mesa_run_path" TMPDIR="$APPLE_GPU_TEMP_DIR" "$@"
	fi
}

mesa_setup()
{
	mesa_setup_path=$1
	shift
	if [ "$CROSS_BUILD" -eq 0 ]; then
		env -i PATH="$mesa_setup_path" TMPDIR="$APPLE_GPU_TEMP_DIR" \
			SDKROOT="$SDK_ROOT" \
			MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
			CC="$CC" CXX="$CXX" OBJC="$CC" OBJCXX="$CXX" \
			PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$MESA_PKG_CONFIG" \
			PKG_CONFIG_SYSROOT_DIR= \
			"$@"
	else
		env -i PATH="$mesa_setup_path" TMPDIR="$APPLE_GPU_TEMP_DIR" \
			PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$MESA_PKG_CONFIG" \
			PKG_CONFIG_SYSROOT_DIR= \
			"$@"
	fi
}

MESA_BUILD="$BUILDS/Mesa"
MESA_PKG_CONFIG="$BUILDS/empty-mesa-pkgconfig"
mkdir -p -- "$MESA_PKG_CONFIG"
MESA_TOOL_PATH="$(dirname -- "$PYTHON"):$(dirname -- "$MESON"):$(dirname -- "$NINJA"):$APPLE_GPU_TOOL_PATH"
VENUS_PROTOCOL_GENERATED_SHA256=
if [ "$SOFTWARE" -eq 0 ]; then
	# The protocol producer runs out of tree against authenticated source, so
	# its Git banner records the real prepared topic rather than an outer repo.
	VENUS_PROTOCOL_BUILD="$BUILDS/venus-protocol"
	env -i PATH="$MESA_TOOL_PATH" TMPDIR="$APPLE_GPU_TEMP_DIR" \
		CC="$NATIVE_CC" SDKROOT="$NATIVE_SDK_ROOT" \
		CFLAGS="-isysroot $NATIVE_SDK_ROOT -target $NATIVE_TARGET" \
		MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
		PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$MESA_PKG_CONFIG" \
		"$MESON" setup "$VENUS_PROTOCOL_BUILD" "$VENUS_PROTOCOL_SOURCE" \
			--buildtype debugoptimized -Db_ndebug=false --wrap-mode=nodownload \
			-Dvulkan-headers-dir="$VULKAN_HEADERS_SOURCE"
	env -i PATH="$MESA_TOOL_PATH" TMPDIR="$APPLE_GPU_TEMP_DIR" \
		SDKROOT="$NATIVE_SDK_ROOT" MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
		"$MESON" compile -C "$VENUS_PROTOCOL_BUILD" -j "$JOBS"
	env -i PATH="$MESA_TOOL_PATH" TMPDIR="$APPLE_GPU_TEMP_DIR" \
		"$MESON" test -C "$VENUS_PROTOCOL_BUILD" --print-errorlogs
	VENUS_PROTOCOL_GENERATED_SHA256=$(
		CDPATH= cd -- "$VENUS_PROTOCOL_BUILD"
		shasum -a 256 vn_protocol_driver*.h vn_protocol_renderer*.h |
			shasum -a 256 | awk '{print $1}'
	)
fi
# vtn_bindgen2 and the drivers' precompiled shader compilers run on the build
# machine. A cross configuration cannot produce them -- they would have to link
# build machine targets against the host machine's vtn, nir and util, which
# meson refuses -- so a cross build installs them from a native pass first and
# then asks for them with -Dprecomp-compiler=system.
MESA_PRECOMP_ARGS=
if [ "$CROSS_BUILD" -eq 1 ] && [ "$SOFTWARE" -eq 0 ]; then
	MESA_TOOLS_BUILD="$BUILDS/Mesa-tools"
	MESA_TOOLS_STAGE="$BUILDS/Mesa-tools-stage"
	mkdir -p -- "$MESA_TOOLS_STAGE"
	env -i PATH="$MESA_TOOL_PATH" TMPDIR="$APPLE_GPU_TEMP_DIR" \
		SDKROOT="$NATIVE_SDK_ROOT" MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
		CC="$NATIVE_CC" CXX="$NATIVE_CXX" \
		OBJC="$NATIVE_CC" OBJCXX="$NATIVE_CXX" \
		PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$MESA_PKG_CONFIG" \
		PKG_CONFIG_SYSROOT_DIR= \
		"$MESON" setup "$MESA_TOOLS_BUILD" "$SOURCES/Mesa" \
			--prefix "$MESA_TOOLS_STAGE" --libdir lib \
			--buildtype release --wrap-mode=nodownload \
			-Dplatforms=macos -Dvulkan-drivers=kosmickrisp \
			-Dgallium-drivers= -Dopengl=false -Dzstd=disabled \
			-Dllvm=disabled -Dspirv-tools=disabled \
			-Dinstall-mesa-clc=true -Dinstall-precomp-compiler=true
	env -i PATH="$MESA_TOOL_PATH" TMPDIR="$APPLE_GPU_TEMP_DIR" \
		SDKROOT="$NATIVE_SDK_ROOT" MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
		"$MESON" compile -C "$MESA_TOOLS_BUILD" -j "$JOBS"
	env -i PATH="$MESA_TOOL_PATH" TMPDIR="$APPLE_GPU_TEMP_DIR" \
		SDKROOT="$NATIVE_SDK_ROOT" MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
		"$MESON" install --no-rebuild -C "$MESA_TOOLS_BUILD" > /dev/null
	for tool in vtn_bindgen2 kk_clc; do
		[ -x "$MESA_TOOLS_STAGE/bin/$tool" ] ||
			fail "the native Mesa pass did not install $tool"
	done
	MESA_TOOL_PATH="$MESA_TOOLS_STAGE/bin:$MESA_TOOL_PATH"
	MESA_PRECOMP_ARGS=-Dprecomp-compiler=system
fi

if [ "$SOFTWARE" -eq 0 ]; then
	mesa_setup "$MESA_TOOL_PATH" \
		"$MESON" setup "$MESA_BUILD" "$SOURCES/Mesa" \
			--prefix "$NO_HOST_PREFIX" --libdir lib --buildtype release \
			-Ddebug=true \
			--wrap-mode=nodownload --prefer-static \
			$MESA_MACHINE_ARGS $MESA_PRECOMP_ARGS \
			-Dplatforms=macos -Dvulkan-drivers=kosmickrisp \
			-Dgallium-drivers= -Dopengl=false -Dzstd=disabled \
			-Dllvm=disabled -Dspirv-tools=disabled
	mesa_run "$MESA_TOOL_PATH" "$MESON" compile -C "$MESA_BUILD" -j "$JOBS"
	KOSMICKRISP_BINARY="$MESA_BUILD/src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib"
	[ -f "$KOSMICKRISP_BINARY" ] ||
		fail "Mesa did not produce the pinned KosmicKrisp dynamic library"
	mkdir -p -- "$STAGE/include/vulkan" "$STAGE/include/vk_video"
	$RSYNC -a "$VULKAN_HEADERS_SOURCE/include/vulkan/" "$STAGE/include/vulkan/"
	$RSYNC -a "$VULKAN_HEADERS_SOURCE/include/vk_video/" "$STAGE/include/vk_video/"
	# The Venus renderer charges each device it creates for a guest to the host's
	# device-memory pool through KosmicKrisp's private device-creation structure.
	# The header is an interface between two libraries of this prefix, so it is
	# staged for the renderer's build and removed before the prefix is published.
	mkdir -p -- "$STAGE/include/kosmickrisp"
	install -m 0644 \
		"$SOURCES/Mesa/src/kosmickrisp/vulkan/kk_device_memory_pool.h" \
		"$STAGE/include/kosmickrisp/kk_device_memory_pool.h"
	# The Vulkan driver is packaged before the OpenGL pass so that Zink links
	# against the published framework and records its runtime install name.
	install_framework KosmicKrisp "$KOSMICKRISP_BINARY" \
		org.linux-um-darwin.KosmicKrisp "$MESA_VERSION"
	KOSMICKRISP_FRAMEWORK_BINARY="$STAGE/Frameworks/KosmicKrisp.framework/KosmicKrisp"
fi

# Mesa's OpenGL stack is a second configuration of the same authenticated
# source: Zink renders desktop OpenGL 4.6 core on the packaged Vulkan
# driver, and libEGL exposes it through the surfaceless platform. The
# software variant renders on the CPU instead: llvmpipe JITs through LLVM,
# which Mesa links statically, so the prefix carries no LLVM file, and softpipe
# interprets without one.
if [ "$SOFTWARE" -eq 0 ]; then
	MESA_GL_DRIVER_ARGS="-Dgallium-drivers=zink -Dzink-vulkan-library-file=$STAGE/Frameworks/KosmicKrisp.framework/KosmicKrisp"
	MESA_LLVM_ARGS="-Dllvm=disabled"
else
	LLVM_PREFIX="$TEMP_ROOT/llvm"
	"$ROOT_DIR/dependencies/build-apple-gpu-llvm.sh" \
		--archive "$LLVM_ARCHIVE" --prefix "$LLVM_PREFIX" \
		--work-root "$TEMP_ROOT/llvm-work" --jobs "$JOBS"
	# The cross file names the llvm-config that describes the Simulator
	# libraries; the helper installed the one built for this machine.
	awk -v llvm_config="$LLVM_PREFIX/bin/llvm-config" '
		BEGIN { quote = sprintf("%c", 39) }
		{ print }
		/^python = / {
			print "llvm-config = [" quote llvm_config quote "]"
		}
	' "$VIRGL_CROSS" > "$VIRGL_CROSS.llvm"
	mv -- "$VIRGL_CROSS.llvm" "$VIRGL_CROSS"
	MESA_GL_DRIVER_ARGS="-Dgallium-drivers=$MESA_SOFTWARE_DRIVERS"
	MESA_LLVM_ARGS="-Dllvm=enabled -Dshared-llvm=disabled -Ddraw-use-llvm=true"
fi
MESA_GL_BUILD="$BUILDS/Mesa-gl"
# Mesa compiles the directories of its driver configuration (DATADIR and
# SYSCONFDIR, where it looks for drirc) in from its prefix. The libraries are
# built for the neutral prefix and installed into an image, which is where the
# frameworks are taken from. libEGL names libgallium by its install name below
# the neutral prefix, which is replaced with the framework's below.
MESA_GL_IMAGE="$BUILDS/Mesa-gl-image"
MESA_GL_STAGE="$MESA_GL_IMAGE$NO_HOST_PREFIX"
mkdir -p -- "$MESA_GL_IMAGE"
MESA_GL_TOOL_PATH="$MESA_TOOL_PATH"
# Mesa's GLSL grammars use two Bison 3 declarations. Apple's toolchain
# ships Bison 2, so an older generator is driven through a translation
# shim rather than by forking the grammars.
BISON_VERSION=$("$BISON" --version 2>/dev/null | sed -n '1s/.*) //p')
case $BISON_VERSION in
[3-9].*|[1-9][0-9].*) ;;
*)
	BISON_SHIM_DIR="$BUILDS/bison-shim"
	mkdir -p -- "$BISON_SHIM_DIR"
	sed -e "s|@BISON@|$BISON|g" \
		"$ROOT_DIR/dependencies/bison2-grammar-shim.sh" \
		> "$BISON_SHIM_DIR/bison"
	chmod 0755 "$BISON_SHIM_DIR/bison"
	MESA_GL_TOOL_PATH="$BISON_SHIM_DIR:$MESA_TOOL_PATH"
	;;
esac
mesa_setup "$MESA_GL_TOOL_PATH" \
	"$MESON" setup "$MESA_GL_BUILD" "$SOURCES/Mesa" \
		--prefix "$NO_HOST_PREFIX" --libdir lib --buildtype release \
		-Ddebug=true \
		--wrap-mode=nodownload \
		$MESA_MACHINE_ARGS $MESA_PRECOMP_ARGS \
		-Dplatforms=macos -Dvulkan-drivers= \
		$MESA_GL_DRIVER_ARGS -Dopengl=true -Degl=enabled \
		-Dglx=disabled -Dgles1=disabled -Dgles2=disabled \
		-Dzstd=disabled $MESA_LLVM_ARGS -Dspirv-tools=disabled
mesa_run "$MESA_GL_TOOL_PATH" "$MESON" compile -C "$MESA_GL_BUILD" -j "$JOBS"
mesa_run "$MESA_GL_TOOL_PATH" env DESTDIR="$MESA_GL_IMAGE" \
	"$MESON" install --no-rebuild -C "$MESA_GL_BUILD" > /dev/null
MESA_EGL_BINARY="$MESA_GL_STAGE/lib/libEGL.1.dylib"
MESA_GALLIUM_BINARY="$MESA_GL_STAGE/lib/libgallium-$MESA_VERSION.dylib"
[ -f "$MESA_EGL_BINARY" ] && [ -f "$MESA_GALLIUM_BINARY" ] ||
	fail "Mesa did not produce the packaged OpenGL libraries"
# libEGL records libgallium by the install name of the neutral prefix.
MESA_GALLIUM_INSTALL_NAME="$NO_HOST_PREFIX/lib/libgallium-$MESA_VERSION.dylib"
"$OTOOL" -L "$MESA_EGL_BINARY" | grep -q -F "$MESA_GALLIUM_INSTALL_NAME" ||
	fail "libEGL does not name $MESA_GALLIUM_INSTALL_NAME"
install_framework MesaGallium "$MESA_GALLIUM_BINARY" \
	org.linux-um-darwin.mesa.Gallium "$MESA_VERSION"
install_framework MesaEGL "$MESA_EGL_BINARY" \
	org.linux-um-darwin.mesa.EGL "$MESA_VERSION"
"$INSTALL_NAME_TOOL" -change \
	"$MESA_GALLIUM_INSTALL_NAME" "@rpath/MesaGallium.framework/MesaGallium" \
	"$STAGE/Frameworks/MesaEGL.framework/MesaEGL"
for headers in EGL GL KHR; do
	mkdir -p -- "$STAGE/include/$headers"
	$RSYNC -a "$MESA_GL_STAGE/include/$headers/" \
		"$STAGE/include/$headers/"
done

PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$STAGE/lib/pkgconfig" \
PKG_CONFIG_SYSROOT_DIR= \
	"$MESON" setup "$BUILDS/libepoxy" "$SOURCES/libepoxy" \
	--prefix "$STAGE" --buildtype release --cross-file "$EPOXY_CROSS" \
	--wrap-mode=nodownload \
	-Ddefault_library=static -Dtests=false -Dglx=no -Degl=yes -Dx11=false
"$MESON" compile -C "$BUILDS/libepoxy" -j "$JOBS"
"$MESON" install -C "$BUILDS/libepoxy"

# The renderer's Metal texture lookup includes Zink's interop description. A
# build without Zink has no such header installed, so it is staged from the
# authenticated Mesa source for the renderer's build and removed afterwards.
# A software rasterizer offers no Metal interop, so the lookup finds no
# texture and the scanout is read back.
if [ "$SOFTWARE" -eq 1 ]; then
	install -m 0644 \
		"$SOURCES/Mesa/src/gallium/drivers/zink/zink_interop_public.h" \
		"$STAGE/include/GL/zink_interop_public.h"
fi
set -- \
	-Ddefault_library=static -Dtests=false \
	-Dcheck-gl-errors=false -Dvideo=false \
	-Dplatforms=egl
if [ "$SOFTWARE" -eq 0 ]; then
	set -- "$@" \
		-Drender-server-mode=thread \
		-Dvenus=true -Dvulkan-dload=false -Dvulkan-preload=false \
		-Dvenus-protocol-dir="$VENUS_PROTOCOL_BUILD" \
		-Dvulkan-include-dir="$VULKAN_HEADERS_SOURCE/include" \
		-Dvulkan-library-file="$KOSMICKRISP_BINARY"
else
	set -- "$@" -Dvenus=false
fi
# The renderer compiles the path of its render server executable in from its
# prefix. The server runs as a thread of the host here and is never executed,
# so the path is only the name it is called by: build for the neutral prefix.
PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$STAGE/lib/pkgconfig" \
PKG_CONFIG_SYSROOT_DIR= \
	"$MESON" setup "$BUILDS/virglrenderer" "$SOURCES/virglrenderer" \
	--prefix "$NO_HOST_PREFIX" --buildtype release --cross-file "$VIRGL_CROSS" \
	--wrap-mode=nodownload "$@"
"$MESON" compile -C "$BUILDS/virglrenderer" -j "$JOBS"
VIRGL_IMAGE="$BUILDS/virglrenderer-image"
DESTDIR="$VIRGL_IMAGE" "$MESON" install -C "$BUILDS/virglrenderer"
merge_neutral_image "$VIRGL_IMAGE" "$STAGE" "$STAGE"
install -m 0644 "$SOURCES/virglrenderer/src/virgl_hw.h" \
	"$STAGE/include/virgl/virgl_hw.h"

# CMake imports exact archives and headers. Build-only pkg-config metadata
# contains the exclusive staging path and must not enter the published prefix,
# upstream's vtest development server is not part of the product payload, and
# the renderer's driver interface is not an API of the prefix.
rm -rf -- "$STAGE/lib/pkgconfig" "$STAGE/bin" "$STAGE/include/kosmickrisp"
if [ "$SOFTWARE" -eq 1 ]; then
	rm -f -- "$STAGE/include/GL/zink_interop_public.h"
fi


if [ "$SOFTWARE" -eq 0 ]; then
	KOSMICKRISP_FRAMEWORK="$STAGE/Frameworks/KosmicKrisp.framework"
	mkdir -p -- "$KOSMICKRISP_FRAMEWORK/Headers/vulkan" \
		"$KOSMICKRISP_FRAMEWORK/Headers/vk_video" \
		"$KOSMICKRISP_FRAMEWORK/Modules"
	$RSYNC -a "$STAGE/include/vulkan/" \
		"$KOSMICKRISP_FRAMEWORK/Headers/vulkan/"
	$RSYNC -a "$STAGE/include/vk_video/" \
		"$KOSMICKRISP_FRAMEWORK/Headers/vk_video/"
	cat > "$KOSMICKRISP_FRAMEWORK/Headers/KosmicKrisp.h" <<'EOF'
#ifndef UML_KOSMICKRISP_FRAMEWORK_H
#define UML_KOSMICKRISP_FRAMEWORK_H
#include <vulkan/vulkan.h>
#endif
EOF
	cat > "$KOSMICKRISP_FRAMEWORK/Modules/module.modulemap" <<'EOF'
framework module KosmicKrisp {
    umbrella header "KosmicKrisp.h"
    export *
    module * { export * }
}
EOF
fi

mkdir -p "$STAGE/share/uml-dependencies"
cp "$MANIFEST" "$STAGE/share/uml-dependencies/manifest.tsv"
# Extract symbols while Mesa's objects still exist: the temporary build tree
# is removed after publication. Keep Release optimization and assertions as
# configured above; debug information is a separate compiler option.
mkdir -p -- "$STAGE/dSYMs"
for framework in "$STAGE/Frameworks/"*.framework; do
	framework_name=$(basename -- "$framework")
	"$XCRUN" dsymutil "$framework/${framework_name%.framework}" \
		-o "$STAGE/dSYMs/$framework_name.dSYM"
done

{
	printf 'source\ttree\n'
	for source in libepoxy virglrenderer mesa venus-protocol; do
		printf '%s\t%s\n' "$source" \
			"$(git -C "$SOURCE_ROOT/$source" rev-parse 'HEAD^{tree}')"
	done
} > "$STAGE/share/uml-dependencies/source-trees.tsv"
SDK_VERSION=$($XCRUN --sdk "$SDK_NAME" --show-sdk-version)
COMPILER_VERSION=$("$CC" --version | sed -n '1p')
BUILD_TIMESTAMP=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
{
	printf '%s\n' 'field	value'
	printf 'format\t2\n'
	printf 'product\t%s\n' "$PRODUCT"
	printf 'platform\t%s\n' "$PLATFORM"
	printf 'sdk_name\t%s\n' "$SDK_NAME"
	printf 'sdk_version\t%s\n' "$SDK_VERSION"
	printf 'sdk_path\t%s\n' "$SDK_ROOT"
	printf 'compiler\t%s\n' "$COMPILER_VERSION"
	printf 'built_at_utc\t%s\n' "$BUILD_TIMESTAMP"
	if [ "$SOFTWARE" -eq 0 ]; then
		printf 'venus_protocol_generated_sha256\t%s\n' \
			"$VENUS_PROTOCOL_GENERATED_SHA256"
	fi
} > "$STAGE/share/uml-dependencies/build-receipt.tsv"
cp "$PLATFORM_CONTRACT" \
	"$STAGE/share/uml-dependencies/$CONTRACT_NAME"
if [ "$SOFTWARE" -eq 1 ]; then
	# The JIT's source identity: the prefix carries no LLVM file, only code
	# built from this archive.
	cp "$LLVM_ARCHIVE_MANIFEST" \
		"$STAGE/share/uml-dependencies/llvm-archives.tsv"
fi
publish_verified "$STAGE" "$PREFIX" verify_prefix \
	"$PLATFORM Apple GPU dependency prefix"
