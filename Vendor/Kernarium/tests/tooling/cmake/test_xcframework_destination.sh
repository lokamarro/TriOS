#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

SCRIPT=${1:?XCFramework build script is required}
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-xcframework-destination.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

TEST_NAME=test_xcframework_destination.sh
. "$(dirname -- "$0")/../support/expect.sh"

INVALID_OUTPUT="$TEST_ROOT/not-a-package"
if UML_XCFRAMEWORK_BUILD_DIR="$TEST_ROOT/build" \
	UML_XCFRAMEWORK_OUTPUT="$INVALID_OUTPUT" \
	"$SCRIPT" >"$TEST_ROOT/invalid.out" 2>"$TEST_ROOT/invalid.err"; then
	fail "an output without the supported XCFramework name was accepted"
fi
grep -F 'XCFramework output must be named UMLEmulator.xcframework' \
	"$TEST_ROOT/invalid.err" >/dev/null ||
	fail "invalid output rejection lacked the expected diagnostic"
[ ! -e "$INVALID_OUTPUT" ] ||
	fail "invalid output was created before rejection"

PACKAGE_OUTPUT="$TEST_ROOT/UMLEmulator.xcframework"
mkdir -p -- "$PACKAGE_OUTPUT"
printf '%s\n' preserved >"$PACKAGE_OUTPUT/marker"
if UML_XCFRAMEWORK_BUILD_DIR="$PACKAGE_OUTPUT/build" \
	UML_XCFRAMEWORK_OUTPUT="$PACKAGE_OUTPUT" \
	"$SCRIPT" >"$TEST_ROOT/nested.out" 2>"$TEST_ROOT/nested.err"; then
	fail "a package containing its own build root was accepted"
fi
grep -F 'XCFramework output may not contain its slice build root' \
	"$TEST_ROOT/nested.err" >/dev/null ||
	fail "nested build-root rejection lacked the expected diagnostic"
[ "$(cat "$PACKAGE_OUTPUT/marker")" = preserved ] ||
	fail "rejected package destination was modified"

printf '%s\n' "XCFramework destination safety tests passed"
