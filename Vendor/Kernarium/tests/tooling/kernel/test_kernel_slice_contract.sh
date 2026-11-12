#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/mainline-6.18"
COMMON_METADATA="$LINE_ROOT/patches/metadata.env"
SLICE_ROOT="$LINE_ROOT/slices/x86_64"
SLICE_METADATA="$SLICE_ROOT/patches/metadata.env"
KERNEL_SERIES="$ROOT_DIR/kernel/common/tooling/kernel_series.py"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-kernel-slices.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM
WRONG_TREE=ffffffffffffffffffffffffffffffffffffffff

TEST_NAME='kernel slice contract fixture'
. "$(dirname -- "$0")/../support/expect.sh"

require_file()
{
	[ -f "$1" ] || fail "required slice owner is missing: $1"
}

for path in \
	"$COMMON_METADATA" \
	"$LINE_ROOT/scripts/select-slice.sh" \
	"$SLICE_METADATA" \
	"$KERNEL_SERIES"; do
	require_file "$path"
done

# shellcheck disable=SC1090
. "$COMMON_METADATA"
COMMON_SERIES_LIST=$UML_KERNEL_SERIES
COMMON_PATCH_BRANCH=${UML_KERNEL_SERIES##*:}
COMMON_RELEASE_VERSION=$UML_KERNEL_RELEASE_VERSION
COMMON_UPSTREAM_REPOSITORY=$UML_KERNEL_UPSTREAM_REPOSITORY
COMMON_UPSTREAM_BRANCH=$UML_KERNEL_UPSTREAM_BRANCH
COMMON_UPSTREAM_COMMIT=$UML_KERNEL_UPSTREAM_COMMIT

# The x86_64 slice is a true overlay whose series follow the arm64 series.
# shellcheck disable=SC1090
. "$SLICE_METADATA"
[ "${UML_KERNEL_SLICE:-}" = x86_64 ] ||
	fail "x86_64 overlay metadata has the wrong slice identity"
case "$UML_KERNEL_SERIES" in
"$COMMON_SERIES_LIST "*) ;;
*) fail "x86_64 series do not start with the arm64 series" ;;
esac
[ "$UML_KERNEL_OWNED_SERIES" = "kernel/mainline-6.18/slices/x86_64/patches/series" ] ||
	fail "x86_64 overlay owns more than its own series"
SLICE_PATCH_BRANCH=${UML_KERNEL_SERIES##*:}
[ "$SLICE_PATCH_BRANCH" != "$COMMON_PATCH_BRANCH" ] ||
	fail "common and x86_64 patches use the same maintained Git branch"

# shellcheck disable=SC1090
. "$LINE_ROOT/scripts/select-slice.sh"
uml_kernel_select_slice "$LINE_ROOT" arm64 ||
	fail "slice selector cannot load the normal arm64 product"
[ "$UML_KERNEL_METADATA_PATH" = "$COMMON_METADATA" ] &&
	[ "$UML_KERNEL_PATCH_BRANCH" = "$COMMON_PATCH_BRANCH" ] &&
	[ -z "$UML_KERNEL_PARENT_BRANCH" ] &&
	[ -z "$UML_KERNEL_SLICE_CONFIG_FRAGMENT" ] ||
	fail "arm64 selection does not resolve to the shared series/config"
uml_kernel_select_slice "$LINE_ROOT" x86_64 ||
	fail "slice selector cannot load the x86_64 overlay"
[ "$UML_KERNEL_METADATA_PATH" = "$SLICE_METADATA" ] &&
	[ "$UML_KERNEL_PATCH_BRANCH" = "$SLICE_PATCH_BRANCH" ] &&
	[ "$UML_KERNEL_PARENT_BRANCH" = "$COMMON_PATCH_BRANCH" ] &&
	[ "$UML_KERNEL_UPSTREAM_COMMIT" = "$COMMON_UPSTREAM_COMMIT" ] &&
	[ "$UML_KERNEL_SLICE_CONFIG_FRAGMENT" = \
		"$SLICE_ROOT/config/userspace.fragment" ] ||
	fail "x86_64 selection does not compose the overlay series/config"

# The overlay takes the common upstream identity, and the series loader that
# every producer runs first rejects an overlay that restates any of it.
RESTATED_METADATA="$TEST_ROOT/restated-x86_64.env"
{
	cat "$SLICE_METADATA"
	printf '%s\n' "UML_KERNEL_UPSTREAM_COMMIT='$WRONG_TREE'"
} > "$RESTATED_METADATA"
if python3 -B "$KERNEL_SERIES" check --metadata "$RESTATED_METADATA" \
	>"$TEST_ROOT/restated.out" 2>"$TEST_ROOT/restated.err"; then
	fail "series loader accepted an overlay with its own upstream identity"
fi
grep -F 'restates UML_KERNEL_UPSTREAM_COMMIT' "$TEST_ROOT/restated.err" \
	>/dev/null || fail "restated overlay identity failed for the wrong reason"

PREPARE_SOURCE="$LINE_ROOT/scripts/prepare-source.sh"

# Exercise recreation against real Git histories while stubbing only unrelated
# source materialization and ABI verification. The old replay has a different
# subject and content from the checked-in replacement, which is the normal
# stale-series case that --recreate must repair.
PREPARE_FIXTURE="$TEST_ROOT/prepare-fixture"
PREPARE_TEMPLATE_SOURCE="$TEST_ROOT/prepare-template-source"
PREPARE_PATCH_DIR="$PREPARE_FIXTURE/kernel/mainline-6.18/patches/series"
PREPARE_METADATA="$PREPARE_FIXTURE/kernel/mainline-6.18/patches/metadata.env"
PREPARE_SCRIPT="$PREPARE_FIXTURE/kernel/mainline-6.18/scripts/prepare-source.sh"
PREPARE_GIT_BIN="$PREPARE_FIXTURE/bin"
mkdir -p \
	"$PREPARE_PATCH_DIR" \
	"$PREPARE_FIXTURE/kernel/mainline-6.18/scripts" \
	"$PREPARE_FIXTURE/kernel/common/abi" \
	"$PREPARE_FIXTURE/kernel/common/artifacts" \
	"$PREPARE_FIXTURE/kernel/common/tooling" \
	"$PREPARE_FIXTURE/scripts" \
	"$PREPARE_GIT_BIN"
cp "$PREPARE_SOURCE" "$PREPARE_SCRIPT"
# The replay helpers resolve their series tooling from the tree they ship in,
# so the fixture tree carries the real helpers.
cp "$ROOT_DIR/scripts/patch_series.py" "$PREPARE_FIXTURE/scripts/"
cp "$KERNEL_SERIES" "$PREPARE_FIXTURE/kernel/common/tooling/kernel_series.py"
cp "$ROOT_DIR/kernel/common/artifacts/contract_checks.py" \
	"$PREPARE_FIXTURE/kernel/common/artifacts/contract_checks.py"
cp "$LINE_ROOT/scripts/select-slice.sh" \
	"$PREPARE_FIXTURE/kernel/mainline-6.18/scripts/select-slice.sh"
for stub in \
	"$PREPARE_FIXTURE/kernel/mainline-6.18/scripts/materialize-git.sh" \
	"$PREPARE_FIXTURE/kernel/common/abi/sync.sh"; do
	{
		printf '%s\n' '#!/bin/sh'
		printf '%s\n' 'exit 0'
	} > "$stub"
	chmod +x "$stub"
done
{
	printf '%s\n' '#!/bin/sh'
	printf '%s\n' 'set -eu'
	printf '%s\n' ': "${UML_KERNEL_SLICE_FIXTURE_SOURCE:?}"'
	printf '%s\n' 'printf "%s\n" "$UML_KERNEL_SLICE_FIXTURE_SOURCE"'
} > "$PREPARE_FIXTURE/kernel/mainline-6.18/scripts/source-location.sh"
chmod +x \
	"$PREPARE_FIXTURE/kernel/mainline-6.18/scripts/source-location.sh"

git init -q "$PREPARE_TEMPLATE_SOURCE"
printf '%s\n' upstream > "$PREPARE_TEMPLATE_SOURCE/base.txt"
git -C "$PREPARE_TEMPLATE_SOURCE" add base.txt
git -C "$PREPARE_TEMPLATE_SOURCE" commit -qm 'fixture upstream'
git -C "$PREPARE_TEMPLATE_SOURCE" branch -M upstream-fixture
PREPARE_UPSTREAM_COMMIT=$(git -C "$PREPARE_TEMPLATE_SOURCE" rev-parse HEAD)
PREPARE_UPSTREAM_TREE=$(git -C "$PREPARE_TEMPLATE_SOURCE" \
	rev-parse 'HEAD^{tree}')
git -C "$PREPARE_TEMPLATE_SOURCE" switch -q -c canonical-fixture
printf '%s\n' canonical > "$PREPARE_TEMPLATE_SOURCE/canonical.txt"
git -C "$PREPARE_TEMPLATE_SOURCE" add canonical.txt
git -C "$PREPARE_TEMPLATE_SOURCE" commit -qm 'fixture current topic'
PREPARE_CANONICAL_TREE=$(git -C "$PREPARE_TEMPLATE_SOURCE" \
	rev-parse 'HEAD^{tree}')
python3 -B "$ROOT_DIR/scripts/patch_series.py" export \
	--source "$PREPARE_TEMPLATE_SOURCE" --base "$PREPARE_UPSTREAM_COMMIT" \
	--head canonical-fixture --series-dir "$PREPARE_PATCH_DIR" >/dev/null ||
	fail "cannot export the recreate fixture series"
git -C "$PREPARE_TEMPLATE_SOURCE" switch -q upstream-fixture
git -C "$PREPARE_TEMPLATE_SOURCE" switch -q -c managed-fixture
printf '%s\n' stale > "$PREPARE_TEMPLATE_SOURCE/stale.txt"
git -C "$PREPARE_TEMPLATE_SOURCE" add stale.txt
git -C "$PREPARE_TEMPLATE_SOURCE" commit -qm 'fixture stale topic'
PREPARE_STALE_TIP=$(git -C "$PREPARE_TEMPLATE_SOURCE" rev-parse HEAD)

write_prepare_metadata()
{
	prepare_final_tree=$1
	printf '%s\n' \
		'UML_KERNEL_SERIES_SCHEMA=4' \
		"UML_KERNEL_RELEASE_VERSION='$COMMON_RELEASE_VERSION'" \
		"UML_KERNEL_UPSTREAM_REPOSITORY='$COMMON_UPSTREAM_REPOSITORY'" \
		"UML_KERNEL_UPSTREAM_BRANCH='$COMMON_UPSTREAM_BRANCH'" \
		"UML_KERNEL_UPSTREAM_COMMIT='$PREPARE_UPSTREAM_COMMIT'" \
		"UML_KERNEL_UPSTREAM_TREE='$PREPARE_UPSTREAM_TREE'" \
		"UML_KERNEL_BASE_REF='refs/heads/upstream-fixture'" \
		"UML_KERNEL_SERIES='kernel/mainline-6.18/patches/series:managed-fixture'" \
		"UML_KERNEL_OWNED_SERIES='kernel/mainline-6.18/patches/series'" \
		"UML_KERNEL_FINAL_TREE='$prepare_final_tree'" \
		> "$PREPARE_METADATA"
}

initialize_prepare_source()
{
	prepare_source=$1
	git clone -q "$PREPARE_TEMPLATE_SOURCE" "$prepare_source"
	git -C "$prepare_source" remote remove origin
	git -C "$prepare_source" branch -f upstream-fixture \
		"$PREPARE_UPSTREAM_COMMIT"
	git -C "$prepare_source" switch -q managed-fixture
	git -C "$prepare_source" config uml.source.releaseVersion \
		"$COMMON_RELEASE_VERSION"
	git -C "$prepare_source" config uml.source.upstreamRepository \
		"$COMMON_UPSTREAM_REPOSITORY"
	git -C "$prepare_source" config uml.source.upstreamBranch \
		"$COMMON_UPSTREAM_BRANCH"
	git -C "$prepare_source" config uml.source.upstreamCommit \
		"$PREPARE_UPSTREAM_COMMIT"
	git -C "$prepare_source" config uml.source.upstreamTree \
		"$PREPARE_UPSTREAM_TREE"
	git -C "$prepare_source" config uml.source.baseRef \
		'refs/heads/upstream-fixture'
}

require_no_replacement_branch()
{
	prepare_source=$1
	replacement_refs=$(git -C "$prepare_source" for-each-ref \
		--format='%(refname)' 'refs/heads/managed-fixture.recreate-*')
	[ -z "$replacement_refs" ] ||
		fail "failed recreation retained temporary branch $replacement_refs"
}

require_prepare_backup()
{
	prepare_source=$1
	[ "$(git -C "$prepare_source" rev-parse \
		"refs/uml/recreate-backups/arm64/$PREPARE_STALE_TIP")" = \
		"$PREPARE_STALE_TIP" ] ||
		fail "recreate failure lost the pre-recreate backup tip"
}

write_prepare_metadata "$PREPARE_CANONICAL_TREE"
PREPARE_SUCCESS_SOURCE="$TEST_ROOT/prepare-success-source"
initialize_prepare_source "$PREPARE_SUCCESS_SOURCE"
UML_KERNEL_SLICE_FIXTURE_SOURCE=$PREPARE_SUCCESS_SOURCE \
	"$PREPARE_SCRIPT" --slice arm64 --recreate --no-fetch \
	>"$TEST_ROOT/prepare-success.out" 2>"$TEST_ROOT/prepare-success.err" || {
	sed -n '1,80p' "$TEST_ROOT/prepare-success.err" >&2
	fail "recreate rejected a stale subject and patch ID"
}
PREPARE_SUCCESS_TIP=$(git -C "$PREPARE_SUCCESS_SOURCE" \
	rev-parse refs/heads/managed-fixture)
[ "$PREPARE_SUCCESS_TIP" != "$PREPARE_STALE_TIP" ] &&
	[ "$(git -C "$PREPARE_SUCCESS_SOURCE" \
	rev-parse 'refs/heads/managed-fixture^{tree}')" = \
	"$PREPARE_CANONICAL_TREE" ] ||
	fail "recreate did not install the current canonical replay"
[ "$(git -C "$PREPARE_SUCCESS_SOURCE" rev-parse \
	"refs/uml/recreate-backups/arm64/$PREPARE_STALE_TIP")" = \
	"$PREPARE_STALE_TIP" ] ||
	fail "successful recreate did not preserve the stale tip"
[ "$(git -C "$PREPARE_SUCCESS_SOURCE" branch --show-current)" = \
	managed-fixture ] ||
	fail "successful recreate did not restore the managed branch checkout"
require_no_replacement_branch "$PREPARE_SUCCESS_SOURCE"

# A failed git am and a replay that misses the final tree both leave the
# target ref and checkout unchanged and remove the temporary candidate.
REAL_GIT=$(command -v git)
{
	printf '%s\n' '#!/bin/sh'
	printf '%s\n' 'set -eu'
	printf '%s\n' ': "${UML_KERNEL_SLICE_FIXTURE_REAL_GIT:?}"'
	printf '%s\n' 'case "${UML_KERNEL_SLICE_FIXTURE_GIT_MODE:-}" in'
	printf '%s\n' 'apply-fail)'
	printf '%s\n' 'case " $* " in'
	printf '%s\n' '*" am --keep-cr "*) exit 97 ;;'
	printf '%s\n' 'esac'
	printf '%s\n' ';;'
	printf '%s\n' 'cas)'
	printf '%s\n' 'previous='
	printf '%s\n' 'inject=0'
	printf '%s\n' 'for argument do'
	printf '%s\n' 'if [ "$previous" = update-ref ] &&'
	printf '%s\n' '   [ "$argument" = "refs/heads/$UML_KERNEL_SLICE_FIXTURE_TARGET" ]; then'
	printf '%s\n' 'inject=1'
	printf '%s\n' 'fi'
	printf '%s\n' 'previous=$argument'
	printf '%s\n' 'done'
	printf '%s\n' 'if [ "$inject" -eq 1 ] &&'
	printf '%s\n' '   [ ! -e "$UML_KERNEL_SLICE_FIXTURE_CAS_MARKER" ]; then'
	printf '%s\n' '"$UML_KERNEL_SLICE_FIXTURE_REAL_GIT" -C "$UML_KERNEL_SLICE_FIXTURE_SOURCE" update-ref "refs/heads/$UML_KERNEL_SLICE_FIXTURE_TARGET" "$UML_KERNEL_SLICE_FIXTURE_CONCURRENT_TIP" "$UML_KERNEL_SLICE_FIXTURE_OLD_TIP"'
	printf '%s\n' ': > "$UML_KERNEL_SLICE_FIXTURE_CAS_MARKER"'
	printf '%s\n' 'fi'
	printf '%s\n' ';;'
	printf '%s\n' 'esac'
	printf '%s\n' 'exec "$UML_KERNEL_SLICE_FIXTURE_REAL_GIT" "$@"'
} > "$PREPARE_GIT_BIN/git"
chmod +x "$PREPARE_GIT_BIN/git"

PREPARE_APPLY_FAIL_SOURCE="$TEST_ROOT/prepare-apply-fail-source"
initialize_prepare_source "$PREPARE_APPLY_FAIL_SOURCE"
if PATH="$PREPARE_GIT_BIN:$PATH" \
	UML_KERNEL_SLICE_FIXTURE_REAL_GIT=$REAL_GIT \
	UML_KERNEL_SLICE_FIXTURE_GIT_MODE=apply-fail \
	UML_KERNEL_SLICE_FIXTURE_SOURCE=$PREPARE_APPLY_FAIL_SOURCE \
	"$PREPARE_SCRIPT" --slice arm64 --recreate --no-fetch \
	>"$TEST_ROOT/prepare-apply-fail.out" \
	2>"$TEST_ROOT/prepare-apply-fail.err"; then
	fail "recreate accepted an injected current-patch application failure"
fi
grep -F 'series does not apply' "$TEST_ROOT/prepare-apply-fail.err" >/dev/null ||
	fail "injected patch application failure failed for the wrong reason"
[ "$(git -C "$PREPARE_APPLY_FAIL_SOURCE" \
	rev-parse refs/heads/managed-fixture)" = "$PREPARE_STALE_TIP" ] &&
	[ "$(git -C "$PREPARE_APPLY_FAIL_SOURCE" branch --show-current)" = \
	managed-fixture ] ||
	fail "patch application failure changed the managed target"
require_prepare_backup "$PREPARE_APPLY_FAIL_SOURCE"
require_no_replacement_branch "$PREPARE_APPLY_FAIL_SOURCE"

PREPARE_VERIFY_FAIL_SOURCE="$TEST_ROOT/prepare-verify-fail-source"
initialize_prepare_source "$PREPARE_VERIFY_FAIL_SOURCE"
write_prepare_metadata "$WRONG_TREE"
if UML_KERNEL_SLICE_FIXTURE_SOURCE=$PREPARE_VERIFY_FAIL_SOURCE \
	"$PREPARE_SCRIPT" --slice arm64 --recreate --no-fetch \
	>"$TEST_ROOT/prepare-verify-fail.out" \
	2>"$TEST_ROOT/prepare-verify-fail.err"; then
	fail "recreate accepted a candidate with the wrong final tree"
fi
grep -F "expected $WRONG_TREE" "$TEST_ROOT/prepare-verify-fail.err" >/dev/null ||
	fail "wrong final tree failed for the wrong reason"
[ "$(git -C "$PREPARE_VERIFY_FAIL_SOURCE" \
	rev-parse refs/heads/managed-fixture)" = "$PREPARE_STALE_TIP" ] &&
	[ "$(git -C "$PREPARE_VERIFY_FAIL_SOURCE" branch --show-current)" = \
	managed-fixture ] ||
	fail "candidate verification failure changed the managed target"
require_prepare_backup "$PREPARE_VERIFY_FAIL_SOURCE"
require_no_replacement_branch "$PREPARE_VERIFY_FAIL_SOURCE"
write_prepare_metadata "$PREPARE_CANONICAL_TREE"

# Advance the target ref from inside the Git wrapper immediately before the
# script's compare-and-swap. The stale-tip backup must survive, the concurrent
# tip must win, and cleanup must switch to that tip and remove the candidate.
PREPARE_CAS_SOURCE="$TEST_ROOT/prepare-cas-source"
PREPARE_CAS_MARKER="$TEST_ROOT/prepare-cas-injected"
initialize_prepare_source "$PREPARE_CAS_SOURCE"
git -C "$PREPARE_CAS_SOURCE" switch -q -c concurrent-fixture \
	managed-fixture
printf '%s\n' concurrent > "$PREPARE_CAS_SOURCE/concurrent.txt"
git -C "$PREPARE_CAS_SOURCE" add concurrent.txt
git -C "$PREPARE_CAS_SOURCE" commit -qm 'fixture concurrent target update'
PREPARE_CONCURRENT_TIP=$(git -C "$PREPARE_CAS_SOURCE" rev-parse HEAD)
git -C "$PREPARE_CAS_SOURCE" switch -q managed-fixture
if PATH="$PREPARE_GIT_BIN:$PATH" \
	UML_KERNEL_SLICE_FIXTURE_REAL_GIT=$REAL_GIT \
	UML_KERNEL_SLICE_FIXTURE_GIT_MODE=cas \
	UML_KERNEL_SLICE_FIXTURE_SOURCE=$PREPARE_CAS_SOURCE \
	UML_KERNEL_SLICE_FIXTURE_TARGET=managed-fixture \
	UML_KERNEL_SLICE_FIXTURE_CONCURRENT_TIP=$PREPARE_CONCURRENT_TIP \
	UML_KERNEL_SLICE_FIXTURE_OLD_TIP=$PREPARE_STALE_TIP \
	UML_KERNEL_SLICE_FIXTURE_CAS_MARKER=$PREPARE_CAS_MARKER \
	"$PREPARE_SCRIPT" --slice arm64 --recreate --no-fetch \
	>"$TEST_ROOT/prepare-cas.out" 2>"$TEST_ROOT/prepare-cas.err"; then
	fail "recreate overwrote a concurrently advanced target ref"
fi
[ -e "$PREPARE_CAS_MARKER" ] ||
	fail "CAS fixture did not inject its concurrent ref update"
[ "$(git -C "$PREPARE_CAS_SOURCE" \
	rev-parse refs/heads/managed-fixture)" = "$PREPARE_CONCURRENT_TIP" ] ||
	fail "CAS failure overwrote the concurrent target tip"
[ "$(git -C "$PREPARE_CAS_SOURCE" rev-parse \
	"refs/uml/recreate-backups/arm64/$PREPARE_STALE_TIP")" = \
	"$PREPARE_STALE_TIP" ] ||
	fail "CAS failure lost the pre-recreate backup tip"
[ "$(git -C "$PREPARE_CAS_SOURCE" branch --show-current)" = \
	managed-fixture ] ||
	fail "CAS failure did not restore the concurrently updated target checkout"
require_no_replacement_branch "$PREPARE_CAS_SOURCE"

# Every producer exposes the same selector and rejects unknown slice
# identities before consulting a managed checkout.
for relative in \
	build/build-kernel.sh \
	scripts/prepare-source.sh \
	scripts/rebase-patches.sh; do
	producer="$LINE_ROOT/$relative"
	require_file "$producer"
	"$producer" --help >"$TEST_ROOT/help.out"
	grep -F -- '--slice' "$TEST_ROOT/help.out" >/dev/null &&
		grep -F 'arm64' "$TEST_ROOT/help.out" >/dev/null &&
		grep -F 'x86_64' "$TEST_ROOT/help.out" >/dev/null ||
		fail "$relative does not document the shared slice selector"
	if "$producer" --slice invalid \
		>"$TEST_ROOT/invalid.out" 2>"$TEST_ROOT/invalid.err"; then
		fail "$relative accepted an unknown kernel slice"
	fi
	grep -F 'kernel slice must be arm64 or x86_64' \
		"$TEST_ROOT/invalid.err" >/dev/null ||
		fail "$relative rejected an unknown slice for the wrong reason"
done

# Exercise the producer's guards with an isolated selector fixture so this
# policy test never needs a configured Linux checkout or compiler toolchain.
BUILD_FIXTURE="$TEST_ROOT/build-fixture"
mkdir -p \
	"$BUILD_FIXTURE/kernel/common/tooling" \
	"$BUILD_FIXTURE/kernel/mainline-6.18/build/toolchain" \
	"$BUILD_FIXTURE/kernel/mainline-6.18/scripts" \
	"$BUILD_FIXTURE/kernel/mainline-6.18/patches" \
	"$BUILD_FIXTURE/kernel/mainline-6.18/slices/x86_64/patches"
cp "$LINE_ROOT/build/build-kernel.sh" \
	"$BUILD_FIXTURE/kernel/mainline-6.18/build/build-kernel.sh"
cp "$LINE_ROOT/build/toolchain/darwin.env" \
	"$BUILD_FIXTURE/kernel/mainline-6.18/build/toolchain/darwin.env"
cp "$LINE_ROOT/scripts/select-slice.sh" \
	"$BUILD_FIXTURE/kernel/mainline-6.18/scripts/select-slice.sh"
cp "$ROOT_DIR/kernel/common/tooling/config-fragments.sh" \
	"$BUILD_FIXTURE/kernel/common/tooling/config-fragments.sh"
cp "$COMMON_METADATA" \
	"$BUILD_FIXTURE/kernel/mainline-6.18/patches/metadata.env"
cp "$SLICE_METADATA" \
	"$BUILD_FIXTURE/kernel/mainline-6.18/slices/x86_64/patches/metadata.env"
printf '%s\n' \
	'#!/bin/sh' \
	"printf '%s\\n' '$TEST_ROOT/fake-linux'" \
	> "$BUILD_FIXTURE/kernel/mainline-6.18/scripts/source-location.sh"
chmod +x "$BUILD_FIXTURE/kernel/mainline-6.18/scripts/source-location.sh"
# Every kernel has 4 KiB pages, so the producer has no page-size option; a
# build that asks for another size is refused, not ignored.
if UML_KERNEL_JOBS=1 \
	"$BUILD_FIXTURE/kernel/mainline-6.18/build/build-kernel.sh" \
		--slice x86_64 --build "$TEST_ROOT/build-x86-16k" \
		--page-size 16k >"$TEST_ROOT/x86-16k.out" \
		2>"$TEST_ROOT/x86-16k.err"; then
	fail "build-kernel.sh accepted a page size"
fi
grep -F 'unknown option: --page-size' "$TEST_ROOT/x86-16k.err" >/dev/null ||
	fail "build-kernel.sh did not reject --page-size as an unknown option"

# The Android product exists once, as the arm64 kernel without a lock-checking
# probe, and the producer rejects every other request.
expect_android_rejection()
{
	android_reason=$1
	shift
	if UML_KERNEL_JOBS=1 \
		"$BUILD_FIXTURE/kernel/mainline-6.18/build/build-kernel.sh" "$@" \
			>"$TEST_ROOT/android-rejected.out" \
			2>"$TEST_ROOT/android-rejected.err"; then
		fail "build-kernel.sh accepted: $*"
	fi
	grep -F -- "$android_reason" "$TEST_ROOT/android-rejected.err" >/dev/null ||
		fail "build-kernel.sh rejected '$*' for the wrong reason"
}
expect_android_rejection 'the android product supports only the arm64 slice' \
	--product android --slice x86_64 --build "$TEST_ROOT/build-android-x86"
expect_android_rejection '--lockdep is not available for the android product' \
	--product android --build "$TEST_ROOT/build-android-lockdep" --lockdep
expect_android_rejection '--product must be linux or android' \
	--product desktop --build "$TEST_ROOT/build-desktop"

# Run verify-replay against no-op producer doubles. This checks the complete
# ordered matrix without downloading Linux or spending a full kernel build.
REPLAY_FIXTURE="$TEST_ROOT/replay-fixture"
REPLAY_LOG="$TEST_ROOT/replay.log"
mkdir -p \
	"$REPLAY_FIXTURE/kernel/mainline-6.18/scripts" \
	"$REPLAY_FIXTURE/kernel/mainline-6.18/build" \
	"$TEST_ROOT/replay-work"
cp "$LINE_ROOT/scripts/verify-replay.sh" \
	"$REPLAY_FIXTURE/kernel/mainline-6.18/scripts/verify-replay.sh"
printf '%s\n' \
	'#!/bin/sh' \
	'set -eu' \
	'label=${0##*/}' \
	'printf "%s" "$label" >> "$UML_KERNEL_SLICE_TEST_LOG"' \
	'for argument do' \
	'printf " %s" "$argument" >> "$UML_KERNEL_SLICE_TEST_LOG"' \
	'done' \
	'printf "\\n" >> "$UML_KERNEL_SLICE_TEST_LOG"' \
	> "$TEST_ROOT/producer-double.sh"
chmod +x "$TEST_ROOT/producer-double.sh"
ln -s "$TEST_ROOT/producer-double.sh" \
	"$REPLAY_FIXTURE/kernel/mainline-6.18/scripts/prepare-source.sh"
ln -s "$TEST_ROOT/producer-double.sh" \
	"$REPLAY_FIXTURE/kernel/mainline-6.18/build/build-kernel.sh"
UML_KERNEL_SLICE_TEST_LOG=$REPLAY_LOG
export UML_KERNEL_SLICE_TEST_LOG
"$REPLAY_FIXTURE/kernel/mainline-6.18/scripts/verify-replay.sh" \
	--work-root "$TEST_ROOT/replay-work" --jobs 7 \
	>"$TEST_ROOT/replay.out" 2>"$TEST_ROOT/replay.err" || {
	sed -n '1,80p' "$TEST_ROOT/replay.err" >&2
	fail "verify-replay matrix fixture failed"
}
awk '
NR == 1 { good += ($0 == "prepare-source.sh --slice arm64") }
NR == 2 { good += ($0 == "prepare-source.sh --slice arm64 --verify --no-fetch") }
NR == 3 { good += ($1 == "build-kernel.sh" && $2 == "--slice" &&
	$3 == "arm64" && $4 == "--build" && $5 ~ /\/build-arm64-4k$/ &&
	$6 == "--jobs" && $7 == "7" && NF == 7) }
NR == 4 { good += ($1 == "build-kernel.sh" && $2 == "--slice" &&
	$3 == "arm64" && $4 == "--build" && $5 ~ /\/build-arm64-4k-android$/ &&
	$6 == "--product" && $7 == "android" && $8 == "--jobs" && $9 == "7") }
NR == 5 { good += ($0 == "prepare-source.sh --slice x86_64 --no-fetch") }
NR == 6 { good += ($0 == "prepare-source.sh --slice x86_64 --verify --no-fetch") }
NR == 7 { good += ($1 == "build-kernel.sh" && $2 == "--slice" &&
	$3 == "x86_64" && $4 == "--build" && $5 ~ /\/build-x86_64-4k$/ &&
	$6 == "--jobs" && $7 == "7" && NF == 7) }
END { exit !(NR == 7 && good == 7) }
' "$REPLAY_LOG" </dev/null || {
	sed -n '1,40p' "$REPLAY_LOG" >&2
	fail "verify-replay does not cover arm64, the Android product, and x86_64"
}

# Every kernel line, slice and product builds as the neutral user and host, so
# the Linux banner names neither the builder's account nor machine.
grep -F 'KBUILD_BUILD_USER=kernarium KBUILD_BUILD_HOST=kernarium' \
	"$LINE_ROOT/build/build-kernel.sh" >/dev/null ||
	fail "build-kernel.sh does not build as the neutral user and host"

printf '%s\n' "kernel common/x86_64 slice producer contract passed"
