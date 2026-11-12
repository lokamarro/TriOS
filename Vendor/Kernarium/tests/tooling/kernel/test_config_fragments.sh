#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Kernel configuration composition and verification from
# kernel/common/tooling/config-fragments.sh, run against a fake Linux tree whose
# scripts/config records its arguments, and the split of the maintained policy
# into a shared fragment and per-line overlays.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-kernel-config-fragments.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

TEST_NAME=test_config_fragments.sh
. "$(dirname -- "$0")/../support/expect.sh"
# shellcheck disable=SC1091
. "$ROOT_DIR/kernel/common/tooling/config-fragments.sh"

SOURCE="$TEST_ROOT/linux"
CONFIG="$TEST_ROOT/.config"
CONFIG_LOG="$TEST_ROOT/scripts-config.log"
export CONFIG_LOG
mkdir -p "$SOURCE/scripts"
cat >"$SOURCE/scripts/config" <<'EOF'
#!/bin/sh
for argument do
	printf '[%s]' "$argument"
	[ "$argument" != FAILS ] || {
		printf '%s\n' "scripts/config: cannot set $argument" >&2
		exit 1
	}
done >>"$CONFIG_LOG"
printf '\n' >>"$CONFIG_LOG"
EOF
chmod +x "$SOURCE/scripts/config"

# The base is copied verbatim; each later fragment line becomes one
# scripts/config call, in order, and comments and blank lines become none.
cat >"$TEST_ROOT/base.fragment" <<'EOF'
# SPDX-License-Identifier: GPL-2.0-or-later
CONFIG_SHARED=y
CONFIG_NR=32
CONFIG_ARM_PMU=n
CONFIG_SSL_CHAN="fd:0,fd:1"
# CONFIG_OFF is not set
EOF
cat >"$TEST_ROOT/overlay.fragment" <<'EOF'
# A comment naming CONFIG_IGNORED=y is not a request.

CONFIG_ENABLED=y
CONFIG_FONT_8x16=y
CONFIG_NR=64
CONFIG_LSM="landlock,smack"
CONFIG_EMPTY=""
# CONFIG_DISABLED is not set
EOF
printf '%s\n' 'CONFIG_PAGE=y' >"$TEST_ROOT/page.fragment"
uml_kernel_config_compose "$SOURCE" "$CONFIG" "$TEST_ROOT/base.fragment" \
	"$TEST_ROOT/overlay.fragment" "$TEST_ROOT/page.fragment" ||
	fail "a valid fragment list did not compose"
cmp -s "$TEST_ROOT/base.fragment" "$CONFIG" ||
	fail "the base fragment was not copied verbatim"
cat >"$TEST_ROOT/expected.log" <<EOF
[--file][$CONFIG][--keep-case][-e][ENABLED]
[--file][$CONFIG][--keep-case][-e][FONT_8x16]
[--file][$CONFIG][--keep-case][--set-val][NR][64]
[--file][$CONFIG][--keep-case][--set-str][LSM][landlock,smack]
[--file][$CONFIG][--keep-case][--set-str][EMPTY][]
[--file][$CONFIG][--keep-case][-d][DISABLED]
[--file][$CONFIG][--keep-case][-e][PAGE]
EOF
cmp -s "$TEST_ROOT/expected.log" "$CONFIG_LOG" || {
	diff -u "$TEST_ROOT/expected.log" "$CONFIG_LOG" >&2 || :
	fail "fragment lines did not become the expected scripts/config calls"
}

# Callers test the status, which suspends errexit inside the functions, so a
# rejection must still stop composition before the rejected line is applied.
for line in \
	'CONFIG_MODULE=m' \
	'CONFIG_OFF=n' \
	'CONFIG_HEX=0x10' \
	'CONFIG_NEGATIVE=-1' \
	'CONFIG_BLANK=' \
	'CONFIG_QUOTE="a"b"' \
	'CONFIG_BACKSLASH="a\b"' \
	'CONFIG_AMPERSAND="a&b"' \
	'CONFIG_UNTERMINATED="a' \
	'CONFIG_NO_VALUE' \
	'NOT_A_SYMBOL=y'; do
	printf '%s\n' "$line" 'CONFIG_AFTER=y' >"$TEST_ROOT/rejected.fragment"
	: >"$CONFIG_LOG"
	expect_diagnostic "unsupported kernel config fragment line in $TEST_ROOT/rejected.fragment: $line" \
		uml_kernel_config_compose "$SOURCE" "$CONFIG" \
		"$TEST_ROOT/base.fragment" "$TEST_ROOT/rejected.fragment" \
		"$TEST_ROOT/page.fragment"
	[ ! -s "$CONFIG_LOG" ] ||
		fail "composition continued past the rejected line: $line"
done
printf '%s\n' 'CONFIG_FAILS=y' 'CONFIG_AFTER=y' >"$TEST_ROOT/failing.fragment"
: >"$CONFIG_LOG"
expect_failure "a failing scripts/config call" \
	uml_kernel_config_compose "$SOURCE" "$CONFIG" \
	"$TEST_ROOT/base.fragment" "$TEST_ROOT/failing.fragment" \
	"$TEST_ROOT/page.fragment"
if grep -E 'AFTER|PAGE' "$CONFIG_LOG" >/dev/null; then
	fail "composition continued after scripts/config failed"
fi
# Every fragment is checked before the base is copied.
rm -f -- "$CONFIG"
expect_diagnostic "missing kernel config fragment: $TEST_ROOT/missing.fragment" \
	uml_kernel_config_compose "$SOURCE" "$CONFIG" \
	"$TEST_ROOT/base.fragment" "$TEST_ROOT/missing.fragment"
[ ! -e "$CONFIG" ] || fail "a missing fragment still started composition"
expect_diagnostic "no kernel config fragments to compose" \
	uml_kernel_config_compose "$SOURCE" "$CONFIG"

# Verification compares each request with the resolved configuration. The
# last fragment naming a symbol owns it, and an absent symbol or a "not set"
# comment both satisfy a request to disable it.
cat >"$TEST_ROOT/resolved.config" <<'EOF'
#
# Automatically generated file; DO NOT EDIT.
#
CONFIG_SHARED=y
CONFIG_NR=64
CONFIG_LSM="landlock,smack"
CONFIG_SELECTED=y
# CONFIG_DISABLED is not set
EOF
cat >"$TEST_ROOT/requested-base.fragment" <<'EOF'
CONFIG_SHARED=y
CONFIG_NR=32
# CONFIG_ABSENT is not set
EOF
cat >"$TEST_ROOT/requested-overlay.fragment" <<'EOF'
CONFIG_NR=64
CONFIG_LSM="landlock,smack"
# CONFIG_DISABLED is not set
EOF
uml_kernel_config_verify "$TEST_ROOT/resolved.config" \
	"$TEST_ROOT/requested-base.fragment" \
	"$TEST_ROOT/requested-overlay.fragment" ||
	fail "a configuration matching its fragments was rejected"

check_mismatch()
{
	request=$1
	diagnostic=$2
	printf '%s\n' "$request" >"$TEST_ROOT/mismatch.fragment"
	expect_diagnostic "$diagnostic" \
		uml_kernel_config_verify "$TEST_ROOT/resolved.config" \
		"$TEST_ROOT/requested-base.fragment" \
		"$TEST_ROOT/requested-overlay.fragment" \
		"$TEST_ROOT/mismatch.fragment"
	grep -F 'the resolved kernel configuration differs from its fragments:' \
		"$TEST_ROOT/rejected-$FAILURE_NUMBER.err" >/dev/null ||
		fail "a mismatch lacked the summary line: $request"
}
check_mismatch 'CONFIG_DROPPED=y' \
	"CONFIG_DROPPED: $TEST_ROOT/mismatch.fragment requested y, resolved absent"
check_mismatch '# CONFIG_SELECTED is not set' \
	"CONFIG_SELECTED: $TEST_ROOT/mismatch.fragment requested n, resolved y"
check_mismatch 'CONFIG_NR=32' \
	"CONFIG_NR: $TEST_ROOT/mismatch.fragment requested 32, resolved 64"
check_mismatch 'CONFIG_LSM="landlock,selinux"' \
	"CONFIG_LSM: $TEST_ROOT/mismatch.fragment requested \"landlock,selinux\", resolved \"landlock,smack\""

# Every product starts from the shared policy, and each product's overlay lists
# only where that product differs: an overlay never names a shared symbol, and
# a setting both overlays make identically belongs in the shared policy.
policy_problems=$(awk '
	/^CONFIG_[A-Za-z0-9_]+=/ {
		symbol = substr($0, 1, index($0, "=") - 1)
		value = substr($0, index($0, "=") + 1)
	}
	/^# CONFIG_[A-Za-z0-9_]+ is not set$/ { symbol = $2; value = "n" }
	!/^CONFIG_[A-Za-z0-9_]+=/ && !/^# CONFIG_[A-Za-z0-9_]+ is not set$/ { next }
	FILENAME == ARGV[1] { shared[symbol] = 1; next }
	symbol in shared { print FILENAME ": " symbol " repeats the shared policy" }
	FILENAME == ARGV[2] { first[symbol] = value; next }
	(symbol in first) && first[symbol] == value {
		print symbol " is set identically in both overlays"
	}
' "$ROOT_DIR/kernel/common/config/darwin-uml.fragment" \
	"$ROOT_DIR/kernel/mainline-6.18/config/mainline.fragment" \
	"$ROOT_DIR/kernel/mainline-6.18/config/android.fragment" </dev/null)
[ -z "$policy_problems" ] || {
	printf '%s\n' "$policy_problems" >&2
	fail "a product overlay repeats shared kernel configuration"
}

# The Android product's symbol lists agree with the fragments that compose it:
# no listed symbol is both required and forbidden, and neither the shared
# policy nor the Android overlay requests the opposite of a list.
ANDROID_CONFIG_DIR="$ROOT_DIR/kernel/mainline-6.18/config"
list_problems=$(awk '
	function symbol_of(line) {
		if (match(line, /^CONFIG_[A-Za-z0-9_]+=/))
			return substr(line, 1, index(line, "=") - 1)
		if (match(line, /^# CONFIG_[A-Za-z0-9_]+ is not set$/)) {
			split(line, words, " ")
			return words[2]
		}
		return ""
	}
	function value_of(line) {
		if (line ~ /^# /)
			return "n"
		return substr(line, index(line, "=") + 1)
	}
	/^[[:space:]]*(#|$)/ && !/^# CONFIG_[A-Za-z0-9_]+ is not set$/ { next }
	FILENAME == ARGV[1] { required[$1] = 1; next }
	FILENAME == ARGV[2] {
		forbidden[$1] = 1
		if ($1 in required)
			print $1 " is both required and forbidden"
		next
	}
	{
		symbol = symbol_of($0)
		if (symbol == "")
			next
		value = value_of($0)
		if ((symbol in required) && value != "y")
			print FILENAME ": " symbol "=" value " but the Android product requires y"
		if ((symbol in forbidden) && value != "n")
			print FILENAME ": " symbol "=" value " but the Android product forbids it"
	}
' "$ANDROID_CONFIG_DIR/android-required-builtins.txt" \
	"$ANDROID_CONFIG_DIR/android-forbidden-options.txt" \
	"$ROOT_DIR/kernel/common/config/darwin-uml.fragment" \
	"$ANDROID_CONFIG_DIR/android.fragment" </dev/null)
[ -z "$list_problems" ] || {
	printf '%s\n' "$list_problems" >&2
	fail "the Android symbol lists contradict the fragments"
}

printf '%s\n' "kernel config fragment composition tests passed"
