#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Export one maintained dependency topic checkout as its checked-in patch
# series and the tree it produces. Never edit the generated .patch files or
# series.tsv by hand.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT_DIR/dependencies/series-field.sh"
MANIFEST=${UML_DEPENDENCY_MANIFEST:-"$ROOT_DIR/dependencies/manifest.tsv"}
METADATA_ROOT=${UML_DEPENDENCY_METADATA_ROOT:-"$ROOT_DIR/dependencies"}
DEPENDENCY=
SOURCE=

fail()
{
	printf '%s\n' "dependencies/rebase-patches.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: dependencies/rebase-patches.sh --dependency NAME --source PATH

PATH must be a clean Git checkout whose HEAD is the pinned upstream commit
followed by the dependency's topic commits. Amend, reorder, add, or drop
topics there (`git rebase -i <pinned-commit>`), then run this command to
export the series and record its tree.

A layered series ("lower+owned" in the manifest) exports only its owned,
last layer. Its topics follow the lower layers' topics in the checkout, which
end at the one commit whose tree the lower layers record; export a changed
lower layer with its owner's tool first.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--dependency)
		[ "$#" -ge 2 ] || fail "--dependency requires a name"
		DEPENDENCY=$2
		shift 2
		;;
	--source)
		[ "$#" -ge 2 ] || fail "--source requires a path"
		SOURCE=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$DEPENDENCY" ] && [ -n "$SOURCE" ] || {
	usage >&2
	exit 1
}
case "$DEPENDENCY" in
*[!A-Za-z0-9._+-]*|'') fail "unsafe dependency name: $DEPENDENCY" ;;
esac
[ -f "$MANIFEST" ] || fail "manifest is missing: $MANIFEST"
[ "$(git -C "$SOURCE" rev-parse --is-inside-work-tree 2>/dev/null || true)" = true ] &&
	[ "$(CDPATH= cd -- "$SOURCE" && pwd -P)" = \
		"$(CDPATH= cd -- "$(git -C "$SOURCE" rev-parse --show-toplevel)" && pwd -P)" ] ||
	fail "source is not a Git working tree: $SOURCE"

row=$(awk -F '\t' -v wanted="$DEPENDENCY" '$1 == wanted { print; exit }' \
	"$MANIFEST")
[ -n "$row" ] || fail "$DEPENDENCY is absent from the manifest"
tab=$(printf '\t')
IFS="$tab" read -r name version products repository base source_paths series <<EOF
$row
EOF
[ "$name" = "$DEPENDENCY" ] || fail "could not parse dependency row"
[ "$series" != '-' ] || fail "$DEPENDENCY has no maintained patch series"
series_field_split "$series"
owned=$SERIES_FIELD_OWNED
lower=$SERIES_FIELD_LOWER
case "$owned" in
rootfs-vulkan:*)
	fail "$DEPENDENCY is owned by tools/rootfs/vulkan/rebase-patches.sh"
	;;
*:*) fail "unknown patch-series owner: $owned" ;;
esac

series_tool()
{
	python3 -B "$ROOT_DIR/scripts/patch_series.py" "$@"
}

export_base=$base
if [ -n "$lower" ]; then
	# The only layer the manifest may name below a dependency's own is one
	# series the paired Vulkan guest owns.
	case "$lower" in
	rootfs-vulkan:*) lower_relative=${lower#rootfs-vulkan:} ;;
	*) lower_relative= ;;
	esac
	series_field_rootfs_vulkan_allowed "$DEPENDENCY" "$lower_relative" ||
		fail "$DEPENDENCY has an unsupported lower patch-series layer: $lower"
	lower_directory=$(dirname -- \
		"$METADATA_ROOT/../$SERIES_FIELD_ROOTFS_VULKAN/$lower_relative")
	lower_tree=$(series_tool recorded-tree --series-dir "$lower_directory") ||
		fail "the lower layer records no tree: $lower"
	boundaries=$(git -C "$SOURCE" rev-list --format='%T %H' --no-commit-header \
		"$base..HEAD" | awk -v tree="$lower_tree" '$1 == tree { print $2 }')
	[ -n "$boundaries" ] && [ "$(printf '%s\n' "$boundaries" | wc -l | tr -d ' ')" = 1 ] ||
		fail "exactly one commit of $SOURCE must end the lower layer with tree $lower_tree"
	export_base=$boundaries
fi

series_directory=$(dirname -- "$METADATA_ROOT/$owned")
[ -d "$series_directory" ] || fail "patch directory is missing: $series_directory"
series_tool export --source "$SOURCE" \
	--base "$export_base" --head HEAD --series-dir "$series_directory" --tree >/dev/null
printf '%s\n' "regenerated $DEPENDENCY patch series from $SOURCE"
