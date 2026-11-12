#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
REGENERATOR="$ROOT_DIR/dependencies/rebase-patches.sh"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-dependency-rebase.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

TEST_NAME=test-rebase-patches.sh
. "$(dirname -- "$0")/../support/expect.sh"

SOURCE="$TEST_ROOT/source"
METADATA="$TEST_ROOT/dependencies"
MANIFEST="$METADATA/manifest.tsv"
mkdir -p "$SOURCE" "$METADATA/patches/fixture"
git -C "$SOURCE" init --quiet
printf '%s\n' base >"$SOURCE/payload.txt"
git -C "$SOURCE" add payload.txt
git -C "$SOURCE" commit --quiet -m 'fixture: upstream base'
BASE=$(git -C "$SOURCE" rev-parse HEAD)
git -C "$SOURCE" switch --quiet -c maintained
printf '%s\n' maintained >"$SOURCE/payload.txt"
git -C "$SOURCE" commit --quiet -am 'fixture: maintained change'

cat >"$MANIFEST" <<EOF
name	version	products	repository	commit	source_paths	patch_series
fixture	1.0	fixture	https://example.invalid/fixture.git	$BASE	.	patches/fixture/series.tsv
EOF
UML_DEPENDENCY_MANIFEST="$MANIFEST" \
UML_DEPENDENCY_METADATA_ROOT="$METADATA" \
	"$REGENERATOR" --dependency fixture --source "$SOURCE" >/dev/null

SERIES="$METADATA/patches/fixture/series.tsv"
PATCH=$(awk -F '\t' '$1 == "patch" { print $2 }' "$SERIES")
[ "$PATCH" = fixture-maintained-change.patch ] ||
	fail "regenerator did not name the patch after its subject: $PATCH"
[ -f "$METADATA/patches/fixture/$PATCH" ] ||
	fail "regenerator did not emit the listed patch"
[ "$(sed -n 1p "$METADATA/patches/fixture/$PATCH")" = \
	'From 0000000000000000000000000000000000000000 Mon Sep 17 00:00:00 2001' ] ||
	fail "regenerator output carries a commit ID"
RECORDED_TREE=$(awk -F '\t' '$1 == "tree" { print $2 }' "$SERIES")
[ "$RECORDED_TREE" = "$(git -C "$SOURCE" rev-parse 'HEAD^{tree}')" ] ||
	fail "regenerator recorded the wrong final tree"

# Modification happens in Git history. Regeneration must follow the amended
# commit and must not retain manually edited bytes.
printf '%s\n' second >>"$SOURCE/payload.txt"
git -C "$SOURCE" commit --quiet --amend -am 'fixture: amended maintained change'
printf '%s\n' '# forbidden manual edit' >>"$METADATA/patches/fixture/$PATCH"
UML_DEPENDENCY_MANIFEST="$MANIFEST" \
UML_DEPENDENCY_METADATA_ROOT="$METADATA" \
	"$REGENERATOR" --dependency fixture --source "$SOURCE" >/dev/null
! grep -R -q 'forbidden manual edit' "$METADATA/patches/fixture" ||
	fail "regenerator retained manual patch-file editing"
grep -q 'fixture-amended-maintained-change.patch' "$SERIES" ||
	fail "regenerator did not follow amended Git history"
[ ! -e "$METADATA/patches/fixture/$PATCH" ] ||
	fail "regenerator kept the superseded patch"

# A detached topic regenerates like a branch.
git -C "$SOURCE" switch --quiet --detach
UML_DEPENDENCY_MANIFEST="$MANIFEST" \
UML_DEPENDENCY_METADATA_ROOT="$METADATA" \
	"$REGENERATOR" --dependency fixture --source "$SOURCE" >/dev/null
[ "$(awk -F '\t' '$1 == "tree" { print $2 }' "$SERIES")" = \
	"$(git -C "$SOURCE" rev-parse 'HEAD^{tree}')" ] ||
	fail "regenerator did not record the amended tree"

printf '%s\n' dirty >>"$SOURCE/payload.txt"
if UML_DEPENDENCY_MANIFEST="$MANIFEST" \
	UML_DEPENDENCY_METADATA_ROOT="$METADATA" \
	"$REGENERATOR" --dependency fixture --source "$SOURCE" \
	>"$TEST_ROOT/dirty.out" 2>"$TEST_ROOT/dirty.err"; then
	fail "regenerator accepted a dirty source repository"
fi
[ -s "$TEST_ROOT/dirty.err" ] || fail "dirty rejection lacked a diagnostic"

# A layered series exports only its owned, last layer: the topics after the
# one commit whose tree the paired guest's lower layer records.
LAYERED="$TEST_ROOT/layered"
LSOURCE="$LAYERED/source"
LOWER="$LAYERED/tools/rootfs/vulkan/patches/mesa"
OWNED="$LAYERED/dependencies/patches/mesa"
mkdir -p "$LSOURCE" "$LOWER" "$OWNED"
git -C "$LSOURCE" init --quiet
printf '%s\n' base >"$LSOURCE/guest.txt"
printf '%s\n' base >"$LSOURCE/host.txt"
git -C "$LSOURCE" add guest.txt host.txt
git -C "$LSOURCE" commit --quiet -m 'fixture: upstream base'
LBASE=$(git -C "$LSOURCE" rev-parse HEAD)
git -C "$LSOURCE" switch --quiet -c unified
printf '%s\n' guest >"$LSOURCE/guest.txt"
git -C "$LSOURCE" commit --quiet -am 'fixture: guest change'
python3 -B "$ROOT_DIR/scripts/patch_series.py" export --source "$LSOURCE" \
	--base "$LBASE" --head HEAD --series-dir "$LOWER" --tree >/dev/null
printf '%s\n' host >"$LSOURCE/host.txt"
git -C "$LSOURCE" commit --quiet -am 'fixture: host change'
printf '%s\n' second >>"$LSOURCE/host.txt"
git -C "$LSOURCE" commit --quiet -am 'fixture: second host change'
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
	name version products repository commit source_paths patch_series \
	mesa 1.0 fixture https://example.invalid/mesa.git "$LBASE" . \
	'rootfs-vulkan:patches/mesa/series.tsv+patches/mesa/series.tsv' \
	>"$LAYERED/dependencies/manifest.tsv"
UML_DEPENDENCY_MANIFEST="$LAYERED/dependencies/manifest.tsv" \
UML_DEPENDENCY_METADATA_ROOT="$LAYERED/dependencies" \
	"$REGENERATOR" --dependency mesa --source "$LSOURCE" >/dev/null
[ "$(awk -F '\t' '$1 == "patch" { print $2 }' "$OWNED/series.tsv" | tr '\n' ' ')" = \
	'fixture-host-change.patch fixture-second-host-change.patch ' ] ||
	fail "a layered export did not stop at the lower layer's tree"
[ "$(awk -F '\t' '$1 == "tree" { print $2 }' "$OWNED/series.tsv")" = \
	"$(git -C "$LSOURCE" rev-parse 'HEAD^{tree}')" ] ||
	fail "a layered export did not record the whole stack's tree"
[ "$(python3 -B "$ROOT_DIR/scripts/patch_series.py" tree --repo "$LSOURCE" \
	--base "$LBASE" --series-dir "$LOWER" --series-dir "$OWNED")" = \
	"$(git -C "$LSOURCE" rev-parse 'HEAD^{tree}')" ] ||
	fail "the layers do not reproduce the checkout"

# A lower layer the checkout no longer ends is refused rather than guessed.
git -C "$LSOURCE" switch --quiet --detach "$LBASE"
printf '%s\n' other >"$LSOURCE/guest.txt"
git -C "$LSOURCE" commit --quiet -am 'fixture: different guest change'
printf '%s\n' host >"$LSOURCE/host.txt"
git -C "$LSOURCE" commit --quiet -am 'fixture: host change'
if UML_DEPENDENCY_MANIFEST="$LAYERED/dependencies/manifest.tsv" \
	UML_DEPENDENCY_METADATA_ROOT="$LAYERED/dependencies" \
	"$REGENERATOR" --dependency mesa --source "$LSOURCE" \
	>"$TEST_ROOT/layer.out" 2>"$TEST_ROOT/layer.err"; then
	fail "a layered export accepted a checkout without the lower layer"
fi
grep -q 'end the lower layer' "$TEST_ROOT/layer.err" ||
	fail "the missing lower layer lacked a diagnostic"

printf '%s\n' "dependency patch regeneration tests passed"
