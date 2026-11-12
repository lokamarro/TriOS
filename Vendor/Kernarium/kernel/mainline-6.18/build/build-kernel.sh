#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build one replayed Darwin UML kernel without relying on a developer-specific
# LLVM, LLD, or cross-tool location.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/mainline-6.18"
COMMON_ROOT="$ROOT_DIR/kernel/common"
# shellcheck disable=SC1091
. "$LINE_ROOT/build/toolchain/darwin.env"
CONFIG_DIR="$LINE_ROOT/config"
SOURCE_LOCATION="$LINE_ROOT/scripts/source-location.sh"
SLICE_SELECTOR="$LINE_ROOT/scripts/select-slice.sh"
CONFIG_FRAGMENTS="$COMMON_ROOT/tooling/config-fragments.sh"

fail()
{
	printf '%s\n' "kernel/mainline-6.18/build/build-kernel.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/mainline-6.18/build/build-kernel.sh \
       --build PATH [--slice arm64|x86_64] [--jobs N]
       [--product linux|android] [--lockdep]

The compiler, linker, make program, cross prefix, and tool overrides are
configured through UML_KERNEL_{MAKE,CC,HOST_CC,LD,CROSS_COMPILE,AR,NM,OBJCOPY,
OBJDUMP,READELF,STRIP,LLVM,JOBS,EXTRA_HOST_CFLAGS,EXTRA_HOST_CXXFLAGS}. pahole
and the static libelf used for BTF come from UML_KERNEL_HOST_TOOLS_PREFIX
(default .build/dependencies/kernel-host-tools, built by
dependencies/build-kernel-host-tools.sh); UML_KERNEL_PAHOLE may select another
pahole v1.29 or newer.
The configuration is the shared kernel/common/config/darwin-uml.fragment
policy, then this line's overlay for the product. The linux product (the
default) uses config/mainline.fragment. The x86_64 userspace slice applies its
own config fragment after those. The android
product uses config/android.fragment instead; it is the arm64 slice only, and
the build refuses a configuration that lacks a symbol of
config/android-required-builtins.txt or has one of
config/android-forbidden-options.txt. Its kernel is published in the
android-6.18 artifact family.
The output directory must be outside the Linux source tree. Darwin host-tool
compatibility is injected from kernel/mainline-6.18/build/toolchain/darwin-host.
macOS, iOS Simulator, and physical iOS all use the same constrained native
address-space profile. An omitted jobs value uses all online host processors.
The source is always read from the required, Git-ignored
kernel/mainline-6.18/source-location.conf. --lockdep also applies
config/lockdep.fragment last, for a lock-checking probe kernel, which is never
an accepted artifact.
EOF
}

BUILD_DIR=''
SLICE=arm64
PRODUCT=linux
LOCKDEP=0
while [ "$#" -gt 0 ]; do
	case "$1" in
	--slice)
		[ "$#" -ge 2 ] || fail "--slice requires arm64 or x86_64"
		SLICE=$2
		shift 2
		;;
	--product)
		[ "$#" -ge 2 ] || fail "--product requires linux or android"
		PRODUCT=$2
		shift 2
		;;
	--build)
		[ "$#" -ge 2 ] || fail "--build requires a path"
		BUILD_DIR=$2
		shift 2
		;;
	--jobs)
		[ "$#" -ge 2 ] || fail "--jobs requires a positive integer"
		UML_KERNEL_JOBS=$2
		shift 2
		;;
	--lockdep)
		LOCKDEP=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		fail "unknown option: $1"
		;;
	esac
done

[ -r "$SLICE_SELECTOR" ] || fail "missing slice selector: $SLICE_SELECTOR"
# shellcheck disable=SC1090
. "$SLICE_SELECTOR"
[ -r "$CONFIG_FRAGMENTS" ] || fail "missing config fragment helper: $CONFIG_FRAGMENTS"
# shellcheck disable=SC1090
. "$CONFIG_FRAGMENTS"
uml_kernel_select_slice "$LINE_ROOT" "$SLICE" ||
	fail "cannot select kernel slice: $SLICE"

[ -n "$BUILD_DIR" ] || {
	usage >&2
	exit 2
}
[ -x "$SOURCE_LOCATION" ] || fail "missing source-location verifier: $SOURCE_LOCATION"
SOURCE_DIR=$("$SOURCE_LOCATION" --require-source) ||
	fail "kernel build requires a valid kernel/mainline-6.18/source-location.conf"
case "$PRODUCT" in
linux)
	PRODUCT_FRAGMENT="$CONFIG_DIR/mainline.fragment"
	;;
android)
	# The Android product exists once: arm64, no lock-checking probe.
	[ "$UML_KERNEL_SLICE" = arm64 ] ||
		fail "the android product supports only the arm64 slice"
	[ "$LOCKDEP" -eq 0 ] ||
		fail "--lockdep is not available for the android product"
	PRODUCT_FRAGMENT="$CONFIG_DIR/android.fragment"
	;;
*) fail "--product must be linux or android" ;;
esac
case "$UML_KERNEL_JOBS" in
*[!0-9]*|'') fail "UML_KERNEL_JOBS must be a positive integer" ;;
esac
[ "$UML_KERNEL_JOBS" -gt 0 ] || fail "UML_KERNEL_JOBS must be a positive integer"

[ -f "$SOURCE_DIR/Makefile" ] || fail "not a Linux source tree: $SOURCE_DIR"
[ -z "$(git -C "$SOURCE_DIR" status --porcelain --untracked-files=all)" ] ||
	fail "kernel source working tree is dirty"
source_tree=$(git -C "$SOURCE_DIR" rev-parse HEAD^{tree} 2>/dev/null || true)
[ "$source_tree" = "$UML_KERNEL_FINAL_TREE" ] ||
	fail "$UML_KERNEL_SLICE source tree is $source_tree; expected $UML_KERNEL_FINAL_TREE"
"$COMMON_ROOT/abi/sync.sh" --kernel-source "$SOURCE_DIR" --verify ||
	fail "kernel ABI copies are stale; replay the tracked series or run kernel/common/abi/sync.sh"
python3 -B "$COMMON_ROOT/video/sync-protocol.py" --kernel-source "$SOURCE_DIR" --verify ||
	fail "kernel Virtio video protocol copy is stale; replay the tracked series or run kernel/common/video/sync-protocol.py"
python3 -B "$COMMON_ROOT/gpu/sync-protocol.py" --kernel-source "$SOURCE_DIR" --verify ||
	fail "kernel Virtio-GPU memory-report header is stale or collides; replay the tracked series or run kernel/common/gpu/sync-protocol.py"
mkdir -p -- "$BUILD_DIR"
SOURCE_DIR=$(CDPATH= cd -- "$SOURCE_DIR" && pwd -P)
BUILD_DIR=$(CDPATH= cd -- "$BUILD_DIR" && pwd -P)
[ "$SOURCE_DIR" != "$BUILD_DIR" ] ||
	fail "kernel output must be outside the source tree"

command -v "$UML_KERNEL_MAKE" >/dev/null 2>&1 ||
	fail "make program is unavailable: $UML_KERNEL_MAKE"
command -v "$UML_KERNEL_CC" >/dev/null 2>&1 ||
	fail "C compiler is unavailable: $UML_KERNEL_CC"
command -v "$UML_KERNEL_HOST_CC" >/dev/null 2>&1 ||
	fail "host C compiler is unavailable: $UML_KERNEL_HOST_CC"
command -v "$UML_KERNEL_LD" >/dev/null 2>&1 ||
	fail "linker is unavailable: $UML_KERNEL_LD"

# Every line generates BTF. pahole encodes it at each vmlinux link and
# resolve_btfids links the static libelf; both come from the pinned
# kernel-host-tools prefix, never from PATH. Kconfig derives the BTF options
# from the pahole version, and 1.29 is the first release whose parallel
# encoding is always ordered.
pahole_help="build it with dependencies/build-kernel-host-tools.sh or set UML_KERNEL_PAHOLE"
[ -x "$UML_KERNEL_PAHOLE" ] ||
	fail "pahole is unavailable: $UML_KERNEL_PAHOLE ($pahole_help)"
pahole_version=$("$UML_KERNEL_PAHOLE" --version 2>/dev/null) ||
	fail "pahole does not run: $UML_KERNEL_PAHOLE ($pahole_help)"
case "$pahole_version" in
v[0-9]*.[0-9]*)
	pahole_major=${pahole_version#v}
	pahole_major=${pahole_major%%.*}
	pahole_minor=${pahole_version#v*.}
	pahole_minor=${pahole_minor%%[!0-9]*}
	;;
*) fail "unrecognized pahole version '$pahole_version': $UML_KERNEL_PAHOLE" ;;
esac
case "$pahole_major$pahole_minor" in
''|*[!0-9]*) fail "unrecognized pahole version '$pahole_version': $UML_KERNEL_PAHOLE" ;;
esac
[ "$pahole_major" -gt 1 ] || { [ "$pahole_major" -eq 1 ] && [ "$pahole_minor" -ge 29 ]; } ||
	fail "pahole $pahole_version is older than v1.29: $UML_KERNEL_PAHOLE ($pahole_help)"
for host_tool_file in include/libelf.h include/gelf.h include/elf.h lib/libelf.a; do
	[ -f "$UML_KERNEL_HOST_TOOLS_PREFIX/$host_tool_file" ] ||
		fail "kernel host tools lack $host_tool_file: $UML_KERNEL_HOST_TOOLS_PREFIX ($pahole_help)"
done

# Kbuild's standard deterministic-archive flag is not supported by Apple's BSD
# ar. Selecting a compatible archiver here does not make output hashes a
# cross-run acceptance identity.
# When a caller selects an LLVM clang installed with its sibling LLVM tools,
# use that coherent tool family unless an explicit override was supplied.  This
# remains path-agnostic (Homebrew, Xcode toolchains, and custom LLVM installs
# all work) and leaves non-LLVM callers untouched.
compiler_path=$(command -v "$UML_KERNEL_CC")
compiler_dir=$(dirname -- "$compiler_path")
if [ -x "$compiler_dir/llvm-ar" ]; then
	: "${UML_KERNEL_AR:=$compiler_dir/llvm-ar}"
	: "${UML_KERNEL_NM:=$compiler_dir/llvm-nm}"
	: "${UML_KERNEL_OBJCOPY:=$compiler_dir/llvm-objcopy}"
	: "${UML_KERNEL_OBJDUMP:=$compiler_dir/llvm-objdump}"
	: "${UML_KERNEL_READELF:=$compiler_dir/llvm-readelf}"
	: "${UML_KERNEL_STRIP:=$compiler_dir/llvm-strip}"
fi

# Linux 6.18 uses __counted_by flexible arrays through type expressions. Some
# Apple toolchain releases accept the attribute but cannot compile those
# expressions. Diagnose that host-toolchain mismatch before a long Kbuild.
if ! printf '%s\n' \
	'#define __counted_by(member) __attribute__((__counted_by__(member)))' \
	'struct uml_counted_by_probe { int count; int values[] __counted_by(count); };' \
	'int uml_counted_by_probe(struct uml_counted_by_probe *probe) {' \
	'  return __builtin_types_compatible_p(__typeof__(probe->values), __typeof__(&(probe->values)[0]));' \
	'}' | "$UML_KERNEL_CC" -x c -Werror -c -o /dev/null - >/dev/null 2>&1; then
	fail "C compiler lacks Linux 6.18 __counted_by type-expression support: $UML_KERNEL_CC (install a newer LLVM or set UML_KERNEL_CC/UML_KERNEL_HOST_CC)"
fi

run_make()
{
	host_compat="$LINE_ROOT/build/toolchain/darwin-host/host_compat.h"
	host_include="$LINE_ROOT/build/toolchain/darwin-host/include"
	host_tool_bin="$LINE_ROOT/build/toolchain/darwin-host/bin"
	host_tools_include="$UML_KERNEL_HOST_TOOLS_PREFIX/include"
	host_uapi_flags="-I$host_include -I$host_tools_include -idirafter $BUILD_DIR/arch/arm64/include/generated/uapi -idirafter $SOURCE_DIR/arch/arm64/include/uapi -idirafter $SOURCE_DIR/include/uapi -idirafter $BUILD_DIR/include/generated/uapi"
	host_cflags="-include $host_compat $host_uapi_flags $UML_KERNEL_EXTRA_HOST_CFLAGS"
	host_cxxflags="$host_uapi_flags $UML_KERNEL_EXTRA_HOST_CXXFLAGS"
	# resolve_btfids asks pkg-config for libelf; only the pinned prefix may
	# answer, and the library path serves a host without pkg-config.
	set -- "HOSTCFLAGS=$host_cflags" "HOSTCXXFLAGS=$host_cxxflags" \
		"HOSTLDFLAGS=-L$UML_KERNEL_HOST_TOOLS_PREFIX/lib" \
		"PAHOLE=$UML_KERNEL_PAHOLE" "$@"
	if [ -n "$UML_KERNEL_AR" ]; then
		set -- "AR=$UML_KERNEL_AR" "$@"
	fi
	if [ -n "$UML_KERNEL_NM" ]; then
		set -- "NM=$UML_KERNEL_NM" "$@"
	fi
	if [ -n "$UML_KERNEL_OBJCOPY" ]; then
		set -- "OBJCOPY=$UML_KERNEL_OBJCOPY" "$@"
	fi
	if [ -n "$UML_KERNEL_OBJDUMP" ]; then
		set -- "OBJDUMP=$UML_KERNEL_OBJDUMP" "$@"
	fi
	if [ -n "$UML_KERNEL_READELF" ]; then
		set -- "READELF=$UML_KERNEL_READELF" "$@"
	fi
	if [ -n "$UML_KERNEL_STRIP" ]; then
		set -- "STRIP=$UML_KERNEL_STRIP" "$@"
	fi
	if [ -n "$UML_KERNEL_LLVM" ]; then
		set -- "LLVM=$UML_KERNEL_LLVM" "$@"
	fi
	# The Linux banner names the build user and host; every kernel is built
	# as kernarium@kernarium so it names neither the builder's account nor
	# machine. scripts/check-embedded-artifacts.py refuses any other pair.
	PATH="$host_tool_bin:$PATH" \
		KBUILD_BUILD_USER=kernarium KBUILD_BUILD_HOST=kernarium \
		PKG_CONFIG_LIBDIR="$UML_KERNEL_HOST_TOOLS_PREFIX/lib/pkgconfig" \
		PKG_CONFIG_PATH= "$UML_KERNEL_MAKE" \
		-C "$SOURCE_DIR" "O=$BUILD_DIR" \
		ARCH=um SUBARCH=arm64 UML_HOST_OS=Darwin \
		"CC=$UML_KERNEL_CC" "HOSTCC=$UML_KERNEL_HOST_CC" \
		"LD=$UML_KERNEL_LD" "CROSS_COMPILE=$UML_KERNEL_CROSS_COMPILE" "$@"
}

set -- "$COMMON_ROOT/config/darwin-uml.fragment" "$PRODUCT_FRAGMENT"
if [ -n "$UML_KERNEL_SLICE_CONFIG_FRAGMENT" ]; then
	set -- "$@" "$UML_KERNEL_SLICE_CONFIG_FRAGMENT"
fi
if [ "$LOCKDEP" -eq 1 ]; then
	set -- "$@" "$CONFIG_DIR/lockdep.fragment"
fi
uml_kernel_config_compose "$SOURCE_DIR" "$BUILD_DIR/.config" "$@" ||
	fail "cannot compose the kernel configuration"
run_make olddefconfig
uml_kernel_config_verify "$BUILD_DIR/.config" "$@" ||
	fail "kernel configuration does not match its fragments"
if [ "$PRODUCT" = android ]; then
	# Fail before the long build when a symbol the product needs was dropped
	# or one it must not have came back.
	python3 -B "$COMMON_ROOT/artifacts/verify-config-policy.py" \
		--config "$BUILD_DIR/.config" \
		--required "$CONFIG_DIR/android-required-builtins.txt" \
		--forbidden "$CONFIG_DIR/android-forbidden-options.txt" ||
		fail "the Android kernel configuration violates its product policy"
fi
run_make -j"$UML_KERNEL_JOBS"
[ -f "$BUILD_DIR/vmlinux" ] ||
	fail "Kbuild completed without producing $BUILD_DIR/vmlinux"
python3 -B "$COMMON_ROOT/abi/bridge_imports.py" \
	--kernel-binary "$BUILD_DIR/vmlinux" \
	--slice "$UML_KERNEL_SLICE"
if [ "$(uname -s)" = Darwin ]; then
	python3 "$COMMON_ROOT/tooling/verify-console-input.py" \
		--source "$SOURCE_DIR" --build "$BUILD_DIR"
fi
printf '%s\n' "built $PRODUCT $UML_KERNEL_SLICE 4k Darwin UML kernel: $BUILD_DIR/vmlinux"
