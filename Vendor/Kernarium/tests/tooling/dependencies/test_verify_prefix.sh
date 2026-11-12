#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

SOURCE_DEPENDENCY_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../dependencies" && pwd)
DEPLOYMENT_TARGET=27.0
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-dependency-prefix.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM
DEPENDENCY_DIR="$TEST_ROOT/dependencies"
mkdir -p "$DEPENDENCY_DIR" "$TEST_ROOT/scripts"
cp "$SOURCE_DEPENDENCY_DIR/verify-prefix.sh" \
	"$SOURCE_DEPENDENCY_DIR/prepare.sh" \
	"$SOURCE_DEPENDENCY_DIR/series-field.sh" \
	"$DEPENDENCY_DIR/"
# Preparation resolves its series tooling from the tree it ships in, so the
# fixture tree carries the real helper too.
cp "$SOURCE_DEPENDENCY_DIR/../scripts/patch_series.py" "$TEST_ROOT/scripts/"
VERIFIER="$DEPENDENCY_DIR/verify-prefix.sh"
PREPARE="$DEPENDENCY_DIR/prepare.sh"

TEST_NAME=test-verify-prefix.sh
. "$(dirname -- "$0")/../support/expect.sh"

verify()
{
	"$VERIFIER" \
		--prefix "$1" \
		--manifest "$MANIFEST" \
		--deployment-target "$DEPLOYMENT_TARGET"
}

copy_fixture()
{
	destination=$1
	cp -R "$BASE_PREFIX" "$destination"
}

command -v xcrun >/dev/null 2>&1 || fail "xcrun is required"
MANIFEST="$DEPENDENCY_DIR/manifest.tsv"
PATCH_DIRECTORY="$DEPENDENCY_DIR/patches/fixture"
PATCH="$PATCH_DIRECTORY/fixture-maintained-change.patch"
BASE_PREFIX="$TEST_ROOT/prefix"
mkdir -p "$BASE_PREFIX/include/glib-2.0" \
	"$BASE_PREFIX/include/SoftFloat-3e" \
	"$BASE_PREFIX/include/FEXCore/Core" \
	"$BASE_PREFIX/include/FEXCore/Utils" \
	"$BASE_PREFIX/include/FEXCore/Config" \
	"$BASE_PREFIX/include/FEXCore/IR" \
	"$BASE_PREFIX/include/fmt" \
	"$BASE_PREFIX/include/ankerl" \
	"$BASE_PREFIX/include/slirp" \
	"$BASE_PREFIX/include/epoxy" \
	"$BASE_PREFIX/include/rfb" \
	"$BASE_PREFIX/include/virgl" \
	"$BASE_PREFIX/lib/glib-2.0/include" \
	"$BASE_PREFIX/share/uml-dependencies" \
	"$PATCH_DIRECTORY"
PATCH_REPOSITORY="$TEST_ROOT/patch-repository"
mkdir -p "$PATCH_REPOSITORY"
git -C "$PATCH_REPOSITORY" init --quiet
printf '%s\n' upstream >"$PATCH_REPOSITORY/fixture.txt"
git -C "$PATCH_REPOSITORY" add fixture.txt
git -C "$PATCH_REPOSITORY" commit --quiet -m 'fixture: upstream base'
BASE_COMMIT=$(git -C "$PATCH_REPOSITORY" rev-parse HEAD)
printf '%s\n' project-patched >"$PATCH_REPOSITORY/fixture.txt"
git -C "$PATCH_REPOSITORY" commit --quiet -am 'fixture: maintained change'
python3 -B "$TEST_ROOT/scripts/patch_series.py" export \
	--source "$PATCH_REPOSITORY" --base "$BASE_COMMIT" --head HEAD \
	--series-dir "$PATCH_DIRECTORY" --tree >/dev/null
[ -f "$PATCH" ] || fail "fixture series did not produce $PATCH"
cat >"$MANIFEST" <<EOF
name	version	products	repository	commit	source_paths	patch_series
fixture	1.0	macos	https://example.invalid/fixture.git	$BASE_COMMIT	.	patches/fixture/series.tsv
EOF
cp "$MANIFEST" "$MANIFEST.original"
cp "$PATCH" "$PATCH.original"

required_headers='include/zlib.h
include/zconf.h
include/pcre2.h
include/ffi.h
include/iconv.h
include/libintl.h
include/glib-2.0/glib.h
lib/glib-2.0/include/glibconfig.h
include/slirp/libslirp.h
include/rfb/rfb.h
include/rfb/rfbclient.h
include/SoftFloat-3e/softfloat.h
include/SoftFloat-3e/softfloat_types.h
include/cephes_128bit.h
include/FEXCore/Core/Context.h
include/FEXCore/Utils/AllocatorHooks.h
include/FEXCore/Config/ConfigValues.inl
include/FEXCore/IR/IRDefines.inc
include/fmt/format.h
include/xxhash.h
include/ankerl/unordered_dense.h'
printf '%s\n' "$required_headers" | while IFS= read -r relative; do
	printf '%s\n' fixture >"$BASE_PREFIX/$relative"
done

printf '%s\n' 'int uml_dependency_fixture(void) { return 0; }' \
	>"$TEST_ROOT/fixture.c"
xcrun --sdk macosx clang -arch arm64 \
	-mmacosx-version-min="$DEPLOYMENT_TARGET" -c "$TEST_ROOT/fixture.c" \
	-o "$TEST_ROOT/fixture-arm64.o"
xcrun ar rcs "$TEST_ROOT/fixture-arm64.a" "$TEST_ROOT/fixture-arm64.o"

required_archives='libz.a
libpcre2-8.a
libffi.a
libcharset.a
libiconv.a
libintl.a
libglib-2.0.a
libslirp.a
libvncserver.a
libvncclient.a
libsoftfloat_3e.a
libcephes_128bit.a
libFEXCore.a
libFEXCore_Base.a
libJemallocDummy.a
libfmt.a
libxxhash.a'
printf '%s\n' "$required_archives" | while IFS= read -r archive; do
	cp "$TEST_ROOT/fixture-arm64.a" "$BASE_PREFIX/lib/$archive"
done

cp "$MANIFEST" "$BASE_PREFIX/share/uml-dependencies/manifest.tsv"
FIXTURE_TREE=$(git -C "$PATCH_REPOSITORY" rev-parse 'HEAD^{tree}')
printf 'source\ttree\nfixture\t%s\n' "$FIXTURE_TREE" \
	>"$BASE_PREFIX/share/uml-dependencies/source-trees.tsv"

# GLib installs debugger helpers whose filenames embed an ELF shared-library
# basename but end in -gdb.py. They are scripts, not dynamic libraries.
mkdir -p "$BASE_PREFIX/share/gdb/auto-load/private/prefix/lib"
printf '%s\n' fixture \
	>"$BASE_PREFIX/share/gdb/auto-load/private/prefix/lib/libglib-2.0.so.0-gdb.py"

cat >"$BASE_PREFIX/share/uml-dependencies/receipt.tsv" <<EOF
schema	2
platform	macos
architecture	arm64
linkage	static
deployment_target	$DEPLOYMENT_TARGET
sdk_version	99.0
built_at_utc	2026-08-03T00:00:00Z
compiler	fixture clang
EOF

verify "$BASE_PREFIX" >/dev/null

sed '1s/^From 0\{40\}/From 1111111111111111111111111111111111111111/' \
	"$PATCH.original" >"$PATCH"
expect_failure "patch not produced by the series exporter" verify "$BASE_PREFIX"
cp "$PATCH.original" "$PATCH"

printf '%s\n' stray >"$PATCH_DIRECTORY/unlisted.patch"
expect_failure "unlisted dependency patch" verify "$BASE_PREFIX"
rm "$PATCH_DIRECTORY/unlisted.patch"

mv "$PATCH" "$PATCH.absent"
expect_failure "deleted dependency patch" verify "$BASE_PREFIX"
mv "$PATCH.absent" "$PATCH"

CASE_PREFIX="$TEST_ROOT/missing-file"
copy_fixture "$CASE_PREFIX"
rm "$CASE_PREFIX/include/zlib.h"
expect_failure "missing required file" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/symlinked-header"
copy_fixture "$CASE_PREFIX"
printf '%s\n' ambient >"$TEST_ROOT/ambient-zlib.h"
rm "$CASE_PREFIX/include/zlib.h"
ln -s "$TEST_ROOT/ambient-zlib.h" "$CASE_PREFIX/include/zlib.h"
expect_failure "symlinked required header" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/symlinked-archive"
copy_fixture "$CASE_PREFIX"
rm "$CASE_PREFIX/lib/libz.a"
ln -s "$TEST_ROOT/fixture-arm64.a" "$CASE_PREFIX/lib/libz.a"
expect_failure "symlinked required archive" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/dynamic-library"
copy_fixture "$CASE_PREFIX"
printf '%s\n' forbidden >"$CASE_PREFIX/lib/libforbidden.dylib"
expect_failure "dynamic library" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/disguised-dynamic-library"
copy_fixture "$CASE_PREFIX"
xcrun --sdk macosx clang -arch arm64 -dynamiclib "$TEST_ROOT/fixture.c" \
	-o "$TEST_ROOT/fixture-arm64.dylib"
cp "$TEST_ROOT/fixture-arm64.dylib" "$CASE_PREFIX/lib/hidden.bundle"
expect_failure "dynamic library with a nonstandard suffix" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/nested-invalid-archive"
copy_fixture "$CASE_PREFIX"
mkdir -p "$CASE_PREFIX/lib/plugins"
printf '%s\n' 'not an archive' >"$CASE_PREFIX/lib/plugins/hidden.a"
expect_failure "nested invalid static archive" verify "$CASE_PREFIX"

# Only the first-party executable-memory owner writes MAP_JIT memory.
CASE_PREFIX="$TEST_ROOT/fexcore-jit-writer"
copy_fixture "$CASE_PREFIX"
printf '%s\n' '#include <pthread.h>' \
	'void uml_dependency_jit_fixture(void) { pthread_jit_write_protect_np(0); }' \
	>"$TEST_ROOT/jit-writer.c"
xcrun --sdk macosx clang -arch arm64 \
	-mmacosx-version-min="$DEPLOYMENT_TARGET" -c "$TEST_ROOT/jit-writer.c" \
	-o "$TEST_ROOT/jit-writer.o"
rm "$CASE_PREFIX/lib/libFEXCore_Base.a"
xcrun ar rcs "$CASE_PREFIX/lib/libFEXCore_Base.a" "$TEST_ROOT/jit-writer.o"
expect_failure "FEXCore archive that writes JIT memory itself" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/cmake-package-metadata"
copy_fixture "$CASE_PREFIX"
mkdir -p "$CASE_PREFIX/lib/cmake/Fixture"
printf '%s\n' '# unused export' \
	>"$CASE_PREFIX/lib/cmake/Fixture/FixtureTargets.cmake"
expect_failure "installed CMake package metadata" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/duplicate-receipt"
copy_fixture "$CASE_PREFIX"
printf '%s\n' 'schema	1' \
	>>"$CASE_PREFIX/share/uml-dependencies/receipt.tsv"
expect_failure "duplicate receipt field" verify "$CASE_PREFIX"

# Receipts of prefixes built while the receipt recorded the manifest's hash
# still verify: the rows and trees identify what a prefix was built from.
CASE_PREFIX="$TEST_ROOT/receipt-with-manifest-hash"
copy_fixture "$CASE_PREFIX"
printf 'manifest_sha256\t%s\n' \
	0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef \
	>>"$CASE_PREFIX/share/uml-dependencies/receipt.tsv"
verify "$CASE_PREFIX" >/dev/null ||
	fail "a receipt that records the manifest's hash was rejected"

CASE_PREFIX="$TEST_ROOT/missing-sdk-observation"
copy_fixture "$CASE_PREFIX"
sed '/^sdk_version	/d' "$CASE_PREFIX/share/uml-dependencies/receipt.tsv" \
	>"$CASE_PREFIX/share/uml-dependencies/receipt.tsv.changed"
mv "$CASE_PREFIX/share/uml-dependencies/receipt.tsv.changed" \
	"$CASE_PREFIX/share/uml-dependencies/receipt.tsv"
expect_failure "missing SDK observation" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/unrecorded-manifest"
copy_fixture "$CASE_PREFIX"
rm "$CASE_PREFIX/share/uml-dependencies/manifest.tsv"
expect_failure "prefix without its dependency manifest" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/unrecorded-source-trees"
copy_fixture "$CASE_PREFIX"
rm "$CASE_PREFIX/share/uml-dependencies/source-trees.tsv"
expect_failure "prefix without its patched source trees" verify "$CASE_PREFIX"

# A series change that alters the tree keeps the manifest row, so only the
# recorded tree tells the prefix is stale.
CASE_PREFIX="$TEST_ROOT/other-source-tree"
copy_fixture "$CASE_PREFIX"
printf 'source\ttree\nfixture\t%s\n' \
	"$(git -C "$PATCH_REPOSITORY" rev-parse 'HEAD~1^{tree}')" \
	>"$CASE_PREFIX/share/uml-dependencies/source-trees.tsv"
expect_failure "prefix built from another patched tree" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/headerless-source-trees"
copy_fixture "$CASE_PREFIX"
printf 'fixture\t%s\n' "$FIXTURE_TREE" \
	>"$CASE_PREFIX/share/uml-dependencies/source-trees.tsv"
expect_failure "source trees without their header" verify "$CASE_PREFIX"

# Rows of other products and the products column itself do not identify this
# prefix; a changed macos row does.
printf '%s\n' \
	"other	1.0	ios-network	https://example.invalid/other.git	$BASE_COMMIT	.	-" \
	>>"$MANIFEST"
sed 's/	macos	/	macos,ios-network	/' "$MANIFEST" >"$MANIFEST.changed"
mv "$MANIFEST.changed" "$MANIFEST"
verify "$BASE_PREFIX" >/dev/null ||
	fail "another product's row or a shared products column rejected the prefix"
cp "$MANIFEST.original" "$MANIFEST"
sed 's/	1\.0	macos	/	1.1	macos	/' "$MANIFEST.original" >"$MANIFEST"
expect_failure "changed macos dependency row" verify "$BASE_PREFIX"
cp "$MANIFEST.original" "$MANIFEST"

CASE_PREFIX="$TEST_ROOT/manifest-tamper"
copy_fixture "$CASE_PREFIX"
printf '%s\n' changed >>"$MANIFEST"
expect_failure "changed dependency manifest" verify "$CASE_PREFIX"
cp "$MANIFEST.original" "$MANIFEST"

CASE_PREFIX="$TEST_ROOT/wrong-architecture"
copy_fixture "$CASE_PREFIX"
xcrun --sdk macosx clang -arch x86_64 -c "$TEST_ROOT/fixture.c" \
	-o "$TEST_ROOT/fixture-x86_64.o"
xcrun ar rcs "$TEST_ROOT/fixture-x86_64.a" "$TEST_ROOT/fixture-x86_64.o"
cp "$TEST_ROOT/fixture-x86_64.a" "$CASE_PREFIX/lib/libz.a"
expect_failure "non-arm64 static archive" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/renamed-dynamic-library"
copy_fixture "$CASE_PREFIX"
cp "$TEST_ROOT/fixture-arm64.dylib" "$CASE_PREFIX/lib/libz.a"
expect_failure "dynamic library renamed as an archive" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/wrong-deployment-target"
copy_fixture "$CASE_PREFIX"
DEPLOYMENT_TARGET=15.0
expect_failure "incompatible deployment target" verify "$CASE_PREFIX"
DEPLOYMENT_TARGET=27.0

printf '%s\n' "dependency prefix verification tests passed"
