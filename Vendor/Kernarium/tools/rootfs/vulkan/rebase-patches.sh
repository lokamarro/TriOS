#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

fail()
{
	printf '%s\n' "error: $*" >&2
	exit 1
}

usage()
{
	printf '%s\n' \
		"usage: $0 --component NAME --source TOPIC_CHECKOUT [--head REF]" \
		"" \
		"REF (default HEAD) ends the component's topics. A host layer may follow" \
		"them in the same checkout; see dependencies/rebase-patches.sh." >&2
	exit 2
}

COMPONENT=
SOURCE=
HEAD_REF=HEAD
while [ "$#" -gt 0 ]; do
	case "$1" in
		--component)
			[ "$#" -ge 2 ] || usage
			COMPONENT=$2
			shift 2
			;;
		--source)
			[ "$#" -ge 2 ] || usage
			SOURCE=$2
			shift 2
			;;
		--head)
			[ "$#" -ge 2 ] || usage
			HEAD_REF=$2
			shift 2
			;;
		*) usage ;;
	esac
done
[ -n "$COMPONENT" ] && [ -n "$SOURCE" ] || usage

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
SOURCE=$(CDPATH= cd -- "$SOURCE" && pwd -P)
LOCK="$SCRIPT_DIR/sources.tsv"
[ -f "$LOCK" ] || fail "source lock is missing: $LOCK"
git -C "$SOURCE" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
	fail "source is not a Git checkout: $SOURCE"

tab=$(printf '\t')
row=$(awk -F '\t' -v component="$COMPONENT" '
	$1 == component { print; count++ }
	END { if (count != 1) exit 1 }
' "$LOCK") || fail "component is not uniquely pinned: $COMPONENT"
IFS="$tab" read -r locked_component repository base base_tree release_ref release_object series <<EOF
$row
EOF
[ "$locked_component" = "$COMPONENT" ] || fail "source-lock parser drift"
[ "$series" != '-' ] || fail "component carries no patch series: $COMPONENT"
[ "$(git -C "$SOURCE" rev-parse "$base^{tree}" 2>/dev/null || true)" = "$base_tree" ] ||
	fail "base tree does not match source lock: $base_tree"

series_directory=$(dirname -- "$SCRIPT_DIR/$series")
[ -d "$series_directory" ] || fail "patch directory is missing: $series_directory"
python3 -B "$SCRIPT_DIR/../../../scripts/patch_series.py" export --source "$SOURCE" \
	--base "$base" --head "$HEAD_REF" --series-dir "$series_directory" --tree >/dev/null
printf '%s\n' "regenerated $COMPONENT patch series from $SOURCE"
