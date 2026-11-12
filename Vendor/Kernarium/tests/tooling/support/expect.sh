# SPDX-License-Identifier: GPL-2.0-or-later
# Assertions shared by the shell tooling tests. A test sets TEST_NAME, which
# prefixes every failure, and sources this file. expect_failure and
# expect_diagnostic keep a rejected command's output in
# $TEST_ROOT/rejected-$FAILURE_NUMBER.out and .err for the caller to inspect.

: "${TEST_NAME:?a test sets TEST_NAME before sourcing expect.sh}"
FAILURE_NUMBER=0

fail()
{
	printf '%s\n' "$TEST_NAME: $*" >&2
	exit 1
}

# expect_failure DESCRIPTION COMMAND...: COMMAND must fail and explain why on
# standard error.
expect_failure()
{
	description=$1
	shift
	FAILURE_NUMBER=$((FAILURE_NUMBER + 1))
	if "$@" >"$TEST_ROOT/rejected-$FAILURE_NUMBER.out" \
		2>"$TEST_ROOT/rejected-$FAILURE_NUMBER.err"; then
		fail "$description was incorrectly accepted"
	fi
	[ -s "$TEST_ROOT/rejected-$FAILURE_NUMBER.err" ] ||
		fail "$description was rejected without a diagnostic"
}

# expect_diagnostic PATTERN COMMAND...: COMMAND must fail, and its standard
# error must contain PATTERN.
expect_diagnostic()
{
	pattern=$1
	shift
	expect_failure "$pattern" "$@"
	grep -F -- "$pattern" "$TEST_ROOT/rejected-$FAILURE_NUMBER.err" >/dev/null || {
		cat "$TEST_ROOT/rejected-$FAILURE_NUMBER.err" >&2
		fail "rejection lacked the expected diagnostic: $pattern"
	}
}

# expect_cmake_diagnostic PATTERN COMMAND...: as expect_diagnostic, for a
# CMake script whose message() wraps long text at whitespace. PATTERN is matched
# with every run of whitespace in standard error collapsed to one space.
expect_cmake_diagnostic()
{
	pattern=$1
	shift
	expect_failure "$pattern" "$@"
	tr -s '[:space:]' ' ' <"$TEST_ROOT/rejected-$FAILURE_NUMBER.err" |
		grep -F -- "$pattern" >/dev/null || {
		cat "$TEST_ROOT/rejected-$FAILURE_NUMBER.err" >&2
		fail "rejection lacked the expected diagnostic: $pattern"
	}
}
