#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

DEPENDENCY_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../dependencies" && pwd)
BUILD="$DEPENDENCY_DIR/build.sh"
SOURCE_MANIFEST="$DEPENDENCY_DIR/manifest.tsv"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-dependency-recipes.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

TEST_NAME=test-build-manifest.sh
. "$(dirname -- "$0")/../support/expect.sh"

check_manifest()
{
	UML_DEPENDENCY_MANIFEST="$1" "$BUILD" --check-manifest-only
}

check_manifest "$SOURCE_MANIFEST" >/dev/null

MISSING="$TEST_ROOT/missing.tsv"
awk -F '\t' '$1 != "zlib" { print }' "$SOURCE_MANIFEST" >"$MISSING"
expect_failure "missing build recipe" check_manifest "$MISSING"

EXTRA="$TEST_ROOT/extra.tsv"
awk -F '\t' 'BEGIN { OFS = FS }
	{ print }
	$1 == "zlib" { $1 = "unexpected"; print }
' "$SOURCE_MANIFEST" >"$EXTRA"
expect_failure "extra manifest recipe" check_manifest "$EXTRA"

REORDERED="$TEST_ROOT/reordered.tsv"
awk -F '\t' '
	$1 == "zlib" { saved = $0; next }
	{ print }
	END { print saved }
' "$SOURCE_MANIFEST" >"$REORDERED"
expect_failure "reordered manifest recipes" check_manifest "$REORDERED"

VERSION="$TEST_ROOT/version.tsv"
awk -F '\t' 'BEGIN { OFS = FS }
	$1 == "zlib" { $2 = "1.3.1" }
	{ print }
' "$SOURCE_MANIFEST" >"$VERSION"
expect_failure "recipe version drift" check_manifest "$VERSION"

MALFORMED="$TEST_ROOT/malformed.tsv"
awk -F '\t' 'BEGIN { OFS = FS }
	$1 == "zlib" { NF = 6 }
	{ print }
' "$SOURCE_MANIFEST" >"$MALFORMED"
expect_failure "malformed manifest row" check_manifest "$MALFORMED"

printf '%s\n' "dependency build recipe manifest tests passed"
