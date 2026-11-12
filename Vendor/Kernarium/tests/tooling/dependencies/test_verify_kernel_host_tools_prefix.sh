#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
VERIFIER="$ROOT_DIR/dependencies/cmake/VerifyKernelHostToolsPrefix.cmake"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-kernel-host-tools-prefix.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

TEST_NAME=test-verify-kernel-host-tools-prefix.sh
. "$(dirname -- "$0")/../support/expect.sh"

SOURCES="musl-obstack elfutils libbpf dwarves"

verify()
{
	cmake -DPREFIX="$1" -DSOURCE_ROOT="$ROOT_DIR" -P "$VERIFIER"
}

# pahole OUTPUT VERSION [ARCH]: an executable that reports VERSION.
pahole()
{
	printf '#include <stdio.h>\nint main(void) { puts("%s"); return 0; }\n' \
		"$2" >"$TEST_ROOT/pahole.c"
	xcrun --sdk macosx clang -arch "${3:-arm64}" "$TEST_ROOT/pahole.c" -o "$1"
}

copy_fixture()
{
	cp -R "$BASE_PREFIX" "$1"
}

command -v xcrun >/dev/null 2>&1 || fail "xcrun is required"
DWARVES_VERSION=$(awk -F '\t' '$1 == "dwarves" { print $2 }' \
	"$ROOT_DIR/dependencies/manifest.tsv")
[ -n "$DWARVES_VERSION" ] || fail "the manifest pins no dwarves release"

BASE_PREFIX="$TEST_ROOT/prefix"
mkdir -p "$BASE_PREFIX/bin" "$BASE_PREFIX/include" \
	"$BASE_PREFIX/lib/pkgconfig" "$BASE_PREFIX/share/uml-dependencies"
pahole "$BASE_PREFIX/bin/pahole" "v$DWARVES_VERSION"
for file in include/elf.h include/gelf.h include/libelf.h include/nlist.h \
	lib/libelf.a lib/pkgconfig/libelf.pc; do
	printf '%s\n' fixture >"$BASE_PREFIX/$file"
done
cp "$ROOT_DIR/dependencies/manifest.tsv" \
	"$BASE_PREFIX/share/uml-dependencies/manifest.tsv"
cp "$ROOT_DIR/dependencies/kernel-host-tools/archives.tsv" \
	"$BASE_PREFIX/share/uml-dependencies/kernel-host-tools-archives.tsv"
TREES="$BASE_PREFIX/share/uml-dependencies/source-trees.tsv"
{
	printf 'source\ttree\n'
	for source in $SOURCES; do
		printf '%s\t%s\n' "$source" "$(awk -F '\t' '$1 == "tree" { print $2 }' \
			"$ROOT_DIR/dependencies/patches/$source/series.tsv")"
	done
} >"$TREES"

verify "$BASE_PREFIX" >/dev/null || fail "a valid prefix was rejected"

# pahole is an arm64 executable reporting the pinned dwarves release.
CASE="$TEST_ROOT/intel-pahole"
copy_fixture "$CASE"
pahole "$CASE/bin/pahole" "v$DWARVES_VERSION" x86_64
expect_cmake_diagnostic "pahole is not an arm64" verify "$CASE"

CASE="$TEST_ROOT/other-release"
copy_fixture "$CASE"
pahole "$CASE/bin/pahole" v0.1
expect_cmake_diagnostic "pahole reports 'v0.1'" verify "$CASE"

# Required payload is present and not a symlink.
CASE="$TEST_ROOT/linked-header"
copy_fixture "$CASE"
rm "$CASE/include/gelf.h"
ln -s "$BASE_PREFIX/include/gelf.h" "$CASE/include/gelf.h"
expect_cmake_diagnostic "include/gelf.h" verify "$CASE"

CASE="$TEST_ROOT/missing-library"
copy_fixture "$CASE"
rm "$CASE/lib/libelf.a"
expect_cmake_diagnostic "lib/libelf.a" verify "$CASE"

# The installed archive manifest is the checked-in one, byte for byte.
CASE="$TEST_ROOT/edited-archives"
copy_fixture "$CASE"
printf '%s\n' '# edited' \
	>>"$CASE/share/uml-dependencies/kernel-host-tools-archives.tsv"
expect_cmake_diagnostic "provenance does not match" verify "$CASE"

CASE="$TEST_ROOT/missing-archives"
copy_fixture "$CASE"
rm "$CASE/share/uml-dependencies/kernel-host-tools-archives.tsv"
expect_cmake_diagnostic "kernel-host-tools-archives.tsv" verify "$CASE"

# The prefix was built from the product's manifest rows.
CASE="$TEST_ROOT/changed-manifest-row"
copy_fixture "$CASE"
awk -F '\t' -v OFS='\t' '$1 == "elfutils" { $5 = "0000000" } { print }' \
	"$ROOT_DIR/dependencies/manifest.tsv" \
	>"$CASE/share/uml-dependencies/manifest.tsv"
expect_cmake_diagnostic "manifest rows do not match" verify "$CASE"

# Each patched source the manifest names records the tree its series records.
for source in $SOURCES; do
	CASE="$TEST_ROOT/stale-$source"
	copy_fixture "$CASE"
	awk -F '\t' -v OFS='\t' -v source="$source" \
		'$1 == source { $2 = "0000000000000000000000000000000000000000" }
		{ print }' "$TREES" >"$CASE/share/uml-dependencies/source-trees.tsv"
	expect_cmake_diagnostic "$source was built from tree" verify "$CASE"

	CASE="$TEST_ROOT/missing-$source"
	copy_fixture "$CASE"
	grep -v "^$source	" "$TREES" \
		>"$CASE/share/uml-dependencies/source-trees.tsv"
	expect_cmake_diagnostic "$source was built from tree ''" verify "$CASE"
done

CASE="$TEST_ROOT/extra-source-tree"
copy_fixture "$CASE"
printf 'gawk\t%s\n' 0000000000000000000000000000000000000000 \
	>>"$CASE/share/uml-dependencies/source-trees.tsv"
expect_cmake_diagnostic "source trees list" verify "$CASE"

printf '%s\n' "Kernel host tools prefix verifier tests passed"
