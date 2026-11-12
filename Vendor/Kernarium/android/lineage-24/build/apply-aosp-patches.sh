#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Apply every routed AOSP project series to an exact pinned Repo workspace.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ANDROID_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
ROOT_DIR=$(CDPATH= cd -- "$ANDROID_ROOT/../.." && pwd)
PATCH_SERIES="$ROOT_DIR/scripts/patch_series.py"
PROJECTS="$ANDROID_ROOT/patches/projects.tsv"
VERIFIER="$ANDROID_ROOT/tools/verify-aosp-patches.py"
DEVICE_SYNC="$ANDROID_ROOT/tools/sync-device-tree.py"
SEPOLICY_CHECK="$ANDROID_ROOT/tools/check-overlay-sepolicy.py"
MESA_VENUS_VERIFIER="$ANDROID_ROOT/tools/verify-mesa-venus-source.py"
AOSP_SOURCE=

# shellcheck disable=SC1090
. "$SCRIPT_DIR/common.sh"

usage()
{
	cat <<'EOF'
Usage: android/lineage-24/build/apply-aosp-patches.sh \
       --aosp-source ABSOLUTE_PATH

Run locally on the x86_64 Linux builder after source preparation. Every routed
project must be clean and at the exact revision in the resolved manifest lock.
The command applies generated per-project patches, then exactly synchronizes
the checked device overlay to device/kernarium/andarium. It performs no fetch or
transport operation.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--aosp-source)
		[ "$#" -ge 2 ] || fail "--aosp-source requires a path"
		AOSP_SOURCE=$2
		shift 2
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
[ "$(uname -s)" = Linux ] || fail "builder host must be Linux"
[ "$(uname -m)" = x86_64 ] || fail "builder host must be x86_64"
[ -d "$AOSP_SOURCE/.repo/manifests" ] || fail "AOSP Repo metadata is missing"
[ -x "$VERIFIER" ] || fail "AOSP patch verifier is missing"
[ -x "$DEVICE_SYNC" ] || fail "device-tree sync tool is missing"
[ -x "$SEPOLICY_CHECK" ] || fail "overlay sepolicy check is missing"
[ -x "$MESA_VENUS_VERIFIER" ] || fail "Mesa Venus source verifier is missing"
[ -f "$PROJECTS" ] || fail "AOSP route table is missing"

"$SEPOLICY_CHECK" --overlay "$ANDROID_ROOT/overlay"
"$MESA_VENUS_VERIFIER" --android-root "$ANDROID_ROOT" \
	--aosp-source "$AOSP_SOURCE"
"$VERIFIER" --android-root "$ANDROID_ROOT" \
	--aosp-source "$AOSP_SOURCE" --expect base

awk -F '\t' '$1 ~ /^[0-9]+$/ { print $2 "\t" $5 "\t" $9 "\t" $10 }' "$PROJECTS" |
while IFS="$(printf '\t')" read -r route_id checkout_path final_tree series_dir; do
	python3 -B "$PATCH_SERIES" apply --repo "$AOSP_SOURCE/$checkout_path" \
		--series-dir "$ANDROID_ROOT/$series_dir" \
		--expect-tree "$final_tree" >/dev/null ||
		fail "could not apply the $route_id series"
done

"$DEVICE_SYNC" --android-root "$ANDROID_ROOT" --workspace "$AOSP_SOURCE"
"$VERIFIER" --android-root "$ANDROID_ROOT" \
	--aosp-source "$AOSP_SOURCE" --expect final
printf '%s\n' "applied routed AOSP 17 project patches"
