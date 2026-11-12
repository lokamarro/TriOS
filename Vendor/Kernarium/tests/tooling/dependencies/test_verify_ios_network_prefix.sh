#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
VERIFIER="$ROOT_DIR/dependencies/cmake/VerifyIosNetworkPrefix.cmake"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-ios-network-prefix.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

TEST_NAME=test-verify-ios-network-prefix.sh
. "$(dirname -- "$0")/../support/expect.sh"

ARCHIVES="libpcre2-8.a libffi.a libintl.a libglib-2.0.a libslirp.a"

verify()
{
	cmake -DPREFIX="$1" -DSOURCE_ROOT="$ROOT_DIR" -DPLATFORM="$2" \
		-P "$VERIFIER"
}

series_tree()
{
	awk -F '\t' '$1 == "tree" { print $2 }' \
		"$ROOT_DIR/dependencies/patches/$1/series.tsv"
}

# archive OUTPUT SDK TARGET: a one-member static archive for TARGET.
archive()
{
	object="$TEST_ROOT/object-$$-$(basename -- "$1").o"
	xcrun --sdk "$2" clang -target "$3" -c "$TEST_ROOT/fixture.c" -o "$object"
	rm -f -- "$1"
	xcrun ar rcs "$1" "$object"
}

# make_prefix PREFIX PLATFORM: a prefix that passes verification.
make_prefix()
{
	prefix=$1
	case "$2" in
	ios) sdk=iphoneos target=arm64-apple-ios27.0
		receipt=ios-device-network-arm64.tsv ;;
	ios-simulator) sdk=iphonesimulator target=arm64-apple-ios27.0-simulator
		receipt=ios-simulator-network-arm64.tsv ;;
	esac
	mkdir -p "$prefix/lib/glib-2.0/include" "$prefix/include/glib-2.0" \
		"$prefix/include/slirp" "$prefix/share/uml-dependencies"
	for name in $ARCHIVES; do
		archive "$prefix/lib/$name" "$sdk" "$target"
	done
	for header in include/pcre2.h include/ffi.h include/glib-2.0/glib.h \
		lib/glib-2.0/include/glibconfig.h include/slirp/libslirp.h; do
		printf '%s\n' fixture >"$prefix/$header"
	done
	cp "$ROOT_DIR/dependencies/manifest.tsv" \
		"$prefix/share/uml-dependencies/manifest.tsv"
	cp "$ROOT_DIR/dependencies/platforms/$receipt" \
		"$prefix/share/uml-dependencies/$receipt"
	{
		printf 'source\ttree\n'
		for source in glib-ios libslirp; do
			printf '%s\t%s\n' "$source" "$(series_tree "$source")"
		done
	} >"$prefix/share/uml-dependencies/source-trees.tsv"
}

copy_fixture()
{
	cp -R "$DEVICE" "$1"
}

command -v xcrun >/dev/null 2>&1 || fail "xcrun is required"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"
printf '%s\n' 'int uml_ios_network_fixture(void) { return 0; }' \
	>"$TEST_ROOT/fixture.c"
DEVICE="$TEST_ROOT/device"
SIMULATOR="$TEST_ROOT/simulator"
make_prefix "$DEVICE" ios
make_prefix "$SIMULATOR" ios-simulator
TREES="$DEVICE/share/uml-dependencies/source-trees.tsv"

verify "$DEVICE" ios >/dev/null || fail "a valid device prefix was rejected"
verify "$SIMULATOR" ios-simulator >/dev/null ||
	fail "a valid Simulator prefix was rejected"

expect_cmake_diagnostic "Unsupported iOS network platform" \
	verify "$DEVICE" macos
expect_cmake_diagnostic "ios-device-network-arm64.tsv" \
	verify "$SIMULATOR" ios

# Every archive is an arm64 archive for the platform at iOS 27.0.
CASE="$TEST_ROOT/simulator-as-device"
cp -R "$SIMULATOR" "$CASE"
cp "$ROOT_DIR/dependencies/platforms/ios-device-network-arm64.tsv" \
	"$CASE/share/uml-dependencies/"
expect_cmake_diagnostic "lib/libpcre2-8.a" verify "$CASE" ios
CASE="$TEST_ROOT/device-as-simulator"
copy_fixture "$CASE"
cp "$ROOT_DIR/dependencies/platforms/ios-simulator-network-arm64.tsv" \
	"$CASE/share/uml-dependencies/"
expect_cmake_diagnostic "lib/libpcre2-8.a" verify "$CASE" ios-simulator

CASE="$TEST_ROOT/simulator-member"
copy_fixture "$CASE"
archive "$CASE/lib/libslirp.a" iphonesimulator arm64-apple-ios27.0-simulator
expect_cmake_diagnostic "lib/libslirp.a" verify "$CASE" ios

CASE="$TEST_ROOT/old-deployment-target"
copy_fixture "$CASE"
archive "$CASE/lib/libglib-2.0.a" iphoneos arm64-apple-ios18.0
expect_cmake_diagnostic "lib/libglib-2.0.a" verify "$CASE" ios

# The whole minimum OS version counts, so iOS 27.0.1 is not iOS 27.0.
CASE="$TEST_ROOT/patch-deployment-target"
copy_fixture "$CASE"
archive "$CASE/lib/libglib-2.0.a" iphoneos arm64-apple-ios27.0.1
expect_cmake_diagnostic "lib/libglib-2.0.a: minos 27.0.1" verify "$CASE" ios

# Every member counts, not only the first one that matches. A member built
# for iOS 11 carries a legacy minimum-version command instead of a
# build-version command.
for member in iphoneos:arm64-apple-ios18.0 \
	iphonesimulator:arm64-apple-ios27.0-simulator \
	iphoneos:arm64-apple-ios11.0; do
	CASE="$TEST_ROOT/mixed-members-${member#*:}"
	copy_fixture "$CASE"
	xcrun --sdk "${member%%:*}" clang -target "${member#*:}" \
		-c "$TEST_ROOT/fixture.c" -o "$TEST_ROOT/other-member.o"
	xcrun ar rs "$CASE/lib/libslirp.a" "$TEST_ROOT/other-member.o" 2>/dev/null
	expect_cmake_diagnostic "lib/libslirp.a" verify "$CASE" ios
done

# Every member carries exactly one build-version command. Each of these
# members, added beside a valid one, leaves every platform row in the archive
# valid: a member without the command, a member with two iOS 27.0 commands,
# and a bitcode member, which otool cannot read as Mach-O.
MEMBERS="$TEST_ROOT/members"
mkdir "$MEMBERS"
xcrun --sdk iphoneos clang -target arm64-apple-ios27.0 \
	-c "$TEST_ROOT/fixture.c" -o "$MEMBERS/ios.o"
xcrun vtool -remove-build-version ios -output "$MEMBERS/no-build-version.o" \
	"$MEMBERS/ios.o"
xcrun ld -r -arch arm64 -headerpad 0x100 "$MEMBERS/ios.o" \
	-o "$MEMBERS/two-build-versions.o"
# Copy the build-version command into the header padding ld left.
python3 - "$MEMBERS/two-build-versions.o" <<'EOF'
import struct
import sys

LC_BUILD_VERSION = 0x32
path = sys.argv[1]
with open(path, "rb") as stream:
    data = bytearray(stream.read())
if struct.unpack_from("<I", data, 0)[0] != 0xFEEDFACF:
    sys.exit(f"{path} is not a 64-bit Mach-O object")
count, size = struct.unpack_from("<2I", data, 16)
offset = 32
for _ in range(count):
    command, command_size = struct.unpack_from("<2I", data, offset)
    if command == LC_BUILD_VERSION:
        build_version = data[offset:offset + command_size]
    offset += command_size
end = 32 + size
if any(data[end:end + len(build_version)]):
    sys.exit(f"{path} has no header padding for another command")
data[end:end + len(build_version)] = build_version
struct.pack_into("<2I", data, 16, count + 1, size + len(build_version))
with open(path, "wb") as stream:
    stream.write(data)
EOF
[ "$(otool -l "$MEMBERS/two-build-versions.o" |
	grep -c 'cmd LC_BUILD_VERSION$')" -eq 2 ] ||
	fail "the fixture member lacks a second build-version command"
xcrun --sdk iphoneos clang -target arm64-apple-ios27.0 -flto \
	-c "$TEST_ROOT/fixture.c" -o "$MEMBERS/bitcode.o"
for member in no-build-version two-build-versions bitcode; do
	CASE="$TEST_ROOT/member-$member"
	copy_fixture "$CASE"
	xcrun ar rs "$CASE/lib/libslirp.a" "$MEMBERS/$member.o" 2>/dev/null
	expect_cmake_diagnostic "lib/libslirp.a($member.o)" verify "$CASE" ios
done

CASE="$TEST_ROOT/intel-archive"
copy_fixture "$CASE"
archive "$CASE/lib/libffi.a" iphonesimulator x86_64-apple-ios27.0-simulator
expect_cmake_diagnostic "lib/libffi.a" verify "$CASE" ios

CASE="$TEST_ROOT/universal-archive"
copy_fixture "$CASE"
archive "$TEST_ROOT/intel.a" iphonesimulator x86_64-apple-ios27.0-simulator
xcrun lipo -create "$DEVICE/lib/libintl.a" "$TEST_ROOT/intel.a" \
	-output "$CASE/lib/libintl.a"
expect_cmake_diagnostic "lib/libintl.a" verify "$CASE" ios

# Required payload is present and not a symlink.
CASE="$TEST_ROOT/missing-archive"
copy_fixture "$CASE"
rm "$CASE/lib/libpcre2-8.a"
expect_cmake_diagnostic "lib/libpcre2-8.a" verify "$CASE" ios

CASE="$TEST_ROOT/linked-header"
copy_fixture "$CASE"
rm "$CASE/include/slirp/libslirp.h"
ln -s "$DEVICE/include/slirp/libslirp.h" "$CASE/include/slirp/libslirp.h"
expect_cmake_diagnostic "include/slirp/libslirp.h" verify "$CASE" ios

# The installed platform receipt is the checked-in one, byte for byte.
CASE="$TEST_ROOT/edited-receipt"
copy_fixture "$CASE"
sed 's/^format\t2$/format\t3/' \
	"$DEVICE/share/uml-dependencies/ios-device-network-arm64.tsv" \
	>"$CASE/share/uml-dependencies/ios-device-network-arm64.tsv"
cmp -s "$DEVICE/share/uml-dependencies/ios-device-network-arm64.tsv" \
	"$CASE/share/uml-dependencies/ios-device-network-arm64.tsv" &&
	fail "the receipt fixture edit changed nothing"
expect_cmake_diagnostic "provenance does not match" verify "$CASE" ios

CASE="$TEST_ROOT/missing-receipt"
copy_fixture "$CASE"
rm "$CASE/share/uml-dependencies/ios-device-network-arm64.tsv"
expect_cmake_diagnostic "share/uml-dependencies/ios-device-network-arm64.tsv" \
	verify "$CASE" ios

# The prefix was built from the product's manifest rows.
CASE="$TEST_ROOT/changed-manifest-row"
copy_fixture "$CASE"
awk -F '\t' -v OFS='\t' '$1 == "libslirp" { $5 = "0000000" } { print }' \
	"$ROOT_DIR/dependencies/manifest.tsv" \
	>"$CASE/share/uml-dependencies/manifest.tsv"
expect_cmake_diagnostic "manifest rows do not match" verify "$CASE" ios

# Each patched source the manifest names records the tree its series records.
for source in glib-ios libslirp; do
	CASE="$TEST_ROOT/stale-$source"
	copy_fixture "$CASE"
	awk -F '\t' -v OFS='\t' -v source="$source" \
		'$1 == source { $2 = "0000000000000000000000000000000000000000" }
		{ print }' "$TREES" >"$CASE/share/uml-dependencies/source-trees.tsv"
	expect_cmake_diagnostic "$source was built from tree" verify "$CASE" ios

	CASE="$TEST_ROOT/missing-$source"
	copy_fixture "$CASE"
	grep -v "^$source	" "$TREES" \
		>"$CASE/share/uml-dependencies/source-trees.tsv"
	expect_cmake_diagnostic "$source was built from tree ''" \
		verify "$CASE" ios
done

CASE="$TEST_ROOT/extra-source-tree"
copy_fixture "$CASE"
printf 'glib\t%s\n' 0000000000000000000000000000000000000000 \
	>>"$CASE/share/uml-dependencies/source-trees.tsv"
expect_cmake_diagnostic "source trees list" verify "$CASE" ios

CASE="$TEST_ROOT/missing-source-trees"
copy_fixture "$CASE"
rm "$CASE/share/uml-dependencies/source-trees.tsv"
expect_cmake_diagnostic "source trees are missing" verify "$CASE" ios

# The patched sources are the product's manifest rows with a patch series, so
# a series given to another row is required with no verifier change.
SOURCE="$TEST_ROOT/source"
mkdir -p "$SOURCE/dependencies/platforms" "$SOURCE/dependencies/patches/pcre2"
for source in glib-ios libslirp; do
	mkdir -p "$SOURCE/dependencies/patches/$source"
	cp "$ROOT_DIR/dependencies/patches/$source/series.tsv" \
		"$SOURCE/dependencies/patches/$source/series.tsv"
done
PCRE2_TREE=1111111111111111111111111111111111111111
printf 'tree\t%s\n' "$PCRE2_TREE" \
	>"$SOURCE/dependencies/patches/pcre2/series.tsv"
cp "$ROOT_DIR/dependencies/platforms/ios-device-network-arm64.tsv" \
	"$SOURCE/dependencies/platforms/"
awk -F '\t' -v OFS='\t' \
	'$1 == "pcre2" { $7 = "patches/pcre2/series.tsv" } { print }' \
	"$ROOT_DIR/dependencies/manifest.tsv" >"$SOURCE/dependencies/manifest.tsv"
verify_patched_pcre2()
{
	cmake -DPREFIX="$1" -DSOURCE_ROOT="$SOURCE" -DPLATFORM=ios \
		-P "$VERIFIER"
}
CASE="$TEST_ROOT/patched-pcre2"
copy_fixture "$CASE"
cp "$SOURCE/dependencies/manifest.tsv" \
	"$CASE/share/uml-dependencies/manifest.tsv"
expect_cmake_diagnostic "pcre2 was built from tree ''" \
	verify_patched_pcre2 "$CASE"
printf 'pcre2\t%s\n' "$PCRE2_TREE" \
	>>"$CASE/share/uml-dependencies/source-trees.tsv"
verify_patched_pcre2 "$CASE" >/dev/null ||
	fail "a prefix listing every patched source the manifest names was rejected"

printf '%s\n' "iOS network prefix verifier tests passed"
