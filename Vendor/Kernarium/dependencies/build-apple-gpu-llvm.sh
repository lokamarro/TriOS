#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the LLVM libraries the iOS Simulator's llvmpipe links, from the
# authenticated source archive of dependencies/llvm/archives.tsv. Only
# dependencies/build-apple-gpu.sh runs it, for the ios-simulator platform:
# Mesa's llvmpipe is a JIT over LLVM's AArch64 back end, and it links these
# static libraries into MesaGallium, so the published GPU prefix ships no LLVM
# file. The macOS and iOS device GPU builds never call it.
#
# Two passes of the same source and options. The first builds the three tools
# the cross build must run on this machine -- llvm-tblgen, llvm-min-tblgen and
# llvm-config -- for macOS. The second builds the libraries for the iOS
# Simulator and installs the first pass's llvm-config beside them: it
# resolves every path from its own location, so it describes the Simulator
# libraries to Mesa's meson, which cannot run one built for the Simulator.
#
# Usage: dependencies/build-apple-gpu-llvm.sh --archive PATH --prefix PATH
#            --work-root PATH [--jobs NUMBER]
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ARCHIVE=
PREFIX=
WORK_ROOT=
JOBS=8
# Every Apple product, and the build machine's own pass, builds at this floor.
DEPLOYMENT_TARGET=27.0

fail()
{
	printf '%s\n' "dependencies/build-apple-gpu-llvm.sh: $*" >&2
	exit 1
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--archive)
		[ "$#" -ge 2 ] || fail "--archive requires a path"
		ARCHIVE=$2
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
	*) fail "unknown option: $1" ;;
	esac
done
[ -f "$ARCHIVE" ] || fail "--archive must name the source archive"
[ -n "$PREFIX" ] && [ -n "$WORK_ROOT" ] ||
	fail "--prefix and --work-root are required"
[ ! -e "$PREFIX" ] || fail "the output prefix already exists: $PREFIX"
case "$JOBS" in
''|*[!0-9]*) fail "jobs must be a positive integer" ;;
esac
[ "$JOBS" -gt 0 ] || fail "jobs must be a positive integer"

. "$ROOT_DIR/dependencies/build-common.sh"
find_tool CMAKE cmake
find_tool NINJA ninja
find_tool XCRUN xcrun

# The archive names its version and its one top-level directory; the cache
# stores it as SHA256-NAME.
TOP=$(basename -- "$ARCHIVE")
case "$TOP" in
[0-9a-f]*-llvm-project-*) TOP=${TOP#*-} ;;
esac
TOP=${TOP%.tar.xz}
case "$TOP" in
llvm-project-*.src) ;;
*) fail "unexpected archive name: $(basename -- "$ARCHIVE")" ;;
esac
VERSION=${TOP#llvm-project-}
VERSION=${VERSION%.src}

NATIVE_SDK=$($XCRUN --sdk macosx --show-sdk-path)
SIMULATOR_SDK=$($XCRUN --sdk iphonesimulator --show-sdk-path)
[ -d "$NATIVE_SDK" ] && [ -d "$SIMULATOR_SDK" ] ||
	fail "xcrun returned an invalid SDK"
CC=$($XCRUN --sdk macosx --find clang)
CXX=$($XCRUN --sdk macosx --find clang++)

mkdir -p -- "$WORK_ROOT" "$(dirname -- "$PREFIX")"
WORK_ROOT=$(CDPATH= cd -- "$WORK_ROOT" && pwd -P)
PREFIX="$(CDPATH= cd -- "$(dirname -- "$PREFIX")" && pwd -P)/$(basename -- "$PREFIX")"
SOURCE="$WORK_ROOT/llvm-source"
rm -rf -- "$SOURCE"
mkdir -p -- "$SOURCE"
# Only the core libraries are built, so the other projects stay in the archive.
/usr/bin/tar -xJf "$ARCHIVE" -C "$SOURCE" --strip-components=1 \
	"$TOP/llvm" "$TOP/cmake" "$TOP/third-party"
[ -f "$SOURCE/llvm/CMakeLists.txt" ] ||
	fail "the archive holds no llvm/CMakeLists.txt"

# No ambient prefix may feed either pass: the search variables are cleared
# for every command, nothing under Homebrew or /usr/local is considered, and
# the build tools are named rather than found on a search path.
llvm_cmake()
{
	llvm_log="$WORK_ROOT/llvm-$LLVM_STEP.log"
	if ! env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin TMPDIR="${TMPDIR:-/tmp}" \
		"$CMAKE" "$@" >"$llvm_log" 2>&1; then
		tail -n 40 "$llvm_log" >&2
		fail "LLVM $LLVM_STEP failed; the full log is $llvm_log"
	fi
}
set -- -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_MAKE_PROGRAM="$NINJA" -DCMAKE_ASM_COMPILER="$CC" \
	-DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
	"-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local" \
	-DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=OFF \
	-DCMAKE_OSX_ARCHITECTURES=arm64 \
	-DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
	-DLLVM_TARGETS_TO_BUILD=AArch64 \
	-DLLVM_ENABLE_PROJECTS= -DLLVM_ENABLE_RUNTIMES= \
	-DLLVM_ENABLE_RTTI=ON -DLLVM_ENABLE_ASSERTIONS=OFF \
	-DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
	-DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_FFI=OFF \
	-DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_HTTPLIB=OFF \
	-DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_OCAMLDOC=OFF \
	-DLLVM_ENABLE_Z3_SOLVER=OFF \
	-DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
	-DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
	-DLLVM_BUILD_LLVM_DYLIB=OFF -DLLVM_LINK_LLVM_DYLIB=OFF \
	-DBUILD_SHARED_LIBS=OFF -DLLVM_ENABLE_PIC=ON

NATIVE_BUILD="$WORK_ROOT/llvm-native"
SIMULATOR_BUILD="$WORK_ROOT/llvm-simulator"
LLVM_STEP=native-configure
llvm_cmake -S "$SOURCE/llvm" -B "$NATIVE_BUILD" "$@" \
	-DCMAKE_OSX_SYSROOT="$NATIVE_SDK"
LLVM_STEP=native-build
llvm_cmake --build "$NATIVE_BUILD" -j "$JOBS" \
	--target llvm-tblgen llvm-min-tblgen llvm-config

# The Simulator libraries. LLVM links its shared tool libraries (remarks, LTO,
# the C API dylib) with ELF linker flags when the system is named iOS, and
# ld rejects them, so those targets are off; the C++ libraries Mesa links are
# static and unaffected.
LLVM_STEP=simulator-configure
llvm_cmake -S "$SOURCE/llvm" -B "$SIMULATOR_BUILD" "$@" \
	-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$SIMULATOR_SDK" \
	-DLLVM_HOST_TRIPLE="arm64-apple-ios$DEPLOYMENT_TARGET-simulator" \
	-DLLVM_DEFAULT_TARGET_TRIPLE="arm64-apple-ios$DEPLOYMENT_TARGET-simulator" \
	-DLLVM_TARGET_ARCH=AArch64 \
	-DLLVM_TABLEGEN="$NATIVE_BUILD/bin/llvm-tblgen" \
	-DLLVM_NATIVE_TOOL_DIR="$NATIVE_BUILD/bin" \
	-DLLVM_CONFIG_PATH="$NATIVE_BUILD/bin/llvm-config" \
	-DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF -DLLVM_INSTALL_UTILS=OFF \
	-DLLVM_TOOL_REMARKS_SHLIB_BUILD=OFF -DLLVM_TOOL_LTO_BUILD=OFF \
	-DLLVM_BUILD_LLVM_C_DYLIB=OFF -DLLVM_ENABLE_LTO=OFF \
	-DCMAKE_INSTALL_PREFIX="$PREFIX"
LLVM_STEP=simulator-build
llvm_cmake --build "$SIMULATOR_BUILD" -j "$JOBS" \
	--target install-llvm-libraries install-llvm-headers \
		install-cmake-exports
mkdir -p -- "$PREFIX/bin"
install -m 0755 "$NATIVE_BUILD/bin/llvm-config" "$PREFIX/bin/llvm-config"

# The installed llvm-config must describe the Simulator libraries Mesa links.
CONFIG="$PREFIX/bin/llvm-config"
[ "$("$CONFIG" --version)" = "$VERSION" ] ||
	fail "llvm-config does not report LLVM $VERSION"
[ "$("$CONFIG" --targets-built)" = AArch64 ] ||
	fail "llvm-config does not report an AArch64-only build"
[ "$("$CONFIG" --shared-mode)" = static ] ||
	fail "llvm-config does not report a static build"
[ "$("$CONFIG" --libdir)" = "$PREFIX/lib" ] ||
	fail "llvm-config does not describe the installed libraries"
[ -f "$PREFIX/lib/libLLVMCore.a" ] || fail "LLVM installed no libraries"
printf '%s\n' "built LLVM $VERSION for the iOS Simulator: $PREFIX"
