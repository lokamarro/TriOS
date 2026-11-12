#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Verify the prepared Android producer host and its local source/tool inputs.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
AOSP_METADATA="$ROOT_DIR/android/lineage-24/metadata/lineage.env"
MESA_VENUS_VERIFIER="$ROOT_DIR/android/lineage-24/tools/verify-mesa-venus-source.py"

AOSP_SOURCE=
DARWIN_SDK=
WORK_ROOT=
RELEASE_ROOT=
SOURCE_ONLY=0

# shellcheck disable=SC1090
. "$SCRIPT_DIR/common.sh"

usage()
{
	cat <<'EOF'
Usage: android/lineage-24/build/preflight.sh \
       --aosp-source ABSOLUTE_PATH \
       [--source-only | \
        --darwin-sdk ABSOLUTE_PATH \
        --work-root ABSOLUTE_PATH \
        --release-root ABSOLUTE_PATH]

Run locally on the x86_64 Linux builder after source preparation. Source-only
mode verifies exact source identities. Full mode additionally verifies the
Darwin SDK, pinned checked-out toolchains, and writable builder-local roots.
EOF
}

require_pinned_checkout()
{
	repository=$1
	expected_commit=$2
	expected_tree=$3
	label=$4
	[ -e "$repository/.git" ] || [ -d "$repository/.git" ] ||
		fail "$label is not a Git checkout: $repository"
	actual_commit=$(git -C "$repository" rev-parse HEAD)
	[ "$actual_commit" = "$expected_commit" ] ||
		fail "$label commit is $actual_commit, expected $expected_commit"
	actual_tree=$(git -C "$repository" rev-parse HEAD^{tree})
	[ "$actual_tree" = "$expected_tree" ] ||
		fail "$label tree is $actual_tree, expected $expected_tree"
	[ -z "$(git -C "$repository" status --porcelain)" ] ||
		fail "$label checkout is dirty"
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--aosp-source)
		[ "$#" -ge 2 ] || fail "--aosp-source requires a path"
		AOSP_SOURCE=$2
		shift 2
		;;
	--darwin-sdk)
		[ "$#" -ge 2 ] || fail "--darwin-sdk requires a path"
		DARWIN_SDK=$2
		shift 2
		;;
	--work-root)
		[ "$#" -ge 2 ] || fail "--work-root requires a path"
		WORK_ROOT=$2
		shift 2
		;;
	--release-root)
		[ "$#" -ge 2 ] || fail "--release-root requires a path"
		RELEASE_ROOT=$2
		shift 2
		;;
	--source-only)
		SOURCE_ONLY=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$AOSP_SOURCE" ] || fail "--aosp-source is required"
absolute_path "AOSP source" "$AOSP_SOURCE"
[ -d "$AOSP_SOURCE" ] || fail "AOSP source does not exist: $AOSP_SOURCE"

[ "$(uname -s)" = Linux ] || fail "builder host must be Linux"
[ "$(uname -m)" = x86_64 ] || fail "builder host must be x86_64"
[ -r /proc/meminfo ] || fail "builder memory information is unavailable"

[ -f "$AOSP_METADATA" ] || fail "missing AOSP metadata: $AOSP_METADATA"
[ -x "$MESA_VENUS_VERIFIER" ] ||
	fail "missing Mesa Venus source verifier: $MESA_VENUS_VERIFIER"
# shellcheck disable=SC1090
. "$AOSP_METADATA"

[ "$LINEAGE24_ANDROID_MAJOR" -eq 17 ] || fail "AOSP metadata is not Android 17"
[ "$LINEAGE24_KERNEL_PAGE_SIZE" -eq 4096 ] || fail "Android kernel page size is not 4096"
[ "$LINEAGE24_MAX_PAGE_SIZE_SUPPORTED" -eq 16384 ] ||
	fail "Android maximum page size is not 16384"

command -v git >/dev/null 2>&1 || fail "git is not available on PATH"
command -v repo >/dev/null 2>&1 || fail "repo is not available on PATH"
[ -d "$AOSP_SOURCE/.repo/manifests" ] || fail "AOSP Repo metadata is missing"

require_pinned_checkout "$AOSP_SOURCE/.repo/manifests" \
	"$LINEAGE24_MANIFEST_COMMIT" "$LINEAGE24_MANIFEST_TREE" \
	"AOSP manifest"
dirty_projects=$(dirty_repo_projects "$AOSP_SOURCE")
if [ -n "$dirty_projects" ]; then
	fail "AOSP Repo workspace has dirty projects: $dirty_projects"
fi
"$MESA_VENUS_VERIFIER" --android-root "$ROOT_DIR/android/lineage-24" \
	--aosp-source "$AOSP_SOURCE"

if [ "$SOURCE_ONLY" -eq 1 ]; then
	[ -z "$DARWIN_SDK$WORK_ROOT$RELEASE_ROOT" ] ||
		fail "--source-only cannot be combined with build paths"
	printf '%s\n' "Android 17 source preflight passed"
	exit 0
fi

command -v zip >/dev/null 2>&1 || fail "zip is not available on PATH"

[ -n "$DARWIN_SDK" ] || fail "--darwin-sdk is required"
[ -n "$WORK_ROOT" ] || fail "--work-root is required"
[ -n "$RELEASE_ROOT" ] || fail "--release-root is required"
absolute_path "Darwin SDK" "$DARWIN_SDK"
absolute_path "work root" "$WORK_ROOT"
absolute_path "release root" "$RELEASE_ROOT"
[ -d "$DARWIN_SDK" ] || fail "Darwin SDK does not exist: $DARWIN_SDK"
[ -f "$DARWIN_SDK/SDKSettings.json" ] ||
	fail "Darwin SDK is missing SDKSettings.json"
[ -d "$WORK_ROOT" ] && [ -w "$WORK_ROOT" ] ||
	fail "work root must exist and be writable: $WORK_ROOT"
[ -d "$RELEASE_ROOT" ] && [ -w "$RELEASE_ROOT" ] ||
	fail "release root must exist and be writable: $RELEASE_ROOT"

AOSP_JAVA="$AOSP_SOURCE/prebuilts/jdk/jdk21/linux-x86/bin/java"
AOSP_NINJA="$AOSP_SOURCE/prebuilts/build-tools/linux-x86/bin/ninja"
for tool in "$AOSP_JAVA" "$AOSP_NINJA"; do
	[ -x "$tool" ] || fail "required checked-out tool is missing: $tool"
done

memory_kib=$(awk '/^MemTotal:/ { print $2; exit }' /proc/meminfo)
[ -n "$memory_kib" ] || fail "cannot read total builder memory"
[ "$memory_kib" -ge 67108864 ] ||
	fail "builder has less than 64 GiB of memory"
available_kib=$(df -Pk "$WORK_ROOT" | awk 'NR == 2 { print $4 }')
[ -n "$available_kib" ] || fail "cannot read work-root free space"
[ "$available_kib" -ge 419430400 ] ||
	fail "work root has less than 400 GiB available"

printf '%s\n' "Android 17 builder preflight passed"
