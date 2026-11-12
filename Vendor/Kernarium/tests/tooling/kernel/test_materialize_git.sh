#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/mainline-6.18"
MATERIALIZER="$LINE_ROOT/scripts/materialize-git.sh"
SOURCE_LOCATION="$LINE_ROOT/scripts/source-location.sh"
TEST_PARENT=${TMPDIR:-/tmp}
if [ -f "$LINE_ROOT/source-location.conf" ]; then
	configured_source=$("$SOURCE_LOCATION" --require-source 2>/dev/null || true)
	if [ -n "$configured_source" ]; then
		TEST_PARENT=$(dirname -- "$configured_source")
	fi
fi
TEST_ROOT=$(mktemp -d "$TEST_PARENT/.uml-kernel-git.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

TEST_NAME=test-materialize-git.sh
. "$(dirname -- "$0")/../support/expect.sh"

# Linux and configured Apple acceptance hosts exercise the complete fixture on
# a case-sensitive filesystem. An otherwise unconfigured case-insensitive
# macOS host can only exercise the mandatory refusal without manufacturing a
# transient source volume for a host-only tooling test.
CASE_CONFIG="$TEST_ROOT/case-source.conf"
printf 'UML_KERNEL_SOURCE_DIR=%s\n' "$TEST_ROOT/case-source" > "$CASE_CONFIG"
if ! "$SOURCE_LOCATION" --config "$CASE_CONFIG" \
	> "$TEST_ROOT/case-source.out" 2> "$TEST_ROOT/case-source.err"; then
	grep -F 'is case-insensitive' "$TEST_ROOT/case-source.err" >/dev/null ||
		fail "source-location validation failed for an unexpected reason"
	expect_failure "missing source-location configuration" \
		"$SOURCE_LOCATION" --config "$TEST_ROOT/missing-source.conf"
	printf '%s\n' "kernel source-location case-sensitivity rejection passed"
	exit 0
fi

UPSTREAM="$TEST_ROOT/upstream"
CACHE="$TEST_ROOT/cache"
CACHE_REPOSITORY="$CACHE/git"
METADATA="$TEST_ROOT/metadata.env"
BASE_REF=refs/heads/uml/upstream-linux-1.2.3
REPOSITORY=https://example.invalid/stable-linux.git
mkdir -p "$UPSTREAM" "$CACHE"
git -C "$UPSTREAM" init --quiet
printf '%s\n' parent >"$UPSTREAM/history.txt"
git -C "$UPSTREAM" add history.txt
git -C "$UPSTREAM" commit --quiet -m 'fixture: stable parent'
PARENT=$(git -C "$UPSTREAM" rev-parse HEAD)
mkdir -p "$UPSTREAM/docs"
printf '%s\n' 'VERSION = 1' >"$UPSTREAM/Makefile"
printf '%s\n' payload >"$UPSTREAM/docs/payload.txt"
printf '#!/bin/sh\nexit 0\n' >"$UPSTREAM/tool.sh"
chmod +x "$UPSTREAM/tool.sh"
ln -s docs/payload.txt "$UPSTREAM/payload-link"
git -C "$UPSTREAM" add Makefile docs payload-link tool.sh
git -C "$UPSTREAM" commit --quiet -m 'Linux 1.2.3 stable'
COMMIT=$(git -C "$UPSTREAM" rev-parse HEAD)
TREE=$(git -C "$UPSTREAM" rev-parse HEAD^{tree})
AUTHOR_TIME=$(git -C "$UPSTREAM" show -s --format=%at HEAD)
COMMITTER_TIME=$(git -C "$UPSTREAM" show -s --format=%ct HEAD)

git clone --quiet "$UPSTREAM" "$CACHE_REPOSITORY"
git -C "$CACHE_REPOSITORY" remote set-url origin "$REPOSITORY"
git -C "$CACHE_REPOSITORY" update-ref refs/heads/uml-pin "$COMMIT"
cat >"$METADATA" <<EOF
UML_KERNEL_SERIES_SCHEMA=4
UML_KERNEL_RELEASE_VERSION='1.2.3'
UML_KERNEL_UPSTREAM_REPOSITORY='$REPOSITORY'
UML_KERNEL_UPSTREAM_BRANCH='linux-1.2.y'
UML_KERNEL_UPSTREAM_COMMIT='$COMMIT'
UML_KERNEL_UPSTREAM_TREE='$TREE'
UML_KERNEL_BASE_REF='$BASE_REF'
EOF

materialize_with_metadata()
{
	metadata=$1
	destination=$2
	cache=${3:-$CACHE}
	config="$TEST_ROOT/source-location-$FAILURE_NUMBER.conf"
	printf 'UML_KERNEL_SOURCE_DIR=%s\n' "$destination" > "$config"
	UML_KERNEL_METADATA_PATH="$metadata" \
	UML_KERNEL_SOURCE_CONFIG="$config" \
		"$MATERIALIZER" --cache "$cache" --no-fetch
}

materialize()
{
	materialize_with_metadata "$METADATA" "$1"
}

SOURCE="$TEST_ROOT/source"
materialize "$SOURCE" >/dev/null
[ "$(git -C "$SOURCE" branch --show-current)" = "${BASE_REF#refs/heads/}" ] ||
	fail "materialized checkout is not on its pinned base branch"
[ "$(git -C "$SOURCE" rev-parse HEAD)" = "$COMMIT" ] ||
	fail "materialized checkout changed the upstream commit identity"
[ "$(git -C "$SOURCE" rev-parse HEAD^{tree})" = "$TREE" ] ||
	fail "materialized checkout changed the upstream tree identity"
[ "$(git -C "$SOURCE" rev-parse HEAD^)" = "$PARENT" ] ||
	fail "materialized checkout discarded upstream history identity"
[ "$(git -C "$SOURCE" show -s --format=%at HEAD)" = "$AUTHOR_TIME" ] ||
	fail "materializer rewrote the upstream author timestamp"
[ "$(git -C "$SOURCE" show -s --format=%ct HEAD)" = "$COMMITTER_TIME" ] ||
	fail "materializer rewrote the upstream committer timestamp"
[ "$(git -C "$SOURCE" show -s --format=%s HEAD)" = 'Linux 1.2.3 stable' ] ||
	fail "materializer rewrote the upstream commit subject"
[ -z "$(git -C "$SOURCE" remote)" ] ||
	fail "published source checkout retained a network remote"
[ -z "$(git -C "$SOURCE" status --porcelain --untracked-files=all)" ] ||
	fail "materialized checkout is dirty"
[ "$(git -C "$SOURCE" config --get uml.source.upstreamRepository)" = \
	"$REPOSITORY" ] || fail "repository provenance was not recorded"
[ "$(git -C "$SOURCE" config --get uml.source.upstreamBranch)" = \
	linux-1.2.y ] || fail "stable branch provenance was not recorded"
[ "$(git -C "$SOURCE" config --get uml.source.upstreamCommit)" = \
	"$COMMIT" ] || fail "commit provenance was not recorded"

expect_failure "replacement of an existing source checkout" materialize "$SOURCE"

expect_failure "missing source-location configuration" \
	env UML_KERNEL_METADATA_PATH="$METADATA" \
		UML_KERNEL_SOURCE_CONFIG="$TEST_ROOT/missing-source.conf" \
		"$MATERIALIZER" --cache "$CACHE" --no-fetch

git -C "$CACHE_REPOSITORY" remote set-url origin \
	https://example.invalid/wrong.git
expect_failure "changed cache origin" materialize "$TEST_ROOT/origin-tamper"
[ ! -e "$TEST_ROOT/origin-tamper" ] ||
	fail "origin failure left a source checkout"
git -C "$CACHE_REPOSITORY" remote set-url origin "$REPOSITORY"

git -C "$CACHE_REPOSITORY" update-ref refs/heads/uml-pin "$PARENT"
expect_failure "changed pinned cache ref" materialize "$TEST_ROOT/ref-tamper"
[ ! -e "$TEST_ROOT/ref-tamper" ] || fail "ref failure left a source checkout"
git -C "$CACHE_REPOSITORY" update-ref refs/heads/uml-pin "$COMMIT"

BAD_TREE_METADATA="$TEST_ROOT/bad-tree.env"
sed "s/UML_KERNEL_UPSTREAM_TREE='[^']*'/UML_KERNEL_UPSTREAM_TREE='0000000000000000000000000000000000000000'/" \
	"$METADATA" >"$BAD_TREE_METADATA"
expect_failure "unexpected upstream tree" materialize_with_metadata \
	"$BAD_TREE_METADATA" "$TEST_ROOT/bad-tree"
[ ! -e "$TEST_ROOT/bad-tree" ] || fail "tree failure left a source checkout"

EMPTY_CACHE="$TEST_ROOT/empty-cache"
mkdir "$EMPTY_CACHE"
expect_failure "missing offline Git cache" materialize_with_metadata \
	"$METADATA" "$TEST_ROOT/missing-cache" "$EMPTY_CACHE"
[ ! -e "$TEST_ROOT/missing-cache" ] ||
	fail "missing-cache failure left a source checkout"

# A checkout another run publishes while this one stages is left untouched:
# the staged checkout is never moved below it.
RACE_BIN="$TEST_ROOT/race-bin"
mkdir "$RACE_BIN"
{
	printf '%s\n' '#!/bin/sh'
	printf '%s\n' 'if [ "$1" = clone ]; then'
	printf '%s\n' '	mkdir "$UML_TEST_RACE_SOURCE"'
	printf '%s\n' '	printf "%s\n" winner >"$UML_TEST_RACE_SOURCE/identity"'
	printf '%s\n' 'fi'
	printf 'exec %s "$@"\n' "$(command -v git)"
} >"$RACE_BIN/git"
chmod +x "$RACE_BIN/git"
materialize_during_race()
{
	(
		PATH="$RACE_BIN:$PATH"
		UML_TEST_RACE_SOURCE=$1
		export PATH UML_TEST_RACE_SOURCE
		materialize "$1"
	)
}
RACE_SOURCE="$TEST_ROOT/race-source"
expect_failure "publication over a concurrently published checkout" \
	materialize_during_race "$RACE_SOURCE"
grep -F 'published concurrently' "$TEST_ROOT/rejected-$FAILURE_NUMBER.err" \
	>/dev/null || fail "concurrent publication was not diagnosed"
[ "$(find "$RACE_SOURCE" -mindepth 1 -print)" = "$RACE_SOURCE/identity" ] ||
	fail "a staged checkout was moved into a concurrently published one"

[ -z "$(find "$TEST_ROOT" -maxdepth 1 -name '.uml-kernel-source.*' -print -quit)" ] ||
	fail "failed materialization left a staging directory"
printf '%s\n' "kernel Git materialization tests passed"
