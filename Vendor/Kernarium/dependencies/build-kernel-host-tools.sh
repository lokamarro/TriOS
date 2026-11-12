#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the host tools the Darwin kernel build needs to generate BTF: pahole,
# plus the static libelf that Kbuild links into resolve_btfids. The script is
# offline and consumes only sources and archives authenticated by
# dependencies/prepare.sh; host build tools and Apple SDK libraries are the only
# other inputs.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE_ROOT=${UML_DEPENDENCY_SOURCE_ROOT:-"$ROOT_DIR/external/sources"}
CACHE_ROOT=${UML_DEPENDENCY_CACHE:-"$ROOT_DIR/.build/dependency-cache"}
OUTPUT=${UML_KERNEL_HOST_TOOLS_PREFIX:-"$ROOT_DIR/.build/dependencies/kernel-host-tools"}
WORK_ROOT=${UML_KERNEL_HOST_TOOLS_BUILD_ROOT:-"$ROOT_DIR/.build/dependency-build/kernel-host-tools"}
ARCHIVE_MANIFEST="$ROOT_DIR/dependencies/kernel-host-tools/archives.tsv"
VERIFY_SCRIPT="$ROOT_DIR/dependencies/cmake/VerifyKernelHostToolsPrefix.cmake"
DARWIN_HOST="$ROOT_DIR/kernel/mainline-6.18/build/toolchain/darwin-host"
UAPI_ARCHIVE_NAME=linux-libc-dev_6.12.113-1_all.deb
JOBS=${UML_KERNEL_HOST_TOOLS_JOBS:-8}
# The tools run only on the kernel build host. elfutils calls strchrnul, which
# Darwin provides from macOS 15.4.
DEPLOYMENT_TARGET=15.4
VERIFY_ONLY=0

fail()
{
	printf '%s\n' "dependencies/build-kernel-host-tools.sh: $*" >&2
	exit 1
}

usage()
{
	printf '%s\n' \
		"Usage: dependencies/build-kernel-host-tools.sh [options]" \
		"" \
		"Options:" \
		"  --source-root PATH  Verified dependency source trees" \
		"  --cache PATH        Dependency cache holding the pinned archives" \
		"  --output PATH       Output prefix" \
		"  --work-root PATH    Temporary build parent" \
		"  --jobs NUMBER       Parallel build jobs (default: 8)" \
		"  --verify-only       Verify an existing prefix" \
		"  -h, --help          Show this help" \
		"" \
		"Prepare the inputs first:" \
		"  dependencies/prepare.sh --product kernel-host-tools" \
		"  dependencies/prepare.sh --archive-manifest dependencies/kernel-host-tools/archives.tsv"
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--source-root)
		[ "$#" -ge 2 ] || fail "--source-root requires a path"
		SOURCE_ROOT=$2
		shift 2
		;;
	--cache)
		[ "$#" -ge 2 ] || fail "--cache requires a path"
		CACHE_ROOT=$2
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
[ "$(uname -s)" = Darwin ] || fail "the kernel host tools are built on macOS"
case "$(uname -m)" in
arm64|aarch64) ;;
*) fail "the kernel host tools require an arm64 host" ;;
esac

. "$ROOT_DIR/dependencies/build-common.sh"

find_tool CMAKE cmake
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
	printf '%s\n' "verified kernel host tools: $OUTPUT"
	exit 0
fi

if [ -e "$OUTPUT" ] || [ -L "$OUTPUT" ]; then
	verify_output "$OUTPUT" ||
		fail "existing output is invalid and was left untouched: $OUTPUT"
	printf '%s\n' "reusing verified kernel host tools: $OUTPUT"
	exit 0
fi

find_tool MAKE gmake
find_tool MESON meson
find_tool NINJA ninja
find_tool AUTORECONF autoreconf
find_tool BISON bison
find_tool FLEX flex
find_tool M4 m4
find_tool RSYNC rsync
find_tool TAR tar
find_tool PYTHON python3

"$ROOT_DIR/dependencies/prepare.sh" --product kernel-host-tools \
	--root "$SOURCE_ROOT" --verify
"$ROOT_DIR/dependencies/prepare.sh" --archive-manifest "$ARCHIVE_MANIFEST" \
	--cache "$CACHE_ROOT" --verify
# Steps below run in other directories, so the verified roots become absolute.
SOURCE_ROOT=$(CDPATH= cd -- "$SOURCE_ROOT" && pwd -P)
CACHE_ROOT=$(CDPATH= cd -- "$CACHE_ROOT" && pwd -P)
UAPI_ARCHIVE=$(awk -F '\t' -v name="$UAPI_ARCHIVE_NAME" \
	'$1 == name { print $3 "-" $1 }' "$ARCHIVE_MANIFEST")
[ -n "$UAPI_ARCHIVE" ] || fail "$UAPI_ARCHIVE_NAME is absent from $ARCHIVE_MANIFEST"
UAPI_ARCHIVE="$CACHE_ROOT/archives/$UAPI_ARCHIVE"

SDK_ROOT=$("$XCRUN" --sdk macosx --show-sdk-path)
CC=$("$XCRUN" --sdk macosx --find clang)
AR=$("$XCRUN" --sdk macosx --find ar)
RANLIB=$("$XCRUN" --sdk macosx --find ranlib)

mkdir -p -- "$WORK_ROOT" "$OUTPUT_PARENT"
WORK_ROOT=$(CDPATH= cd -- "$WORK_ROOT" && pwd -P)
OUTPUT_PARENT=$(CDPATH= cd -- "$OUTPUT_PARENT" && pwd -P)
OUTPUT="$OUTPUT_PARENT/$(basename -- "$OUTPUT")"
TEMP_ROOT=$(mktemp -d "$WORK_ROOT/source-build.XXXXXX")
STAGE=$(mktemp -d "$OUTPUT_PARENT/.kernel-host-tools.XXXXXX")
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

# Build-only tools and libraries live below the temporary root. The published
# prefix receives pahole and the libelf that Kbuild links into resolve_btfids.
BOOTSTRAP="$TEMP_ROOT/bootstrap"
PRIVATE="$TEMP_ROOT/private"
UAPI_INCLUDE="$TEMP_ROOT/linux-uapi/include"
mkdir -p -- "$BOOTSTRAP" "$PRIVATE/include" "$PRIVATE/lib" "$UAPI_INCLUDE" \
	"$STAGE/bin"

# Keep package managers, user caches, and ambient search paths out of every
# configure step. Only explicitly passed prefixes are consulted.
private_env()
{
	env \
		PATH="$BOOTSTRAP/bin:$PATH" \
		CC="$CC" AR="$AR" RANLIB="$RANLIB" \
		PKG_CONFIG_PATH= \
		PKG_CONFIG_LIBDIR="$PRIVATE/lib/pkgconfig" \
		CMAKE_PREFIX_PATH= CMAKE_INCLUDE_PATH= CMAKE_LIBRARY_PATH= \
		CMAKE_FRAMEWORK_PATH= CMAKE_APPBUNDLE_PATH= \
		CPATH= C_INCLUDE_PATH= LIBRARY_PATH= \
		ACLOCAL_PATH= CONFIG_SITE= \
		MAKEINFO=true \
		SDKROOT="$SDK_ROOT" \
		MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
		"$@"
}

# Each step logs to a directory of this run's own below the work root. It
# survives a failed build and is removed once the output is published.
LOG_DIR=$(mktemp -d "$WORK_ROOT/logs.XXXXXX")
logged()
{
	step=$1
	shift
	if ! "$@" >>"$LOG_DIR/$step.log" 2>&1; then
		tail -n 40 "$LOG_DIR/$step.log" >&2
		fail "$step failed; see $LOG_DIR/$step.log"
	fi
}

copy_source()
{
	mkdir -p -- "$2"
	"$RSYNC" -a --exclude=.git "$1/" "$2/"
}

# gawk generates elfutils' DWARF name tables. It is a build-only tool. Its
# generated autotools files are committed, but a checkout that crosses a second
# boundary leaves aclocal.m4 older than the m4 files it was generated from, and
# make then runs the aclocal-1.16 this host does not have. Give the copy one
# mtime so that checkout order never reads as staleness.
copy_source "$SOURCE_ROOT/gawk" "$TEMP_ROOT/src/gawk"
find "$TEMP_ROOT/src/gawk" -exec touch -r "$TEMP_ROOT/src/gawk/configure" {} +
(
	CDPATH= cd -- "$TEMP_ROOT/src/gawk"
	logged gawk private_env ./configure --prefix="$BOOTSTRAP" --disable-nls \
		--disable-extensions --disable-pma
	logged gawk private_env "$MAKE" -j "$JOBS"
	logged gawk private_env "$MAKE" install
)
[ -x "$BOOTSTRAP/bin/gawk" ] || fail "gawk was not built"

logged argp-standalone private_env "$MESON" setup "$TEMP_ROOT/build/argp" \
	"$SOURCE_ROOT/argp-standalone" --prefix="$PRIVATE" \
	--default-library=static -Dbuildtype=release
logged argp-standalone private_env "$NINJA" -C "$TEMP_ROOT/build/argp" install

copy_source "$SOURCE_ROOT/musl-obstack" "$TEMP_ROOT/src/musl-obstack"
(
	CDPATH= cd -- "$TEMP_ROOT/src/musl-obstack"
	logged musl-obstack private_env "$AUTORECONF" -fi
	logged musl-obstack private_env ./configure --prefix="$PRIVATE" --disable-shared \
		--enable-static
	logged musl-obstack private_env "$MAKE" -j "$JOBS" install
)

# The standalone libbpf that pahole embeds includes Linux UAPI headers. Take
# them from the pinned Debian linux-libc-dev package, with the arm64 asm tree.
(
	CDPATH= cd -- "$TEMP_ROOT/linux-uapi"
	"$TAR" -xf "$UAPI_ARCHIVE" data.tar.xz
	"$TAR" -xf data.tar.xz ./usr/include/linux ./usr/include/asm-generic \
		./usr/lib/linux/uapi/arm64/asm
)
# The multiarch asm directories hold symlinks into usr/lib/linux/uapi.
mv "$TEMP_ROOT/linux-uapi/usr/include/linux" "$UAPI_INCLUDE/linux"
mv "$TEMP_ROOT/linux-uapi/usr/include/asm-generic" "$UAPI_INCLUDE/asm-generic"
mv "$TEMP_ROOT/linux-uapi/usr/lib/linux/uapi/arm64/asm" "$UAPI_INCLUDE/asm"

# elfutils: maintainer mode regenerates the DWARF name table and libcpu's
# x86 decoder tables, which Git carries only as generators. The cache values
# replace probes that link with GNU-ld-only options; Apple clang supports
# __thread, and Mach-O has no build-id, -z defs, or -z relro.
copy_source "$SOURCE_ROOT/elfutils" "$TEMP_ROOT/src/elfutils"
(
	CDPATH= cd -- "$TEMP_ROOT/src/elfutils"
	logged elfutils private_env "$AUTORECONF" -fi
)
mkdir -p -- "$TEMP_ROOT/build/elfutils"
(
	CDPATH= cd -- "$TEMP_ROOT/build/elfutils"
	logged elfutils private_env "$TEMP_ROOT/src/elfutils/configure" \
		--prefix="$PRIVATE" \
		--enable-maintainer-mode \
		--enable-install-elfh \
		--disable-nls \
		--disable-debuginfod \
		--disable-libdebuginfod \
		--disable-demangler \
		--disable-symbol-versioning \
		--disable-textrelcheck \
		--without-bzlib --without-lzma --without-zstd \
		CPPFLAGS="-I$DARWIN_HOST/include -I$PRIVATE/include" \
		CFLAGS="-O2 -g" \
		LDFLAGS="-L$PRIVATE/lib" \
		BISON="$BISON" FLEX="$FLEX" M4="$M4" \
		ac_cv_tls=yes ac_cv_zdefs=no ac_cv_zrelro=no ac_cv_buildid=yes
	# Upstream's install target also links the shared objects, which use
	# GNU-ld-only options. Build the static archives in dependency order and
	# install their headers and archives directly.
	for archive in lib/libeu.a libelf/libelf.a libcpu/libcpu.a \
		backends/libebl_backends.a libebl/libebl.a libdwelf/libdwelf.a \
		libdwfl/libdwfl.a libdwfl_stacktrace/libdwfl_stacktrace.a \
		libdw/libdw.a; do
		logged elfutils private_env "$MAKE" -C "$(dirname -- "$archive")" -j "$JOBS" \
			"$(basename -- "$archive")"
	done
	logged elfutils private_env "$MAKE" -C libelf install-libLIBRARIES \
		install-includeHEADERS
	logged elfutils private_env "$MAKE" -C libdw install-libLIBRARIES \
		install-includeHEADERS install-pkgincludeHEADERS
	for directory in libdwfl libdwelf; do
		logged elfutils private_env "$MAKE" -C "$directory" install-pkgincludeHEADERS
	done
	cp version.h "$PRIVATE/include/elfutils/version.h"
)
"$AR" t "$PRIVATE/lib/libdw.a" | grep -q '^dwfl_begin\.o$' ||
	fail "libdw.a does not contain libdwfl"
"$AR" t "$PRIVATE/lib/libdw.a" | grep -q '^i386_disasm\.o$' ||
	fail "libdw.a does not contain libcpu"

# pahole embeds libbpf as its lib/bpf submodule. Build only the pahole target.
copy_source "$SOURCE_ROOT/dwarves" "$TEMP_ROOT/src/dwarves"
copy_source "$SOURCE_ROOT/libbpf" "$TEMP_ROOT/src/dwarves/lib/bpf"
TOOL_CFLAGS="-include $DARWIN_HOST/host_compat.h -I$DARWIN_HOST/include -I$PRIVATE/include -idirafter $UAPI_INCLUDE"
logged dwarves private_env "$CMAKE" -S "$TEMP_ROOT/src/dwarves" -B "$TEMP_ROOT/build/dwarves" \
	-G Ninja \
	-DCMAKE_MAKE_PROGRAM="$NINJA" \
	-DCMAKE_C_COMPILER="$CC" \
	-DCMAKE_OSX_SYSROOT="$SDK_ROOT" \
	-DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_FLAGS="$TOOL_CFLAGS" \
	-DCMAKE_FIND_USE_CMAKE_SYSTEM_PATH=OFF \
	-DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=OFF \
	-DBUILD_SHARED_LIBS=OFF \
	-DLIBBPF_EMBEDDED=ON \
	-DDWARF_INCLUDE_DIR="$PRIVATE/include" \
	-DLIBDW_INCLUDE_DIR="$PRIVATE/include" \
	-DDWARF_LIBRARY="$PRIVATE/lib/libdw.a" \
	-DELF_LIBRARY="$PRIVATE/lib/libelf.a" \
	-DZLIB_INCLUDE_DIR="$SDK_ROOT/usr/include" \
	-DZLIB_LIBRARY="$SDK_ROOT/usr/lib/libz.tbd" \
	-DARGP_LIBRARY="$PRIVATE/lib/libargp.a" \
	-DOBSTACK_LIBRARY="$PRIVATE/lib/libobstack.a"
logged dwarves private_env "$CMAKE" --build "$TEMP_ROOT/build/dwarves" --target pahole \
	-j "$JOBS"
cp "$TEMP_ROOT/build/dwarves/pahole" "$STAGE/bin/pahole"

# resolve_btfids links libelf statically and includes the glibc-style elf.h
# that elfutils installs for hosts without one.
mkdir -p -- "$STAGE/include" "$STAGE/lib/pkgconfig"
for header in elf.h gelf.h libelf.h nlist.h; do
	cp "$PRIVATE/include/$header" "$STAGE/include/$header"
done
cp "$PRIVATE/lib/libelf.a" "$STAGE/lib/libelf.a"
sed -e 's|^prefix=.*$|prefix=${pcfiledir}/../..|' \
	"$TEMP_ROOT/build/elfutils/config/libelf.pc" > "$STAGE/lib/pkgconfig/libelf.pc"

PROVENANCE="$STAGE/share/uml-dependencies"
mkdir -p -- "$PROVENANCE"
cp "$ROOT_DIR/dependencies/manifest.tsv" "$PROVENANCE/manifest.tsv"
cp "$ARCHIVE_MANIFEST" "$PROVENANCE/kernel-host-tools-archives.tsv"
{
	printf 'source\ttree\n'
	for source in musl-obstack elfutils libbpf dwarves; do
		printf '%s\t%s\n' "$source" \
			"$(git -C "$SOURCE_ROOT/$source" rev-parse 'HEAD^{tree}')"
	done
} > "$PROVENANCE/source-trees.tsv"

publish_verified "$STAGE" "$OUTPUT" verify_output "kernel host tools"
"$CMAKE" -E remove_directory "$LOG_DIR"
