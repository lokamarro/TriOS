#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Every dependency verification mode is read-only: verifying an absent prefix
# fails with a diagnostic and leaves the filesystem exactly as it was. A build
# verifies its inputs before it creates anything, so a build from an
# unprepared source root fails the same way.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
DEPENDENCIES="$ROOT_DIR/dependencies"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-verify-read-only.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

TEST_NAME=test-verify-read-only.sh
. "$(dirname -- "$0")/../support/expect.sh"

# Every path a verifier could resolve lies below SCRATCH, whose entries and
# modification times are compared around each run, so a directory created
# and removed again is also caught. SOURCES is an empty, unprepared source
# root.
SCRATCH="$TEST_ROOT/scratch"
SOURCES="$SCRATCH/sources"
CACHE="$SCRATCH/cache"
WORK="$SCRATCH/work"
mkdir -p "$SCRATCH/parent" "$SOURCES"

snapshot()
{
	find "$SCRATCH" -exec stat -f '%Fm %N' {} + | LC_ALL=C sort
}

expect_unchanged()
{
	after=$(snapshot)
	[ "$after" = "$1" ] || {
		printf '%s\n' "before:" "$1" "after:" "$after" >&2
		fail "$2 changed the filesystem"
	}
}

# expect_read_only_failure DESCRIPTION COMMAND...: COMMAND must fail with a
# diagnostic and write nothing.
expect_read_only_failure()
{
	before=$(snapshot)
	expect_failure "$@"
	expect_unchanged "$before" "$1"
}

# A missing prefix whose parent exists, and one whose parent is missing too.
for PREFIX in "$SCRATCH/parent/absent" "$SCRATCH/absent-parent/prefix"; do
	expect_read_only_failure "iOS network verification of $PREFIX" \
		"$DEPENDENCIES/build-ios-network.sh" --verify-only \
		--prefix "$PREFIX" --source-root "$SOURCES" --work-root "$WORK"
	expect_read_only_failure "Apple GPU verification of $PREFIX" \
		"$DEPENDENCIES/build-apple-gpu.sh" --verify-only \
		--prefix "$PREFIX" --source-root "$SOURCES" --work-root "$WORK"
	expect_read_only_failure "iOS storage verification of $PREFIX" \
		"$DEPENDENCIES/build-ios-storage.sh" --verify-only \
		--output "$PREFIX" --source-root "$SOURCES" --work-root "$WORK"
	expect_read_only_failure "iOS zstd verification of $PREFIX" \
		"$DEPENDENCIES/build-ios-zstd.sh" --verify-only \
		--output "$PREFIX" --source-root "$SOURCES" --work-root "$WORK"
	expect_read_only_failure "kernel host tools verification of $PREFIX" \
		"$DEPENDENCIES/build-kernel-host-tools.sh" --verify-only \
		--output "$PREFIX" --source-root "$SOURCES" --cache "$CACHE" \
		--work-root "$WORK"
	expect_read_only_failure "macOS prefix verification of $PREFIX" \
		"$DEPENDENCIES/verify-prefix.sh" --prefix "$PREFIX" \
		--manifest "$DEPENDENCIES/manifest.tsv" --deployment-target 27.0
	expect_read_only_failure "runtime closure verification of $PREFIX" \
		"$DEPENDENCIES/verify-runtime-dependencies.sh" \
		--binary "$PREFIX/uml-darwin" --apple-gpu-prefix "$PREFIX" \
		--framework-directory "$PREFIX/Frameworks"
	expect_read_only_failure "source verification of $PREFIX" \
		"$DEPENDENCIES/prepare.sh" --verify --product all \
		--root "$PREFIX" --cache "$CACHE"
done

# The build script's manifest check reads only the manifest.
BEFORE=$(snapshot)
"$DEPENDENCIES/build.sh" --check-manifest-only \
	--prefix "$SCRATCH/absent-parent/prefix" --source-root "$SOURCES" \
	--work-root "$WORK" >/dev/null || fail "the manifest check failed"
expect_unchanged "$BEFORE" "the build manifest check"

# expect_build_refused DESCRIPTION COMMAND...: COMMAND must reject the
# unprepared source root and write nothing. The builders look up their build
# tools first, so those must be on PATH.
expect_build_refused()
{
	refused=$1
	shift
	before=$(snapshot)
	expect_failure "$refused" "$@"
	grep -F 'is not a prepared Git repository' \
		"$TEST_ROOT/rejected-$FAILURE_NUMBER.err" >/dev/null || {
		cat "$TEST_ROOT/rejected-$FAILURE_NUMBER.err" >&2
		fail "$refused failed before it verified its sources"
	}
	expect_unchanged "$before" "$refused"
}

# Neither the prefix's parent nor the work root may appear.
PREFIX="$SCRATCH/absent-parent/prefix"
expect_build_refused "macOS dependency build" \
	"$DEPENDENCIES/build.sh" \
	--prefix "$PREFIX" --source-root "$SOURCES" --work-root "$WORK"
expect_build_refused "iOS network build" \
	"$DEPENDENCIES/build-ios-network.sh" \
	--prefix "$PREFIX" --source-root "$SOURCES" --work-root "$WORK"
expect_build_refused "Apple GPU build" \
	"$DEPENDENCIES/build-apple-gpu.sh" \
	--prefix "$PREFIX" --source-root "$SOURCES" --work-root "$WORK"
expect_build_refused "iOS storage build" \
	"$DEPENDENCIES/build-ios-storage.sh" \
	--output "$PREFIX" --source-root "$SOURCES" --work-root "$WORK"
expect_build_refused "iOS zstd build" \
	"$DEPENDENCIES/build-ios-zstd.sh" \
	--output "$PREFIX" --source-root "$SOURCES" --work-root "$WORK"
expect_build_refused "kernel host tools build" \
	"$DEPENDENCIES/build-kernel-host-tools.sh" \
	--output "$PREFIX" --source-root "$SOURCES" --cache "$CACHE" \
	--work-root "$WORK"

printf '%s\n' "dependency verification read-only tests passed"
