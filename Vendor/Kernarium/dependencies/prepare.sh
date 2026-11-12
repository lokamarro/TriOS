#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Materialize immutable upstream Git commits and replay only the ordered mail
# patches declared in manifest.tsv. This is the sole dependency step allowed
# to access the network.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT_DIR/dependencies/series-field.sh"
PATCH_SERIES="$ROOT_DIR/scripts/patch_series.py"
MANIFEST=${UML_DEPENDENCY_MANIFEST:-"$ROOT_DIR/dependencies/manifest.tsv"}
PATCH_ROOT=${UML_DEPENDENCY_PATCH_ROOT:-"$ROOT_DIR/dependencies"}
DEPENDENCY_ROOT=${UML_DEPENDENCY_SOURCE_ROOT:-"$ROOT_DIR/external/sources"}
CACHE_ROOT=${UML_DEPENDENCY_CACHE:-"$ROOT_DIR/.build/dependency-cache"}
PRODUCT=${UML_DEPENDENCY_PRODUCT:-macos}
VERIFY_ONLY=0
FETCH=1
RECREATE=0
VALIDATE_MANIFEST_ONLY=0
RESOLVE_REPOSITORY=
RESOLVE_REF=
ARCHIVE_MANIFEST=
TAG_REPOSITORY=
TAG_REF=
TAG_OBJECT=
TAG_COMMIT=
TAG_CHECKOUT=

fail()
{
	printf '%s\n' "dependencies/prepare.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: dependencies/prepare.sh [options]

Options:
  --product NAME  macos (default), ios-network, ios-storage, ios-compression,
                  apple-gpu-vulkan-experiment, kernel-host-tools,
                  x86-conformance, or all
  --root PATH     Prepared source root (default: external/sources); a source
                  with paths that differ only by case needs a case-sensitive
                  file system, and is refused on any other
  --cache PATH    Git cache (default: .build/dependency-cache)
  --verify        Verify prepared repositories without changing or fetching
  --no-fetch      Prepare only from commits already present in the Git cache
  --recreate      Replace a clean prepared source of the same upstream whose
                  pinned commit or patch series changed
  --validate-manifest-only
                  Validate every source and patch-series identity only
  --archive-manifest PATH
                  Fetch SHA-256-pinned HTTPS archives into CACHE/archives
  --resolve-ref REPOSITORY REF
                  Print the object ID of one exact HTTPS branch/tag ref,
                  peeling annotated tags; use it to pin a source manifest
  --materialize-tag REPOSITORY REF OBJECT COMMIT CHECKOUT
                  Fetch an exact annotated tag into an existing checkout,
                  verifying both pinned identities before publishing its ref
  -h, --help      Show this help

A prepared source is identified by the tree its patch series records.
Preparation replaces an existing, mismatched repository only with --recreate,
and then only a clean one of the same upstream repository whose branches
point only at commits the Git cache already holds.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--product)
		[ "$#" -ge 2 ] || fail "--product requires a name"
		PRODUCT=$2
		shift 2
		;;
	--root)
		[ "$#" -ge 2 ] || fail "--root requires a path"
		DEPENDENCY_ROOT=$2
		shift 2
		;;
	--cache)
		[ "$#" -ge 2 ] || fail "--cache requires a path"
		CACHE_ROOT=$2
		shift 2
		;;
	--verify)
		VERIFY_ONLY=1
		FETCH=0
		shift
		;;
	--no-fetch)
		FETCH=0
		shift
		;;
	--recreate)
		RECREATE=1
		shift
		;;
	--validate-manifest-only)
		VALIDATE_MANIFEST_ONLY=1
		shift
		;;
	--archive-manifest)
		[ "$#" -ge 2 ] || fail "--archive-manifest requires a path"
		ARCHIVE_MANIFEST=$2
		shift 2
		;;
	--resolve-ref)
		[ "$#" -ge 3 ] || fail "--resolve-ref requires a repository and exact ref"
		RESOLVE_REPOSITORY=$2
		RESOLVE_REF=$3
		shift 3
		;;
	--materialize-tag)
		[ "$#" -ge 6 ] || fail "--materialize-tag requires repository, ref, object, commit, and checkout"
		TAG_REPOSITORY=$2
		TAG_REF=$3
		TAG_OBJECT=$4
		TAG_COMMIT=$5
		TAG_CHECKOUT=$6
		shift 6
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

case "$PRODUCT" in
*[!A-Za-z0-9._+-]*|'') fail "unsafe dependency product: $PRODUCT" ;;
esac

sha256_file()
{
	if command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | awk '{print $1}'
	else
		sha256sum "$1" | awk '{print $1}'
	fi
}

if [ -n "$ARCHIVE_MANIFEST" ]; then
	[ -z "$RESOLVE_REPOSITORY" ] && [ -z "$TAG_REPOSITORY" ] ||
		fail "archive preparation cannot be combined with ref discovery"
	python3 - "$ARCHIVE_MANIFEST" <<'PY_ARCHIVES'
import csv
import pathlib
import re
import sys
import urllib.parse

with pathlib.Path(sys.argv[1]).open() as stream:
    rows = list(csv.reader((line for line in stream if not line.startswith("#")), delimiter="\t"))
if not rows or rows[0] != ["name", "url", "sha256"] or len(rows) < 2:
    raise SystemExit("invalid dependency archive manifest header")
seen = set()
for row in rows[1:]:
    if len(row) != 3:
        raise SystemExit("invalid dependency archive manifest row")
    name, url, digest = row
    parsed = urllib.parse.urlsplit(url)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+%~-]*", name) or name == "name" or name in seen:
        raise SystemExit("unsafe or repeated dependency archive name")
    if parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password or parsed.fragment:
        raise SystemExit("dependency archives require HTTPS URLs without credentials")
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise SystemExit("dependency archives require SHA-256 identities")
    seen.add(name)
PY_ARCHIVES
	if [ "$VALIDATE_MANIFEST_ONLY" -eq 1 ]; then
		printf 'validated dependency archive manifest: %s\n' "$ARCHIVE_MANIFEST"
		exit 0
	fi
	if [ "$VERIFY_ONLY" -eq 1 ]; then
		[ -d "$CACHE_ROOT/archives" ] || fail "dependency archive cache is absent"
	else
		mkdir -p -- "$CACHE_ROOT/archives"
	fi
	tab=$(printf '\t')
	while IFS="$tab" read -r archive_name archive_url archive_sha256; do
		case "$archive_name" in \#*|name|'') continue ;; esac
		archive="$CACHE_ROOT/archives/$archive_sha256-$archive_name"
		if [ ! -f "$archive" ]; then
			[ "$FETCH" -eq 1 ] && [ "$VERIFY_ONLY" -eq 0 ] ||
				fail "dependency archive is absent: $archive"
			temporary_archive=$(mktemp "$CACHE_ROOT/archives/.archive.XXXXXX")
			if ! curl --fail --location --proto '=https' --proto-redir '=https' \
				--retry 3 --connect-timeout 15 --max-time 300 \
				--output "$temporary_archive" "$archive_url"; then
				rm -f -- "$temporary_archive"
				fail "dependency archive download failed: $archive_name"
			fi
			[ "$(sha256_file "$temporary_archive")" = "$archive_sha256" ] || {
				rm -f -- "$temporary_archive"
				fail "downloaded dependency archive hash differs: $archive_name"
			}
			mv -- "$temporary_archive" "$archive"
		else
			[ "$(sha256_file "$archive")" = "$archive_sha256" ] ||
				fail "cached dependency archive hash differs: $archive_name"
		fi
		printf 'verified dependency archive: %s\n' "$archive"
	done < "$ARCHIVE_MANIFEST"
	exit 0
fi

if [ -n "$TAG_REPOSITORY" ]; then
	[ "$FETCH" -eq 1 ] && [ "$VERIFY_ONLY" -eq 0 ] &&
		[ "$VALIDATE_MANIFEST_ONLY" -eq 0 ] && [ -z "$RESOLVE_REPOSITORY" ] ||
		fail "--materialize-tag cannot be combined with other preparation modes"
	case "$TAG_REPOSITORY" in
	https://?*) ;;
	*) fail "--materialize-tag requires an HTTPS repository" ;;
	esac
	case "$TAG_REF" in
	refs/tags/?*) ;;
	*) fail "--materialize-tag requires an exact refs/tags/ name" ;;
	esac
	git check-ref-format "$TAG_REF" >/dev/null 2>&1 || fail "invalid tag ref"
	for identity in "$TAG_OBJECT" "$TAG_COMMIT"; do
		case "$identity" in
		*[!0-9a-f]*|'') fail "tag identities must be SHA-1 object IDs" ;;
		esac
		[ "${#identity}" -eq 40 ] || fail "tag identities must be SHA-1 object IDs"
	done
	git -C "$TAG_CHECKOUT" rev-parse --git-dir >/dev/null 2>&1 ||
		fail "tag destination is not an existing Git checkout"
	existing=$(git -C "$TAG_CHECKOUT" rev-parse --verify "$TAG_REF" 2>/dev/null || true)
	[ -z "$existing" ] || [ "$existing" = "$TAG_OBJECT" ] ||
		fail "existing tag ref differs from its pinned object"
	git -C "$TAG_CHECKOUT" fetch --no-tags "$TAG_REPOSITORY" "$TAG_REF" ||
		fail "could not fetch pinned tag"
	[ "$(git -C "$TAG_CHECKOUT" rev-parse FETCH_HEAD)" = "$TAG_OBJECT" ] &&
		[ "$(git -C "$TAG_CHECKOUT" cat-file -t "$TAG_OBJECT")" = tag ] &&
		[ "$(git -C "$TAG_CHECKOUT" rev-parse "$TAG_OBJECT^{commit}")" = "$TAG_COMMIT" ] ||
		fail "fetched annotated tag differs from its pinned identities"
	git -C "$TAG_CHECKOUT" update-ref "$TAG_REF" "$TAG_OBJECT" \
		"${existing:-0000000000000000000000000000000000000000}"
	printf 'verified dependency tag: %s %s %s\n' "$TAG_REF" "$TAG_OBJECT" "$TAG_COMMIT"
	exit 0
fi

if [ -n "$RESOLVE_REPOSITORY" ]; then
	[ "$FETCH" -eq 1 ] && [ "$VERIFY_ONLY" -eq 0 ] &&
		[ "$VALIDATE_MANIFEST_ONLY" -eq 0 ] ||
		fail "--resolve-ref cannot be combined with offline or preparation modes"
	case "$RESOLVE_REPOSITORY" in
	https://?*) ;;
	*) fail "--resolve-ref requires an HTTPS repository" ;;
	esac
	case "$RESOLVE_REF" in
	refs/heads/?*|refs/tags/?*) ;;
	*) fail "--resolve-ref requires an exact refs/heads/ or refs/tags/ name" ;;
	esac
	git check-ref-format "$RESOLVE_REF" >/dev/null 2>&1 ||
		fail "--resolve-ref has an invalid ref name"
	resolved=$(git ls-remote --exit-code "$RESOLVE_REPOSITORY" \
		"$RESOLVE_REF" "$RESOLVE_REF^{}") || fail "dependency ref was not found"
	object=$(printf '%s\n' "$resolved" | awk -v ref="$RESOLVE_REF" '
		$2 == ref { if (direct) exit 1; direct = $1; next }
		$2 == ref "^{}" { if (peeled) exit 1; peeled = $1; next }
		{ exit 1 }
		END { if (!direct) exit 1; print peeled ? peeled : direct }
	') || fail "dependency ref response was ambiguous"
	case "$object" in
	*[!0-9a-f]*|'') fail "dependency ref did not resolve to a Git object ID" ;;
	esac
	[ "${#object}" -eq 40 ] || fail "dependency ref object ID must be SHA-1"
	ref_object=$(printf '%s\n' "$resolved" | awk -v ref="$RESOLVE_REF" '$2 == ref { print $1 }')
	printf 'resolved %s %s: ref-object=%s peeled-object=%s\n' \
		"$RESOLVE_REPOSITORY" "$RESOLVE_REF" "$ref_object" "$object" >&2
	printf '%s\n' "$object"
	exit 0
fi

command -v git >/dev/null 2>&1 || fail "git build tool is required"
command -v python3 >/dev/null 2>&1 || fail "python3 build tool is required"
if ! command -v shasum >/dev/null 2>&1 &&
	! command -v sha256sum >/dev/null 2>&1; then
	fail "shasum or sha256sum build tool is required"
fi
[ -r "$MANIFEST" ] || fail "missing dependency manifest: $MANIFEST"
[ -d "$PATCH_ROOT" ] || fail "missing dependency metadata root: $PATCH_ROOT"

if ! awk -F '\t' '
	/^#/ || /^[[:space:]]*$/ { next }
	$1 == "name" {
		if (header || data || NF != 7 || $2 != "version" ||
		    $3 != "products" || $4 != "repository" ||
		    $5 != "commit" || $6 != "source_paths" ||
		    $7 != "patch_series") {
			printf "invalid manifest header at row %d\n", NR > "/dev/stderr"
			exit 1
		}
		header = 1
		next
	}
	!header || NF != 7 {
		printf "malformed manifest row %d\n", NR > "/dev/stderr"
		exit 1
	}
	seen[$1]++ {
		printf "duplicate dependency %s at row %d\n", $1, NR > "/dev/stderr"
		exit 1
	}
	{ data = 1 }
	END {
		if (!header || !data) {
			print "manifest must contain one header and data" > "/dev/stderr"
			exit 1
		}
	}
' "$MANIFEST"; then
	fail "dependency manifest structure is invalid"
fi

tab=$(printf '\t')

safe_relative_path()
{
	case "$1" in
	''|/*|*\\*|..|../*|*/../*|*/..|*//* ) return 1 ;;
	esac
	return 0
}

# series-field.sh owns the field grammar. The last layer of a stack records
# the tree of the whole stack.
resolve_series_layer()
{
	case "$1" in
	rootfs-vulkan:*)
		series_relative=${1#rootfs-vulkan:}
		series_field_rootfs_vulkan_allowed "$name" "$series_relative" ||
			fail "$name has an unsupported rootfs-vulkan series: $1"
		metadata_outer=$(CDPATH= cd -- "$PATCH_ROOT/.." && pwd -P)
		series_owner="$metadata_outer/$SERIES_FIELD_ROOTFS_VULKAN"
		;;
	*:*) fail "$name has an unknown patch-series owner: $1" ;;
	*)
		series_relative=$1
		series_owner=$(CDPATH= cd -- "$PATCH_ROOT" && pwd -P)
		;;
	esac
	safe_relative_path "$series_relative" ||
		fail "$name has an unsafe patch-series path: $1"
	case "$series_relative" in
	*[!A-Za-z0-9._/-]*) fail "$name has an unsafe patch-series path: $1" ;;
	esac
	series_path="$series_owner/$series_relative"
	verify_metadata_path "$series_path"
	[ "${series_relative##*/}" = series.tsv ] ||
		fail "$name patch series must name series.tsv files: $patch_series"
	[ -f "$series_path" ] || fail "$name patch series is missing: $series_path"
}

# Sets SERIES_PATHS to the layers' series.tsv paths, one per line in
# application order, and series_path to the last of them.
resolve_series()
{
	case "$patch_series" in
	*[!A-Za-z0-9._/:+-]*|+*|*+|*++*)
		fail "$name has a malformed patch-series field: $patch_series" ;;
	esac
	SERIES_PATHS=
	old_ifs=$IFS
	IFS=+
	set -f
	for series_layer in $patch_series; do
		IFS=$old_ifs
		resolve_series_layer "$series_layer"
		SERIES_PATHS="${SERIES_PATHS}${SERIES_PATHS:+
}$series_path"
	done
	set +f
	IFS=$old_ifs
	series_field_split "$patch_series"
	if [ -n "$SERIES_FIELD_LOWER" ]; then
		case "$SERIES_FIELD_OWNED" in
		*:*) fail "$name must own the last layer of its patch series" ;;
		esac
	fi
}

# Every metadata path is absolute and joins a physical directory (pwd -P) to a
# validated relative path, so a symbolic link can only be one of its own
# components. Testing each prefix needs no process of its own.
verify_metadata_path()
{
	metadata_prefix=$1
	while [ -n "$metadata_prefix" ]; do
		[ ! -L "$metadata_prefix" ] ||
			fail "metadata path contains a symlink: $1"
		case $metadata_prefix in
		*/*) metadata_prefix=${metadata_prefix%/*} ;;
		*) break ;;
		esac
	done
}

validate_vulkan_pair()
{
	case "$name:$patch_series" in
	venus-protocol:*|vulkan-headers:*|mesa:rootfs-vulkan:*) ;;
	*) return 0 ;;
	esac
	metadata_outer=$(CDPATH= cd -- "$PATCH_ROOT/.." && pwd -P)
	paired_sources="$metadata_outer/tools/rootfs/vulkan/sources.tsv"
	verify_metadata_path "$paired_sources"
	[ -f "$paired_sources" ] || fail "paired Vulkan source lock is missing"
	paired_identity=$(awk -F '\t' -v component="$name" \
		'$1 == component { print $2 "\t" $3; count++ }
		END { if (count != 1) exit 1 }' "$paired_sources") ||
		fail "$name paired Vulkan source identity is missing or duplicated"
	[ "$paired_identity" = "$(printf '%s\t%s' "$repository" "$commit")" ] ||
		fail "$name host and guest Vulkan source identities differ"
	[ "$products" = apple-gpu-vulkan-experiment ] ||
		fail "$name is an experimental Vulkan dependency only"
	if [ "$name" = venus-protocol ]; then
		[ "$patch_series" = rootfs-vulkan:patches/venus-protocol/series.tsv ] ||
			fail "venus-protocol must use its canonical rootfs-vulkan series"
		[ "$(awk -F '\t' '$1 == "venus-protocol" { print $7 }' "$paired_sources")" = \
			patches/venus-protocol/series.tsv ] ||
			fail "venus-protocol paired series owner differs"
	elif [ "$name" = mesa ]; then
		[ "$patch_series" = \
			rootfs-vulkan:patches/mesa/series.tsv+patches/mesa/series.tsv ] ||
			fail "mesa must layer its own series on the canonical rootfs-vulkan series"
		[ "$(awk -F '\t' '$1 == "mesa" { print $7 }' "$paired_sources")" = \
			patches/mesa/series.tsv ] ||
			fail "mesa paired series owner differs"
	else
		[ "$patch_series" = - ] || fail "Vulkan-Headers must use the unmodified SDK"
	fi
}

validate_products()
{
	old_ifs=$IFS
	IFS=,
	set -- $products
	IFS=$old_ifs
	[ "$#" -gt 0 ] || fail "$name has no build product"
	for product_name in "$@"; do
		case "$product_name" in
		*[!A-Za-z0-9._+-]*|'') fail "$name has an unsafe build product" ;;
		esac
	done
}

# The manifest keeps its source_paths column, but every source is complete.
validate_source_paths()
{
	[ "$source_paths" = . ] ||
		fail "$name source_paths must be '.': partial sources are unsupported"
}

series_tool()
{
	python3 -B "$PATCH_SERIES" "$@"
}

# Adds the number of patch rows in the series file $1 to SERIES_COUNT.
count_series_patches()
{
	while IFS="$tab" read -r series_row_kind series_row_rest ||
		[ -n "$series_row_kind" ]; do
		if [ "$series_row_kind" = patch ]; then
			SERIES_COUNT=$((SERIES_COUNT + 1))
		fi
	done <"$1"
}

# A generated series records the tree it produces on the pinned commit. That
# tree, not the patch bytes, identifies the prepared source. Reading it checks
# every layer's files.
series_values()
{
	SERIES_COUNT=0
	SERIES_TREE=
	[ "$patch_series" != '-' ] || return 0
	resolve_series
	SERIES_COUNT=0
	old_ifs=$IFS
	IFS='
'
	set -f
	for layer_path in $SERIES_PATHS; do
		count_series_patches "$layer_path"
	done
	set +f
	IFS=$old_ifs
	SERIES_TREE=$(series_tool_layers recorded-tree) ||
		fail "$name patch series is invalid: $patch_series"
}

# Runs the series tool with one --series-dir per layer, in order, after the
# given arguments.
series_tool_layers()
{
	(
		IFS='
'
		set -f
		for layer_path in $SERIES_PATHS; do
			set -- "$@" --series-dir "$(dirname -- "$layer_path")"
		done
		set +f
		series_tool "$@"
	)
}

validate_series()
{
	[ "$patch_series" != '-' ] || return 0
	series_values
	[ "$SERIES_COUNT" -gt 0 ] || fail "$name patch series is empty"
}

validate_row()
{
	case "$name" in
	*[!A-Za-z0-9._+-]*|'') fail "unsafe dependency name: $name" ;;
	esac
	[ -n "$version" ] || fail "$name has an empty version"
	validate_products
	case "$repository" in
	https://*.git) ;;
	*) fail "$name repository must be an HTTPS Git URL ending in .git" ;;
	esac
	case "$commit" in
	*[!0-9a-f]*|'') fail "$name commit is invalid" ;;
	esac
	[ "${#commit}" -eq 40 ] || fail "$name commit must be a full Git OID"
	validate_source_paths
	validate_vulkan_pair
	validate_series
}

while IFS="$tab" read -r name version products repository commit source_paths \
	patch_series; do
	case "$name" in
	\#*|name|'') continue ;;
	esac
	validate_row
done < "$MANIFEST"

paired_count=$(awk -F '\t' '$1 == "vulkan-headers" || $1 == "venus-protocol" \
	{ count++ } END { print count + 0 }' "$MANIFEST")
[ "$paired_count" -eq 0 ] || [ "$paired_count" -eq 2 ] ||
	fail "the experimental Vulkan SDK and protocol inputs must be declared together"

validate_patch_inventory()
{
	manifest_series=$(awk -F '\t' '
		/^#/ || /^[[:space:]]*$/ || $1 == "name" || $7 == "-" { next }
		{
			count = split($7, layers, "+")
			for (i = 1; i <= count; i++)
				if (layers[i] !~ /^rootfs-vulkan:/)
					print layers[i]
		}
	' "$MANIFEST" | LC_ALL=C sort -u)
	directory_series=$(
		CDPATH= cd -- "$PATCH_ROOT"
		find patches -type f -name series.tsv -print 2>/dev/null | LC_ALL=C sort
	)
	[ "$directory_series" = "$manifest_series" ] ||
		fail "dependency patch-series inventory differs from manifest"
	manifest_patches=$(
		for relative_series in $manifest_series; do
			series_directory=$(dirname -- "$relative_series")
			awk -F '\t' -v prefix="$series_directory/" '
				$1 == "patch" { print prefix $2 }
			' "$PATCH_ROOT/$relative_series"
		done | LC_ALL=C sort
	)
	directory_patches=$(
		CDPATH= cd -- "$PATCH_ROOT"
		find patches -type f -name '*.patch' -print | LC_ALL=C sort
	)
	[ "$directory_patches" = "$manifest_patches" ] ||
		fail "dependency patch inventory differs from ordered series"
}

validate_patch_inventory
if [ "$VALIDATE_MANIFEST_ONLY" -eq 1 ]; then
	printf '%s\n' "validated dependency manifest: $MANIFEST"
	exit 0
fi

if [ "$VERIFY_ONLY" -eq 0 ]; then
	mkdir -p -- "$DEPENDENCY_ROOT" "$CACHE_ROOT"
else
	[ -d "$DEPENDENCY_ROOT" ] ||
		fail "dependency source root does not exist: $DEPENDENCY_ROOT"
fi
DEPENDENCY_ROOT=$(CDPATH= cd -- "$DEPENDENCY_ROOT" && pwd -P)
if [ -d "$CACHE_ROOT" ]; then
	CACHE_ROOT=$(CDPATH= cd -- "$CACHE_ROOT" && pwd -P)
fi

STAGING_ROOT=
cleanup()
{
	if [ -n "${STAGING_ROOT:-}" ] && [ -d "$STAGING_ROOT" ]; then
		rm -rf -- "$STAGING_ROOT"
	fi
}
trap cleanup EXIT HUP INT TERM

PUBLISHER="$ROOT_DIR/dependencies/publish-prefix.py"

# RENAME_EXCL: never replace a destination another process published. The
# publisher exits with status 1 when that destination already exists.
publish_exclusively()
{
	python3 "$PUBLISHER" "$1" "$2"
}

# RENAME_SWAP: exchange a verified replacement with the stale repository.
swap_atomically()
{
	python3 "$PUBLISHER" --swap "$1" "$2"
}

selected_product()
{
	[ "$PRODUCT" = all ] && return 0
	case ",$products," in
	*",$PRODUCT,"*) return 0 ;;
	esac
	return 1
}

git_value()
{
	git -C "$1" config --get "$2" 2>/dev/null || true
}

# Succeeds when the file system holding the existing path $1 ignores case: the
# path still resolves with its last component upper-cased. It writes nothing,
# so it also serves read-only verification.
path_ignores_case()
{
	probe_leaf=${1##*/}
	probe_upper=$(printf '%s' "$probe_leaf" | tr '[:lower:]' '[:upper:]')
	[ "$probe_upper" != "$probe_leaf" ] && [ -e "${1%/*}/$probe_upper" ]
}

# Prints a description of the paths in the tree of commit $2 of repository $1
# that differ only by case, and nothing when there are none. Such a tree has
# no faithful checkout on a case-insensitive file system.
tree_case_collisions()
{
	python3 -B - "$1" "$2" <<'PY_CASE_COLLISIONS'
import subprocess
import sys
import unicodedata

listing = subprocess.run(
    ["git", "-C", sys.argv[1], "ls-tree", "-r", "-z", "--name-only", sys.argv[2]],
    check=True,
    stdout=subprocess.PIPE,
).stdout
folded = {}
for raw in listing.split(b"\0"):
    if raw:
        name = raw.decode("utf-8", "surrogateescape")
        key = unicodedata.normalize("NFD", name).casefold()
        folded.setdefault(key, []).append(name)
clashes = sorted(names for names in folded.values() if len(names) > 1)
if clashes:
    print(
        f"{len(clashes)} groups of paths, for example "
        f"{clashes[0][0]} and {clashes[0][1]}"
    )
PY_CASE_COLLISIONS
}

# Refuses, before anything is checked out, a pinned tree that the staging root's
# file system cannot hold: two paths that differ only by case collapse into one
# file there, and the repository would read as modified.
refuse_case_collisions()
{
	path_ignores_case "$STAGING_ROOT" || return 0
	case_collisions=$(tree_case_collisions "$cache_repository" "$commit") ||
		fail "$name could not be examined for case-only path differences"
	[ -z "$case_collisions" ] ||
		fail "$name tracks paths that differ only by case ($case_collisions), but $DEPENDENCY_ROOT is on a case-insensitive file system and cannot hold them; prepare it with --root on a case-sensitive file system"
}

# Sets the SOURCE_* variables to the uml.source.* settings of repository $1,
# which one Git run reads; Git lists variable names in lower case.
read_source_settings()
{
	SOURCE_REPOSITORY=
	SOURCE_COMMIT=
	SOURCE_PATHS=
	SOURCE_PATCH_SERIES=
	source_settings=$(git -C "$1" config --get-regexp '^uml\.source\.' \
		2>/dev/null || true)
	while IFS= read -r source_setting; do
		source_key=${source_setting%% *}
		source_value=
		case $source_setting in
		*' '*) source_value=${source_setting#* } ;;
		esac
		case $source_key in
		uml.source.repository) SOURCE_REPOSITORY=$source_value ;;
		uml.source.commit) SOURCE_COMMIT=$source_value ;;
		uml.source.paths) SOURCE_PATHS=$source_value ;;
		uml.source.patchseries) SOURCE_PATCH_SERIES=$source_value ;;
		esac
	done <<EOF_SOURCE_SETTINGS
$source_settings
EOF_SOURCE_SETTINGS
}

# Set REPOSITORY_PROBLEM and fail the check without leaving the script, so
# preparation can decide whether --recreate may replace the repository.
check_repository()
{
	repository_directory=$1
	REPOSITORY_PROBLEM=
	[ -d "$repository_directory/.git" ] || {
		REPOSITORY_PROBLEM="$name is not a prepared Git repository"
		return 1
	}
	read_source_settings "$repository_directory"
	[ "$SOURCE_REPOSITORY" = "$repository" ] || {
		REPOSITORY_PROBLEM="$name prepared repository URL changed"
		return 1
	}
	[ "$SOURCE_COMMIT" = "$commit" ] || {
		REPOSITORY_PROBLEM="$name prepared base commit changed"
		return 1
	}
	[ "$SOURCE_PATHS" = "$source_paths" ] || {
		REPOSITORY_PROBLEM="$name prepared source paths changed"
		return 1
	}
	[ "$SOURCE_PATCH_SERIES" = "$patch_series" ] || {
		REPOSITORY_PROBLEM="$name prepared patch series changed"
		return 1
	}
	[ -z "$(git -C "$repository_directory" remote)" ] || {
		REPOSITORY_PROBLEM="$name prepared repository retains a network remote"
		return 1
	}
	# Without optional locks, status does not write a refreshed index, so
	# verification leaves the repository untouched.
	[ -z "$(git --no-optional-locks -C "$repository_directory" status \
		--porcelain --untracked-files=all)" ] || {
		REPOSITORY_PROBLEM="$name prepared repository is dirty"
		# On a case-insensitive file system a tree with case-only path
		# differences always reads as modified; say so.
		if path_ignores_case "$repository_directory/.git"; then
			case_collisions=$(tree_case_collisions "$repository_directory" \
				HEAD 2>/dev/null) || case_collisions=
			[ -z "$case_collisions" ] ||
				REPOSITORY_PROBLEM="$name tracks paths that differ only by case ($case_collisions), so it reads as modified on this case-insensitive file system; prepare it below a case-sensitive --root"
		fi
		return 1
	}
	actual_base=$(git -C "$repository_directory" rev-parse "$commit^{commit}" 2>/dev/null || true)
	[ "$actual_base" = "$commit" ] || {
		REPOSITORY_PROBLEM="$name base commit is unavailable"
		return 1
	}
	series_values
	if [ "$SERIES_COUNT" -eq 0 ]; then
		[ "$(git -C "$repository_directory" rev-parse HEAD)" = "$commit" ] || {
			REPOSITORY_PROBLEM="$name unpatched source is not at its pinned commit"
			return 1
		}
		return 0
	fi
	git -C "$repository_directory" merge-base --is-ancestor "$commit" HEAD || {
		REPOSITORY_PROBLEM="$name prepared history is not based on its pinned commit"
		return 1
	}
	actual_tree=$(git -C "$repository_directory" rev-parse 'HEAD^{tree}')
	[ "$actual_tree" = "$SERIES_TREE" ] || {
		REPOSITORY_PROBLEM="$name prepared tree is $actual_tree; its series produce $SERIES_TREE"
		return 1
	}
}

verify_repository()
{
	check_repository "$1" || fail "$REPOSITORY_PROBLEM"
}

# --recreate replaces only what preparation itself made: a clean repository of
# the same upstream whose branches point only at commits the Git cache holds.
recreatable_repository()
{
	[ ! -L "$1" ] && [ -d "$1/.git" ] || return 1
	[ "$(git_value "$1" uml.source.repository)" = "$repository" ] || return 1
	[ -z "$(git -C "$1" status --porcelain --untracked-files=all 2>/dev/null)" ] ||
		return 1
	for branch_tip in $(git -C "$1" for-each-ref --format='%(objectname)' refs/heads); do
		env GIT_NO_LAZY_FETCH=1 git -C "$CACHE_ROOT/git/$name" \
			cat-file -e "$branch_tip^{commit}" 2>/dev/null || return 1
	done
}

obtain_cache()
{
	cache_parent="$CACHE_ROOT/git"
	cache_repository="$cache_parent/$name"
	if [ ! -e "$cache_repository" ] && [ ! -L "$cache_repository" ]; then
		[ "$FETCH" -eq 1 ] || fail "$name Git cache is absent: $cache_repository"
		mkdir -p -- "$cache_parent"
		temporary_cache=$(mktemp -d "$cache_parent/.${name}.XXXXXX")
		git -C "$temporary_cache" init --quiet
		git -C "$temporary_cache" remote add origin "$repository"
		git -C "$temporary_cache" fetch --no-tags --depth 1 \
			origin "$commit" >/dev/null
		git -C "$temporary_cache" update-ref refs/heads/uml-pin "$commit"
		if ! publish_exclusively "$temporary_cache" "$cache_repository"; then
			rm -rf -- "$temporary_cache"
			[ -e "$cache_repository" ] || fail "$name Git cache could not be published"
		fi
	fi
	[ -d "$cache_repository/.git" ] || fail "$name Git cache is malformed"
	[ "$(git -C "$cache_repository" remote get-url origin 2>/dev/null || true)" = \
		"$repository" ] || fail "$name Git cache origin changed"
	if ! env GIT_NO_LAZY_FETCH=1 git -C "$cache_repository" cat-file -e \
		"$commit^{commit}" 2>/dev/null; then
		[ "$FETCH" -eq 1 ] || fail "$name commit is absent from the offline cache"
		git -C "$cache_repository" fetch --no-tags --depth 1 \
			origin "$commit" >/dev/null
		git -C "$cache_repository" update-ref refs/heads/uml-pin "$commit"
	fi
	[ "$(git -C "$cache_repository" rev-parse "$commit^{commit}")" = "$commit" ] ||
		fail "$name cache resolved the wrong commit"
	[ "$(git -C "$cache_repository" rev-parse refs/heads/uml-pin 2>/dev/null || true)" = \
		"$commit" ] || fail "$name Git cache does not retain its pinned ref"
	missing=$(env GIT_NO_LAZY_FETCH=1 git -C "$cache_repository" \
		rev-list --objects --missing=print "$commit" | sed -n 's/^?//p')
	[ -z "$missing" ] || fail "$name full source is incomplete in the Git cache"
}

prepare_repository()
{
	directory="$DEPENDENCY_ROOT/$name"
	replacing=0
	if [ -e "$directory" ] || [ -L "$directory" ]; then
		check_repository "$directory" && return
		[ "$RECREATE" -eq 1 ] ||
			fail "$REPOSITORY_PROBLEM; review it, then rerun with --recreate"
		recreatable_repository "$directory" ||
			fail "$REPOSITORY_PROBLEM; --recreate replaces only a clean prepared source whose branches the Git cache holds"
		replacing=1
	fi
	obtain_cache
	if [ -z "$STAGING_ROOT" ]; then
		STAGING_ROOT=$(mktemp -d "$DEPENDENCY_ROOT/.prepare.XXXXXX")
	fi
	refuse_case_collisions
	staged_repository="$STAGING_ROOT/$name"
	git clone --quiet --no-checkout \
		"file://$cache_repository" "$staged_repository"
	env GIT_NO_LAZY_FETCH=1 git -C "$staged_repository" \
		switch --detach "$commit" >/dev/null
	git -C "$staged_repository" remote remove origin
	# Branches a clone copied from the cache are not part of the source.
	git -C "$staged_repository" for-each-ref --format='delete %(refname)' \
		refs/heads | git -C "$staged_repository" update-ref --stdin
	if [ "$patch_series" != '-' ]; then
		series_values
		(
			GIT_NO_LAZY_FETCH=1
			export GIT_NO_LAZY_FETCH
			series_tool_layers apply --repo "$staged_repository" \
				--expect-tree "$SERIES_TREE" >/dev/null
		) || fail "$name patch series does not apply"
	fi
	git -C "$staged_repository" config uml.source.repository "$repository"
	git -C "$staged_repository" config uml.source.commit "$commit"
	git -C "$staged_repository" config uml.source.paths "$source_paths"
	git -C "$staged_repository" config uml.source.patchSeries "$patch_series"
	# The staged repository is verified once: a rename does not change it.
	verify_repository "$staged_repository"
	if [ "$replacing" -eq 1 ]; then
		swap_atomically "$staged_repository" "$directory" ||
			fail "$name stale source could not be replaced atomically"
		rm -rf -- "$staged_repository"
		printf '%s\n' "recreated $name in $directory"
		return
	fi
	if publish_exclusively "$staged_repository" "$directory"; then
		return
	fi
	# Another preparation published first; accept its repository only once
	# it verifies.
	[ -e "$directory" ] || [ -L "$directory" ] ||
		fail "$name source repository could not be published atomically"
	verify_repository "$directory"
}

matched=0
while IFS="$tab" read -r name version products repository commit source_paths \
	patch_series; do
	case "$name" in
	\#*|name|'') continue ;;
	esac
	selected_product || continue
	matched=1
	if [ "$VERIFY_ONLY" -eq 1 ]; then
		verify_repository "$DEPENDENCY_ROOT/$name"
	else
		prepare_repository
	fi
done < "$MANIFEST"

[ "$matched" -eq 1 ] || fail "manifest contains no sources for product $PRODUCT"
printf '%s\n' "verified $PRODUCT dependency repositories below $DEPENDENCY_ROOT"
