#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Report how a slice's series carries onto a caller-selected development ref.
# This opt-in audit uses a temporary Git clone and never supplies source to a
# build or rewrites a managed pinned-commit checkout.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/mainline-6.18"
KERNEL_SERIES="$ROOT_DIR/kernel/common/tooling/kernel_series.py"
SLICE_SELECTOR="$LINE_ROOT/scripts/select-slice.sh"

fail()
{
	printf '%s\n' "kernel/mainline-6.18/scripts/check-next-version.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'USAGE'
Usage: kernel/mainline-6.18/scripts/check-next-version.sh \
       --work-root PATH --ref REF [--slice arm64|x86_64] [--keep]

Clone the upstream Linux origin, replay the slice's series on the pinned
commit, and rebase the result onto REF. A conflict stops with the topic and
paths that need a port; otherwise `git range-diff` shows every topic that
changed or became empty. This command never submits or pushes a change.
USAGE
}

WORK_ROOT=''
UPSTREAM_REF=''
KEEP=0
SLICE=arm64
while [ "$#" -gt 0 ]; do
	case "$1" in
	--slice)
		[ "$#" -ge 2 ] || fail "--slice requires arm64 or x86_64"
		SLICE=$2
		shift 2
		;;
	--work-root)
		[ "$#" -ge 2 ] || fail "--work-root requires a path"
		WORK_ROOT=$2
		shift 2
		;;
	--ref)
		[ "$#" -ge 2 ] || fail "--ref requires a value"
		UPSTREAM_REF=$2
		shift 2
		;;
	--keep)
		KEEP=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$WORK_ROOT" ] || fail "--work-root is required"
[ -n "$UPSTREAM_REF" ] || fail "--ref is required"
[ -r "$SLICE_SELECTOR" ] || fail "missing slice selector: $SLICE_SELECTOR"
# shellcheck disable=SC1090
. "$SLICE_SELECTOR"
uml_kernel_select_slice "$LINE_ROOT" "$SLICE" ||
	fail "cannot select kernel slice: $SLICE"
python3 -B "$KERNEL_SERIES" check --metadata "$UML_KERNEL_METADATA_PATH" ||
	fail "the $UML_KERNEL_SLICE series files are invalid"

mkdir -p -- "$WORK_ROOT"
WORK_ROOT=$(CDPATH= cd -- "$WORK_ROOT" && pwd -P)
CHECK_ROOT=$(mktemp -d "$WORK_ROOT/uml-linux-next.XXXXXX") ||
	fail "cannot create temporary checkout"
cleanup()
{
	if [ "$KEEP" -eq 0 ]; then
		rm -rf -- "$CHECK_ROOT"
	fi
}
trap cleanup 0 HUP INT TERM

audit_git()
{
	GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null git \
		-c core.hooksPath=/dev/null "$@"
}

# The audit clone ignores the caller's Git configuration, where Git also finds
# its identity. The rebase keeps every topic's author; carry forward the
# committer Git resolves for the caller, or nothing when it resolves none.
if committer=$(git var GIT_COMMITTER_IDENT 2>/dev/null); then
	GIT_COMMITTER_NAME=${committer%% <*}
	committer=${committer#*<}
	GIT_COMMITTER_EMAIL=${committer%%>*}
	export GIT_COMMITTER_NAME GIT_COMMITTER_EMAIL
fi

LINUX="$CHECK_ROOT/linux"
audit_git clone --filter=blob:none --no-checkout "$UML_KERNEL_UPSTREAM_REPOSITORY" "$LINUX"
audit_git -C "$LINUX" fetch --no-tags origin "$UPSTREAM_REF"
NEXT_BASE=$(audit_git -C "$LINUX" rev-parse FETCH_HEAD^{commit})
audit_git -C "$LINUX" fetch --no-tags origin "$UML_KERNEL_UPSTREAM_COMMIT"
audit_git -C "$LINUX" switch --quiet --detach "$UML_KERNEL_UPSTREAM_COMMIT"
python3 -B "$KERNEL_SERIES" replay --metadata "$UML_KERNEL_METADATA_PATH" \
	--source "$LINUX" >/dev/null ||
	fail "the $UML_KERNEL_SLICE series does not replay on its pinned commit"
audit_git -C "$LINUX" branch pinned HEAD
audit_git -C "$LINUX" switch --quiet -c next pinned

if ! audit_git -C "$LINUX" rebase --empty=drop --onto "$NEXT_BASE" \
		"$UML_KERNEL_UPSTREAM_COMMIT" next >/dev/null 2>&1; then
	printf '%s\n' "topic needs a port onto $UPSTREAM_REF:" >&2
	audit_git -C "$LINUX" log -1 --format='  %s' REBASE_HEAD >&2 || true
	audit_git -C "$LINUX" diff --name-only --diff-filter=U | sed 's/^/    /' >&2
	[ "$KEEP" -eq 1 ] && printf '%s\n' "stopped rebase retained: $LINUX" >&2
	exit 1
fi

audit_git -C "$LINUX" range-diff --no-color \
	"$UML_KERNEL_UPSTREAM_COMMIT..pinned" "$NEXT_BASE..next"
printf '%s\n' "$UML_KERNEL_SLICE series rebases onto $UPSTREAM_REF"
if [ "$KEEP" -eq 1 ]; then
	trap - 0 HUP INT TERM
	printf '%s\n' "next-version checkout retained: $LINUX"
fi
