#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
VERIFIERS="$ROOT_DIR/dependencies/cmake"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-ios-static-xcframework.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

TEST_NAME=test-verify-ios-static-xcframework.sh
. "$(dirname -- "$0")/../support/expect.sh"

verify_storage()
{
	cmake -DPREFIX="$1" -DSOURCE_ROOT="$ROOT_DIR" \
		-P "$VERIFIERS/VerifyIosStorageXCFramework.cmake"
}

verify_zstd()
{
	cmake -DPREFIX="$1" -DSOURCE_ROOT="$ROOT_DIR" \
		-P "$VERIFIERS/VerifyIosZstdXCFramework.cmake"
}

# slice_binary OUTPUT SLICE SOURCE [TARGET]: a static framework binary for
# SLICE built from SOURCE, for TARGET when given.
slice_binary()
{
	case "$2" in
	ios-arm64) sdk=iphoneos target=arm64-apple-ios27.0 ;;
	ios-arm64-simulator) sdk=iphonesimulator
		target=arm64-apple-ios27.0-simulator ;;
	esac
	target=${4:-$target}
	object="$TEST_ROOT/object-$$.o"
	xcrun --sdk "$sdk" clang -target "$target" -c "$3" -o "$object"
	rm -f -- "$1"
	xcrun ar rcs "$1" "$object"
}

# make_prefix PREFIX FRAMEWORK CONTRACT SOURCE HEADER...: an XCFramework prefix
# that passes verification, with a device and a Simulator slice.
make_prefix()
{
	prefix=$1
	framework=$2
	contract=$3
	source=$4
	shift 4
	xcframework="$prefix/$framework.xcframework"
	mkdir -p "$xcframework" "$prefix/share/uml-dependencies"
	printf '%s\n' fixture >"$xcframework/Info.plist"
	for slice in ios-arm64 ios-arm64-simulator; do
		bundle="$xcframework/$slice/$framework.framework"
		mkdir -p "$bundle/Headers" "$bundle/Modules"
		slice_binary "$bundle/$framework" "$slice" "$source"
		for header in "$@"; do
			printf '%s\n' fixture >"$bundle/Headers/$header"
		done
		printf 'framework module %s {}\n' "$framework" \
			>"$bundle/Modules/module.modulemap"
	done
	cp "$ROOT_DIR/dependencies/manifest.tsv" \
		"$prefix/share/uml-dependencies/manifest.tsv"
	for platform in device simulator; do
		cp "$ROOT_DIR/dependencies/platforms/ios-$platform-$contract-arm64.tsv" \
			"$prefix/share/uml-dependencies/"
	done
}

copy_fixture()
{
	cp -R "$1" "$2"
}

command -v xcrun >/dev/null 2>&1 || fail "xcrun is required"
cat >"$TEST_ROOT/gptfdisk.c" <<'EOF'
int kernarium_gptfdisk_read_partition(void) { return 0; }
int kernarium_gptfdisk_grow_partition_to_fill(void) { return 0; }
EOF
cat >"$TEST_ROOT/gptfdisk-reader.c" <<'EOF'
int kernarium_gptfdisk_read_partition(void) { return 0; }
EOF
cat >"$TEST_ROOT/gptfdisk-cli.c" <<'EOF'
int kernarium_gptfdisk_read_partition(void) { return 0; }
int kernarium_gptfdisk_grow_partition_to_fill(void) { return 0; }
int main(void) { return 0; }
EOF
ZSTD_SYMBOLS="ZSTD_createDStream ZSTD_DStreamInSize ZSTD_DStreamOutSize
ZSTD_decompressStream ZSTD_freeDStream ZSTD_getErrorName ZSTD_initDStream
ZSTD_isError ZSTD_versionNumber"
for symbol in $ZSTD_SYMBOLS; do
	printf 'int %s(void) { return 0; }\n' "$symbol"
done >"$TEST_ROOT/zstd.c"
{
	cat "$TEST_ROOT/zstd.c"
	printf '%s\n' 'int ZSTD_compressStream(void) { return 0; }'
} >"$TEST_ROOT/zstd-compressor.c"

STORAGE="$TEST_ROOT/storage"
make_prefix "$STORAGE" GPTFDisk storage "$TEST_ROOT/gptfdisk.c" GPTFDisk.h
GPTFDISK_TREE=$(awk -F '\t' '$1 == "tree" { print $2 }' \
	"$ROOT_DIR/dependencies/patches/gptfdisk/series.tsv")
printf 'source\ttree\ngptfdisk\t%s\n' "$GPTFDISK_TREE" \
	>"$STORAGE/share/uml-dependencies/source-trees.tsv"
# The compression product has no patched source, so its prefix records no
# source trees.
ZSTD="$TEST_ROOT/zstd"
make_prefix "$ZSTD" LibZstd compression "$TEST_ROOT/zstd.c" \
	zstd.h zstd_errors.h

verify_storage "$STORAGE" >/dev/null || fail "a valid storage prefix was rejected"
verify_zstd "$ZSTD" >/dev/null || fail "a valid zstd prefix was rejected"

DEVICE_GPTFDISK="GPTFDisk.xcframework/ios-arm64/GPTFDisk.framework"
SIMULATOR_GPTFDISK="GPTFDisk.xcframework/ios-arm64-simulator/GPTFDisk.framework"

# Each slice is an arm64 archive for its platform at iOS 27.0.
CASE="$TEST_ROOT/simulator-in-device-slice"
copy_fixture "$STORAGE" "$CASE"
cp "$STORAGE/$SIMULATOR_GPTFDISK/GPTFDisk" "$CASE/$DEVICE_GPTFDISK/GPTFDisk"
expect_cmake_diagnostic "$DEVICE_GPTFDISK/GPTFDisk" verify_storage "$CASE"

CASE="$TEST_ROOT/device-in-simulator-slice"
copy_fixture "$STORAGE" "$CASE"
cp "$STORAGE/$DEVICE_GPTFDISK/GPTFDisk" "$CASE/$SIMULATOR_GPTFDISK/GPTFDisk"
expect_cmake_diagnostic "$SIMULATOR_GPTFDISK/GPTFDisk" verify_storage "$CASE"

CASE="$TEST_ROOT/old-deployment-target"
copy_fixture "$STORAGE" "$CASE"
slice_binary "$CASE/$DEVICE_GPTFDISK/GPTFDisk" ios-arm64 \
	"$TEST_ROOT/gptfdisk.c" arm64-apple-ios18.0
expect_cmake_diagnostic "$DEVICE_GPTFDISK/GPTFDisk" verify_storage "$CASE"

CASE="$TEST_ROOT/intel-slice"
copy_fixture "$STORAGE" "$CASE"
slice_binary "$CASE/$SIMULATOR_GPTFDISK/GPTFDisk" ios-arm64-simulator \
	"$TEST_ROOT/gptfdisk.c" x86_64-apple-ios27.0-simulator
expect_cmake_diagnostic "$SIMULATOR_GPTFDISK/GPTFDisk" verify_storage "$CASE"

# Each slice exports its symbol contract and nothing forbidden.
CASE="$TEST_ROOT/missing-symbol"
copy_fixture "$STORAGE" "$CASE"
slice_binary "$CASE/$SIMULATOR_GPTFDISK/GPTFDisk" ios-arm64-simulator \
	"$TEST_ROOT/gptfdisk-reader.c"
expect_cmake_diagnostic "is missing kernarium_gptfdisk_grow_partition_to_fill" \
	verify_storage "$CASE"

CASE="$TEST_ROOT/command-line-front-end"
copy_fixture "$STORAGE" "$CASE"
slice_binary "$CASE/$DEVICE_GPTFDISK/GPTFDisk" ios-arm64 \
	"$TEST_ROOT/gptfdisk-cli.c"
expect_cmake_diagnostic "contains a GPT fdisk CLI front end" \
	verify_storage "$CASE"

CASE="$TEST_ROOT/compressor"
copy_fixture "$ZSTD" "$CASE"
slice_binary "$CASE/LibZstd.xcframework/ios-arm64/LibZstd.framework/LibZstd" \
	ios-arm64 "$TEST_ROOT/zstd-compressor.c"
expect_cmake_diagnostic "contains a zstd CLI or unused compression API" \
	verify_zstd "$CASE"

# Required payload is present and not a symlink.
CASE="$TEST_ROOT/linked-header"
copy_fixture "$STORAGE" "$CASE"
rm "$CASE/$SIMULATOR_GPTFDISK/Headers/GPTFDisk.h"
ln -s "$STORAGE/$SIMULATOR_GPTFDISK/Headers/GPTFDisk.h" \
	"$CASE/$SIMULATOR_GPTFDISK/Headers/GPTFDisk.h"
expect_cmake_diagnostic "$SIMULATOR_GPTFDISK/Headers/GPTFDisk.h" \
	verify_storage "$CASE"

CASE="$TEST_ROOT/missing-module-map"
copy_fixture "$ZSTD" "$CASE"
rm "$CASE/LibZstd.xcframework/ios-arm64/LibZstd.framework/Modules/module.modulemap"
expect_cmake_diagnostic "LibZstd.framework/Modules/module.modulemap" \
	verify_zstd "$CASE"

# The installed platform receipts are the checked-in ones, byte for byte.
CASE="$TEST_ROOT/edited-receipt"
copy_fixture "$ZSTD" "$CASE"
printf '%s\n' 'extra	row' \
	>>"$CASE/share/uml-dependencies/ios-simulator-compression-arm64.tsv"
expect_cmake_diagnostic "provenance does not match" verify_zstd "$CASE"

CASE="$TEST_ROOT/missing-receipt"
copy_fixture "$STORAGE" "$CASE"
rm "$CASE/share/uml-dependencies/ios-device-storage-arm64.tsv"
expect_cmake_diagnostic "share/uml-dependencies/ios-device-storage-arm64.tsv" \
	verify_storage "$CASE"

# The prefix was built from the product's manifest rows.
CASE="$TEST_ROOT/changed-manifest-row"
copy_fixture "$STORAGE" "$CASE"
awk -F '\t' -v OFS='\t' '$1 == "gptfdisk" { $5 = "0000000" } { print }' \
	"$ROOT_DIR/dependencies/manifest.tsv" \
	>"$CASE/share/uml-dependencies/manifest.tsv"
expect_cmake_diagnostic "manifest rows do not match" verify_storage "$CASE"

# The patched source records the tree its series records.
CASE="$TEST_ROOT/stale-gptfdisk"
copy_fixture "$STORAGE" "$CASE"
printf 'source\ttree\ngptfdisk\t%s\n' 0000000000000000000000000000000000000000 \
	>"$CASE/share/uml-dependencies/source-trees.tsv"
expect_cmake_diagnostic "gptfdisk was built from tree" verify_storage "$CASE"

CASE="$TEST_ROOT/missing-gptfdisk"
copy_fixture "$STORAGE" "$CASE"
printf 'source\ttree\n' >"$CASE/share/uml-dependencies/source-trees.tsv"
expect_cmake_diagnostic "gptfdisk was built from tree ''" verify_storage "$CASE"

CASE="$TEST_ROOT/extra-source-tree"
copy_fixture "$STORAGE" "$CASE"
printf 'zstd\t%s\n' 0000000000000000000000000000000000000000 \
	>>"$CASE/share/uml-dependencies/source-trees.tsv"
expect_cmake_diagnostic "source trees list" verify_storage "$CASE"

CASE="$TEST_ROOT/missing-source-trees"
copy_fixture "$STORAGE" "$CASE"
rm "$CASE/share/uml-dependencies/source-trees.tsv"
expect_cmake_diagnostic "source trees are missing" verify_storage "$CASE"

printf '%s\n' "iOS static XCFramework verifier tests passed"
