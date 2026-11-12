#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the patch-managed GPT fdisk core as one static XCFramework containing
# arm64 Simulator and device slices. The script is offline and consumes only
# sources authenticated by dependencies/prepare.sh.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SCRIPT=dependencies/build-ios-storage.sh
LABEL=storage
PRODUCT=ios-storage
FRAMEWORK=GPTFDisk
SOURCE_NAME=gptfdisk
LIBRARY=libGPTFDisk.a
OUTPUT=${UML_IOS_STORAGE_DEPENDENCY_PREFIX:-"$ROOT_DIR/.build/dependencies/ios-storage"}
WORK_ROOT=${UML_IOS_STORAGE_DEPENDENCY_BUILD_ROOT:-"$ROOT_DIR/.build/dependency-build/ios-storage"}
VERIFY_SCRIPT="$ROOT_DIR/dependencies/cmake/VerifyIosStorageXCFramework.cmake"
JOBS=${UML_IOS_STORAGE_DEPENDENCY_JOBS:-8}

check_source()
{
	[ -f "$SOURCE/Makefile.kernarium" ] || fail "prepared GPT fdisk core source is missing"
}

build_library()
{
	cxx=$($XCRUN --sdk "$sdk_name" --find clang++)
	# Makefile.kernarium adds the file-only define, the language standard
	# and hidden visibility itself.
	"$MAKE" -C "$SOURCE" -f Makefile.kernarium \
		BUILD_DIR="$build_dir" LIBRARY="$library" \
		CXX="$cxx" AR="$ar" RANLIB="$ranlib" \
		CXXFLAGS="-target $target -isysroot $sdk_root -O2 -fPIC -ffunction-sections -fdata-sections -Wall -Wextra -Werror" \
		-j "$JOBS"
}

install_headers()
{
	cp "$SOURCE/kernarium_gptfdisk.h" "$1/GPTFDisk.h"
}

record_provenance()
{
	printf 'source\ttree\ngptfdisk\t%s\n' \
		"$(git -C "$SOURCE" rev-parse 'HEAD^{tree}')" \
		> "$1/source-trees.tsv"
}

. "$ROOT_DIR/dependencies/ios-static-xcframework.sh"
