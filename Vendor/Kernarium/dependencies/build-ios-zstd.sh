#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the pinned zstd decompressor as one static XCFramework containing
# arm64 Simulator and device slices. The script is offline and consumes only
# sources authenticated by dependencies/prepare.sh.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SCRIPT=dependencies/build-ios-zstd.sh
LABEL=zstd
PRODUCT=ios-compression
FRAMEWORK=LibZstd
SOURCE_NAME=zstd
LIBRARY=static/libzstd.a
OUTPUT=${UML_IOS_ZSTD_DEPENDENCY_PREFIX:-"$ROOT_DIR/.build/dependencies/ios-zstd"}
WORK_ROOT=${UML_IOS_ZSTD_DEPENDENCY_BUILD_ROOT:-"$ROOT_DIR/.build/dependency-build/ios-zstd"}
VERIFY_SCRIPT="$ROOT_DIR/dependencies/cmake/VerifyIosZstdXCFramework.cmake"
JOBS=${UML_IOS_ZSTD_DEPENDENCY_JOBS:-8}

check_source()
{
	[ -f "$SOURCE/lib/Makefile" ] || fail "prepared zstd source is missing"
}

build_library()
{
	cc=$($XCRUN --sdk "$sdk_name" --find clang)
	"$MAKE" -C "$SOURCE/lib" \
		BUILD_DIR="$build_dir" \
		CC="$cc" AR="$ar" RANLIB="$ranlib" \
		CPPFLAGS="-DZSTD_LEGACY_SUPPORT=0 -DZSTD_DISABLE_ASM=1" \
		CFLAGS="-target $target -isysroot $sdk_root -O3 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -Wall -Wextra -Werror" \
		ZSTD_LEGACY_SUPPORT=0 \
		ZSTD_LIB_COMPRESSION=0 \
		ZSTD_LIB_DECOMPRESSION=1 \
		ZSTD_LIB_DICTBUILDER=0 \
		ZSTD_LIB_DEPRECATED=0 \
		-j "$JOBS" "$library"
	[ -f "$library" ] || fail "zstd archive was not created for $platform"
}

install_headers()
{
	cp "$SOURCE/lib/zstd.h" "$1/zstd.h"
	cp "$SOURCE/lib/zstd_errors.h" "$1/zstd_errors.h"
}

# The zstd source is pinned by the manifest alone, with no local series.
record_provenance()
{
	:
}

. "$ROOT_DIR/dependencies/ios-static-xcframework.sh"
