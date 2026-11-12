#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Materialize the exact commit pinned on the maintained upstream Linux stable
# branch. The cache retains the network remote; the published replay checkout
# does not.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/mainline-6.18"
METADATA_PATH=${UML_KERNEL_METADATA_PATH:-"$LINE_ROOT/patches/metadata.env"}
SOURCE_LOCATION="$LINE_ROOT/scripts/source-location.sh"
PUBLISHER="$ROOT_DIR/dependencies/publish-prefix.py"
CACHE_ROOT=${UML_KERNEL_CACHE_DIR:-"$ROOT_DIR/.build/kernel-cache/mainline-6.18"}
FETCH=1
WORK_ROOT=
TEMPORARY_CACHE=

fail()
{
	printf '%s\n' "kernel/mainline-6.18/scripts/materialize-git.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/mainline-6.18/scripts/materialize-git.sh [options]

Materialize the exact upstream Linux stable commit as a local replay checkout.

Options:
  --cache PATH    Pinned Git cache
                  (default: .build/kernel-cache/mainline-6.18)
  --no-fetch      Use only the exact commit already present in the Git cache
  -h, --help      Show this help

The destination must not exist. A failed fetch or identity check never leaves
a partial destination checkout.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--cache)
		[ "$#" -ge 2 ] || fail "--cache requires a path"
		CACHE_ROOT=$2
		shift 2
		;;
	--no-fetch)
		FETCH=0
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -x "$SOURCE_LOCATION" ] || fail "missing source-location verifier: $SOURCE_LOCATION"
SOURCE_DIR=$("$SOURCE_LOCATION") || fail "kernel source configuration is invalid"
[ -r "$METADATA_PATH" ] || fail "missing metadata: $METADATA_PATH"
# shellcheck disable=SC1090
. "$METADATA_PATH"
[ "${UML_KERNEL_SERIES_SCHEMA:-}" = 4 ] ||
	fail "unsupported kernel source/series schema"
[ -n "${UML_KERNEL_RELEASE_VERSION:-}" ] || fail "release version is missing"
[ -n "${UML_KERNEL_UPSTREAM_REPOSITORY:-}" ] ||
	fail "upstream repository is missing"
[ -n "${UML_KERNEL_UPSTREAM_BRANCH:-}" ] || fail "upstream branch is missing"
[ -n "${UML_KERNEL_UPSTREAM_COMMIT:-}" ] || fail "upstream commit is missing"
[ -n "${UML_KERNEL_UPSTREAM_TREE:-}" ] || fail "upstream tree is missing"
[ -n "${UML_KERNEL_BASE_REF:-}" ] || fail "local base ref is missing"

command -v git >/dev/null 2>&1 || fail "git build tool is required"
command -v python3 >/dev/null 2>&1 || fail "python3 build tool is required"

case "$UML_KERNEL_UPSTREAM_REPOSITORY" in
https://*.git) ;;
*) fail "upstream repository must be an HTTPS Git URL ending in .git" ;;
esac
validate_git_oid()
{
	label=$1
	oid=$2
	case "$oid" in *[!0-9a-f]*|'') fail "$label is not lowercase hexadecimal" ;; esac
	[ "${#oid}" -eq 40 ] || fail "$label is not a full Git object ID"
}
validate_git_oid "upstream commit" "$UML_KERNEL_UPSTREAM_COMMIT"
validate_git_oid "upstream tree" "$UML_KERNEL_UPSTREAM_TREE"
git check-ref-format "refs/heads/$UML_KERNEL_UPSTREAM_BRANCH" >/dev/null 2>&1 ||
	fail "invalid upstream stable branch: $UML_KERNEL_UPSTREAM_BRANCH"
case "$UML_KERNEL_BASE_REF" in refs/heads/*) ;; *) fail "invalid local base ref" ;; esac
git check-ref-format "$UML_KERNEL_BASE_REF" >/dev/null 2>&1 ||
	fail "invalid local base ref: $UML_KERNEL_BASE_REF"
PARENT_DIR=$(dirname -- "$SOURCE_DIR")
SOURCE_NAME=$(basename -- "$SOURCE_DIR")
mkdir -p -- "$PARENT_DIR" "$CACHE_ROOT"
PARENT_DIR=$(CDPATH= cd -- "$PARENT_DIR" && pwd -P)
CACHE_ROOT=$(CDPATH= cd -- "$CACHE_ROOT" && pwd -P)
SOURCE_DIR="$PARENT_DIR/$SOURCE_NAME"
[ ! -e "$SOURCE_DIR" ] && [ ! -L "$SOURCE_DIR" ] ||
	fail "destination already exists: $SOURCE_DIR"
CACHE_REPOSITORY="$CACHE_ROOT/git"

cleanup()
{
	if [ -n "${TEMPORARY_CACHE:-}" ] && [ -d "$TEMPORARY_CACHE" ]; then
		rm -rf -- "$TEMPORARY_CACHE"
	fi
	if [ -n "${WORK_ROOT:-}" ] && [ -d "$WORK_ROOT" ]; then
		rm -rf -- "$WORK_ROOT"
	fi
}
trap cleanup EXIT HUP INT TERM

if [ ! -e "$CACHE_REPOSITORY" ] && [ ! -L "$CACHE_REPOSITORY" ]; then
	[ "$FETCH" -eq 1 ] || fail "upstream Git cache is absent: $CACHE_REPOSITORY"
	TEMPORARY_CACHE=$(mktemp -d "$CACHE_ROOT/.git-cache.XXXXXX")
	git -C "$TEMPORARY_CACHE" init --quiet
	git -C "$TEMPORARY_CACHE" remote add origin "$UML_KERNEL_UPSTREAM_REPOSITORY"
	git -C "$TEMPORARY_CACHE" fetch --no-tags --depth 1 origin \
		"$UML_KERNEL_UPSTREAM_COMMIT" >/dev/null
	git -C "$TEMPORARY_CACHE" update-ref refs/heads/uml-pin \
		"$UML_KERNEL_UPSTREAM_COMMIT"
	# An exclusive rename never moves the cache below one that a concurrent
	# run published first; that winner is checked below like any cache.
	status=0
	python3 "$PUBLISHER" "$TEMPORARY_CACHE" "$CACHE_REPOSITORY" || status=$?
	case $status in
	0) TEMPORARY_CACHE= ;;
	1) ;;
	*) fail "upstream Git cache could not be published: $CACHE_REPOSITORY" ;;
	esac
fi
[ -d "$CACHE_REPOSITORY/.git" ] || fail "upstream Git cache is malformed"
[ "$(git -C "$CACHE_REPOSITORY" remote get-url origin 2>/dev/null || true)" = \
	"$UML_KERNEL_UPSTREAM_REPOSITORY" ] || fail "upstream Git cache origin changed"
if ! git -C "$CACHE_REPOSITORY" cat-file -e \
	"$UML_KERNEL_UPSTREAM_COMMIT^{commit}" 2>/dev/null; then
	[ "$FETCH" -eq 1 ] || fail "upstream commit is absent from the offline cache"
	git -C "$CACHE_REPOSITORY" fetch --no-tags --depth 1 origin \
		"$UML_KERNEL_UPSTREAM_COMMIT" >/dev/null
	git -C "$CACHE_REPOSITORY" update-ref refs/heads/uml-pin \
		"$UML_KERNEL_UPSTREAM_COMMIT"
fi
[ "$(git -C "$CACHE_REPOSITORY" rev-parse "$UML_KERNEL_UPSTREAM_COMMIT^{commit}")" = \
	"$UML_KERNEL_UPSTREAM_COMMIT" ] || fail "cache resolved the wrong upstream commit"
[ "$(git -C "$CACHE_REPOSITORY" rev-parse "$UML_KERNEL_UPSTREAM_COMMIT^{tree}")" = \
	"$UML_KERNEL_UPSTREAM_TREE" ] || fail "upstream commit tree differs from metadata"
[ "$(git -C "$CACHE_REPOSITORY" rev-parse refs/heads/uml-pin 2>/dev/null || true)" = \
	"$UML_KERNEL_UPSTREAM_COMMIT" ] || fail "Git cache does not retain its pinned ref"

WORK_ROOT=$(mktemp -d "$PARENT_DIR/.uml-kernel-source.XXXXXX")
STAGED_SOURCE="$WORK_ROOT/linux"
git clone --quiet --no-checkout "file://$CACHE_REPOSITORY" "$STAGED_SOURCE"
BASE_BRANCH=${UML_KERNEL_BASE_REF#refs/heads/}
git -C "$STAGED_SOURCE" switch --quiet -c "$BASE_BRANCH" \
	"$UML_KERNEL_UPSTREAM_COMMIT"
git -C "$STAGED_SOURCE" remote remove origin
git -C "$STAGED_SOURCE" config uml.source.releaseVersion \
	"$UML_KERNEL_RELEASE_VERSION"
git -C "$STAGED_SOURCE" config uml.source.upstreamRepository \
	"$UML_KERNEL_UPSTREAM_REPOSITORY"
git -C "$STAGED_SOURCE" config uml.source.upstreamBranch \
	"$UML_KERNEL_UPSTREAM_BRANCH"
git -C "$STAGED_SOURCE" config uml.source.upstreamCommit \
	"$UML_KERNEL_UPSTREAM_COMMIT"
git -C "$STAGED_SOURCE" config uml.source.upstreamTree \
	"$UML_KERNEL_UPSTREAM_TREE"
git -C "$STAGED_SOURCE" config uml.source.baseRef "$UML_KERNEL_BASE_REF"
[ -z "$(git -C "$STAGED_SOURCE" remote)" ] || fail "staged checkout retains a remote"
[ -z "$(git -C "$STAGED_SOURCE" status --porcelain --untracked-files=all)" ] ||
	fail "staged checkout is dirty"
[ "$(git -C "$STAGED_SOURCE" rev-parse HEAD)" = "$UML_KERNEL_UPSTREAM_COMMIT" ] ||
	fail "staged checkout is not at the pinned upstream commit"
status=0
python3 "$PUBLISHER" "$STAGED_SOURCE" "$SOURCE_DIR" || status=$?
case $status in
0) ;;
1) fail "destination was published concurrently: $SOURCE_DIR" ;;
*) fail "source checkout could not be published: $SOURCE_DIR" ;;
esac
printf '%s\n' "materialized Linux $UML_KERNEL_RELEASE_VERSION stable commit in $SOURCE_DIR"
