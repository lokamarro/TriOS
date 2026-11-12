#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Prepare or verify the managed Linux replay branch from the exact commit
# pinned on the upstream stable repository and branch.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/mainline-6.18"
COMMON_ROOT="$ROOT_DIR/kernel/common"
MATERIALIZER="$LINE_ROOT/scripts/materialize-git.sh"
KERNEL_SERIES="$COMMON_ROOT/tooling/kernel_series.py"
SOURCE_LOCATION="$LINE_ROOT/scripts/source-location.sh"
SLICE_SELECTOR="$LINE_ROOT/scripts/select-slice.sh"

fail()
{
	printf '%s\n' "kernel/mainline-6.18/scripts/prepare-source.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/mainline-6.18/scripts/prepare-source.sh [options]

Prepare the pinned Darwin UML Linux source, or verify an existing prepared
checkout.  The source location must reside on a case-sensitive filesystem.

Options:
  --slice SLICE  Kernel slice to prepare: arm64 (default) or x86_64
  --cache PATH    Pinned source Git cache (default:
                  .build/kernel-cache/mainline-6.18 or UML_KERNEL_CACHE_DIR)
  --verify        Verify only; never download, switch branches, or apply patches
  --recreate      Replace a clean, stale managed replay branch after preserving
                  its old tip under refs/uml/recreate-backups
  --no-fetch      Prepare only from the exact commit already in the Git cache
  -h, --help      Show this help
EOF
}

CACHE_ROOT=${UML_KERNEL_CACHE_DIR:-"$ROOT_DIR/.build/kernel-cache/mainline-6.18"}
VERIFY_ONLY=0
RECREATE=0
FETCH=1
SLICE=arm64

while [ "$#" -gt 0 ]; do
	case "$1" in
	--slice)
		[ "$#" -ge 2 ] || fail "--slice requires arm64 or x86_64"
		SLICE=$2
		shift 2
		;;
	--cache)
		[ "$#" -ge 2 ] || fail "--cache requires a path"
		CACHE_ROOT=$2
		shift 2
		;;
	--verify)
		VERIFY_ONLY=1
		shift
		;;
	--recreate)
		RECREATE=1
		shift
		;;
	--no-fetch)
		FETCH=0
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		fail "unknown option: $1"
		;;
	esac
done

[ -r "$SLICE_SELECTOR" ] || fail "missing slice selector: $SLICE_SELECTOR"
# shellcheck disable=SC1090
. "$SLICE_SELECTOR"
uml_kernel_select_slice "$LINE_ROOT" "$SLICE" ||
	fail "cannot select kernel slice: $SLICE"
METADATA_PATH=$UML_KERNEL_METADATA_PATH
[ -r "$METADATA_PATH" ] || fail "missing metadata: $METADATA_PATH"

[ "$VERIFY_ONLY" -eq 0 ] || [ "$RECREATE" -eq 0 ] ||
	fail "--verify and --recreate cannot be used together"

[ -x "$SOURCE_LOCATION" ] || fail "missing source-location verifier: $SOURCE_LOCATION"
if [ "$VERIFY_ONLY" -eq 1 ]; then
	SOURCE_DIR=$("$SOURCE_LOCATION" --require-source) ||
		fail "kernel source configuration is invalid"
else
	SOURCE_DIR=$("$SOURCE_LOCATION") ||
		fail "kernel source configuration is invalid"
fi

command -v git >/dev/null 2>&1 || fail "git is required"
[ -x "$MATERIALIZER" ] || fail "missing Git materializer: $MATERIALIZER"
[ -r "$KERNEL_SERIES" ] || fail "missing kernel series helper: $KERNEL_SERIES"
"$COMMON_ROOT/abi/sync.sh" --verify ||
	fail "host ABI copies are stale; run kernel/common/abi/sync.sh"

kernel_series()
{
	python3 -B "$KERNEL_SERIES" "$@" --metadata "$METADATA_PATH"
}
kernel_series check || fail "the $UML_KERNEL_SLICE series files are invalid"

if [ ! -d "$SOURCE_DIR/.git" ]; then
	[ "$VERIFY_ONLY" -eq 0 ] || fail "Linux checkout is missing: $SOURCE_DIR"
	set -- --cache "$CACHE_ROOT"
	[ "$FETCH" -eq 1 ] || set -- "$@" --no-fetch
	"$MATERIALIZER" "$@"
fi

require_source_config()
{
	key=$1
	expected=$2
	actual=$(git -C "$SOURCE_DIR" config --local --get "$key" 2>/dev/null || true)
	[ "$actual" = "$expected" ] ||
		fail "source provenance $key is '$actual', expected '$expected'"
}

verify_upstream_identity()
{
	git -C "$SOURCE_DIR" show-ref --verify --quiet "$UML_KERNEL_BASE_REF" ||
		fail "pinned upstream base ref is missing: $UML_KERNEL_BASE_REF"
	BASE_COMMIT=$(git -C "$SOURCE_DIR" rev-parse "$UML_KERNEL_BASE_REF^{commit}")
	[ "$BASE_COMMIT" = "$UML_KERNEL_UPSTREAM_COMMIT" ] ||
		fail "upstream base is $BASE_COMMIT, expected $UML_KERNEL_UPSTREAM_COMMIT"
	base_tree=$(git -C "$SOURCE_DIR" rev-parse "$UML_KERNEL_BASE_REF^{tree}")
	[ "$base_tree" = "$UML_KERNEL_UPSTREAM_TREE" ] ||
		fail "upstream base tree is $base_tree; expected $UML_KERNEL_UPSTREAM_TREE"
	require_source_config uml.source.releaseVersion "$UML_KERNEL_RELEASE_VERSION"
	require_source_config uml.source.upstreamRepository "$UML_KERNEL_UPSTREAM_REPOSITORY"
	require_source_config uml.source.upstreamBranch "$UML_KERNEL_UPSTREAM_BRANCH"
	require_source_config uml.source.upstreamCommit "$UML_KERNEL_UPSTREAM_COMMIT"
	require_source_config uml.source.upstreamTree "$UML_KERNEL_UPSTREAM_TREE"
	require_source_config uml.source.baseRef "$UML_KERNEL_BASE_REF"
}

BASE_COMMIT=''
verify_upstream_identity

require_clean_checkout()
{
	if [ -n "$(git -C "$SOURCE_DIR" status --porcelain --untracked-files=all)" ]; then
		fail "managed Linux checkout is dirty: $SOURCE_DIR"
	fi
}

verify_managed_branch()
{
	managed_branch=${1:-$UML_KERNEL_PATCH_BRANCH}
	actual_head=$(git -C "$SOURCE_DIR" rev-parse HEAD)
	# The branch must be the checked-in series replayed on the pinned base:
	# its tree is the line's final tree and exporting it reproduces every
	# owned patch byte for byte. Preparation also marks a series boundary that
	# an older replay has no branch for; verification alone writes nothing.
	mark=--mark
	[ "$VERIFY_ONLY" -eq 0 ] || mark=
	kernel_series verify-branch --source "$SOURCE_DIR" --tip "$managed_branch" $mark ||
		fail "managed branch $managed_branch is not the checked-in series (use --recreate only after reviewing it)"
	"$COMMON_ROOT/abi/sync.sh" --kernel-source "$SOURCE_DIR" --verify ||
		fail "managed Linux ABI copies are stale"
	printf '%s\n' "verified $UML_KERNEL_SLICE slice ref $managed_branch HEAD $actual_head against canonical tree $UML_KERNEL_FINAL_TREE" >&2
}

UPSTREAM_BASE_COMMIT=$BASE_COMMIT
verify_common_parent()
{
	git -C "$SOURCE_DIR" show-ref --verify --quiet \
		"refs/heads/$UML_KERNEL_PARENT_BRANCH" ||
		fail "arm64 kernel branch is missing: $UML_KERNEL_PARENT_BRANCH"
	python3 -B "$KERNEL_SERIES" verify-branch \
		--metadata "$UML_KERNEL_COMMON_METADATA_PATH" \
		--source "$SOURCE_DIR" --tip "$UML_KERNEL_PARENT_BRANCH" ||
		fail "arm64 kernel branch is not the checked-in arm64 series"
}

if [ "$UML_KERNEL_SLICE" = x86_64 ]; then
	if [ "$VERIFY_ONLY" -eq 0 ]; then
		# The x86_64 topic is always based on a verified replay of the shared
		# common series. It never carries a duplicate copy of those patches.
		"$0" --slice arm64 --cache "$CACHE_ROOT" --no-fetch
	fi
	verify_common_parent
	BASE_COMMIT=$(git -C "$SOURCE_DIR" rev-parse \
		"$UML_KERNEL_PARENT_BRANCH^{commit}")
fi

branch_exists=0
if git -C "$SOURCE_DIR" show-ref --verify --quiet \
	"refs/heads/$UML_KERNEL_PATCH_BRANCH"; then
	branch_exists=1
fi

if [ "$VERIFY_ONLY" -eq 1 ]; then
	[ "$branch_exists" -eq 1 ] ||
		fail "managed replay branch is missing: $UML_KERNEL_PATCH_BRANCH"
	current_branch=$(git -C "$SOURCE_DIR" branch --show-current)
	[ "$current_branch" = "$UML_KERNEL_PATCH_BRANCH" ] ||
		fail "HEAD is '$current_branch'; expected $UML_KERNEL_PATCH_BRANCH"
	require_clean_checkout
	verify_managed_branch
	printf '%s\n' "verified $UML_KERNEL_SLICE slice in $SOURCE_DIR on $UML_KERNEL_PATCH_BRANCH"
	exit 0
fi

require_clean_checkout

if [ "$branch_exists" -eq 1 ] && [ "$RECREATE" -eq 0 ]; then
	git -C "$SOURCE_DIR" switch "$UML_KERNEL_PATCH_BRANCH"
	verify_managed_branch
	printf '%s\n' "prepared $UML_KERNEL_SLICE slice in $SOURCE_DIR on $UML_KERNEL_PATCH_BRANCH"
	exit 0
fi

if [ "$branch_exists" -eq 1 ]; then
	old_managed_tip=$(git -C "$SOURCE_DIR" rev-parse \
		"$UML_KERNEL_PATCH_BRANCH^{commit}")
	# A stale replay is expected to differ from the current series in any
	# topic. Trust only the configured managed source and branch identity here,
	# plus linear ancestry from the pinned base. The old tip remains recoverable
	# below; the replacement candidate must pass the complete current series
	# verification before installation.
	git -C "$SOURCE_DIR" merge-base --is-ancestor "$UPSTREAM_BASE_COMMIT" \
		"$old_managed_tip" ||
		fail "refusing to recreate branch outside the pinned kernel ancestry"
	[ -z "$(git -C "$SOURCE_DIR" rev-list --merges \
		"$UPSTREAM_BASE_COMMIT..$old_managed_tip")" ] ||
		fail "refusing to recreate a managed replay containing a merge"
	RECREATE_BACKUP_REF="refs/uml/recreate-backups/$UML_KERNEL_SLICE/$old_managed_tip"
	if git -C "$SOURCE_DIR" show-ref --verify --quiet "$RECREATE_BACKUP_REF"; then
		[ "$(git -C "$SOURCE_DIR" rev-parse "$RECREATE_BACKUP_REF^{commit}")" = \
			"$old_managed_tip" ] ||
			fail "recreate backup ref points at an unexpected commit: $RECREATE_BACKUP_REF"
	else
		git -C "$SOURCE_DIR" update-ref "$RECREATE_BACKUP_REF" \
			"$old_managed_tip" ''
	fi
	REPLACEMENT_BRANCH="${UML_KERNEL_PATCH_BRANCH}.recreate-$$"
	git -C "$SOURCE_DIR" show-ref --verify --quiet \
		"refs/heads/$REPLACEMENT_BRANCH" &&
		fail "temporary recreate branch already exists: $REPLACEMENT_BRANCH"
	TARGET_PATCH_BRANCH=$UML_KERNEL_PATCH_BRANCH
	cleanup_replacement()
	{
		status=$?
		trap - 0 1 2 15
		git -C "$SOURCE_DIR" am --abort >/dev/null 2>&1 || true
		if git -C "$SOURCE_DIR" show-ref --verify --quiet \
				"refs/heads/$TARGET_PATCH_BRANCH"; then
			git -C "$SOURCE_DIR" switch "$TARGET_PATCH_BRANCH" \
				>/dev/null 2>&1 || true
		else
			git -C "$SOURCE_DIR" switch --detach "$old_managed_tip" \
				>/dev/null 2>&1 || true
		fi
		git -C "$SOURCE_DIR" branch -D "$REPLACEMENT_BRANCH" \
			>/dev/null 2>&1 || true
		exit "$status"
	}
	trap cleanup_replacement 0 1 2 15
	git -C "$SOURCE_DIR" switch -c "$REPLACEMENT_BRANCH" "$BASE_COMMIT"
	CREATE_BRANCH=$REPLACEMENT_BRANCH
else
	CREATE_BRANCH=$UML_KERNEL_PATCH_BRANCH
fi

if [ "$branch_exists" -eq 0 ]; then
	git -C "$SOURCE_DIR" switch -c "$CREATE_BRANCH" "$BASE_COMMIT"
fi
# Patch authors and author dates come from the checked-in mail patches. The
# replay committer is whoever Git resolves for the caller and its timestamp is
# the real replay time; neither generated commit ID is treated as a source
# identity.
unset GIT_AUTHOR_DATE GIT_COMMITTER_DATE
if [ -n "$UML_KERNEL_PARENT_BRANCH" ]; then
	kernel_series replay --source "$SOURCE_DIR" \
		--after "$UML_KERNEL_PARENT_BRANCH" >/dev/null ||
		fail "could not apply the $UML_KERNEL_SLICE series to its selected parent"
else
	kernel_series replay --source "$SOURCE_DIR" >/dev/null ||
		fail "could not apply the $UML_KERNEL_SLICE series to the pinned base"
fi

verify_managed_branch "$CREATE_BRANCH"
if [ "$branch_exists" -eq 1 ]; then
	candidate_tip=$(git -C "$SOURCE_DIR" rev-parse \
		"$CREATE_BRANCH^{commit}")
	git -C "$SOURCE_DIR" switch --detach "$candidate_tip"
	git -C "$SOURCE_DIR" update-ref \
		"refs/heads/$TARGET_PATCH_BRANCH" "$candidate_tip" \
		"$old_managed_tip" ||
		fail "managed replay changed during recreation; replacement was not installed"
	git -C "$SOURCE_DIR" switch "$TARGET_PATCH_BRANCH"
	git -C "$SOURCE_DIR" branch -D "$REPLACEMENT_BRANCH" >/dev/null
	trap - 0 1 2 15
	printf '%s\n' "preserved replaced $UML_KERNEL_SLICE replay at $RECREATE_BACKUP_REF" >&2
fi
require_clean_checkout
printf '%s\n' "prepared $UML_KERNEL_SLICE slice in $SOURCE_DIR on $UML_KERNEL_PATCH_BRANCH"
