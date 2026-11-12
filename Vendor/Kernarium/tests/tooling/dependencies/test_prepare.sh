#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

DEPENDENCY_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../dependencies" && pwd)
PREPARE="$DEPENDENCY_DIR/prepare.sh"
PATCH_SERIES="$DEPENDENCY_DIR/../scripts/patch_series.py"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-dependency-prepare.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

TEST_NAME=test-prepare.sh
. "$(dirname -- "$0")/../support/expect.sh"

sha256_file()
{
	shasum -a 256 "$1" | awk '{print $1}'
}

prepare()
{
	prepared_root=$1
	shift
	UML_DEPENDENCY_MANIFEST="$MANIFEST" \
	UML_DEPENDENCY_PATCH_ROOT="$METADATA" \
		"$PREPARE" --product fixture --root "$prepared_root" --cache "$CACHE" \
		--no-fetch "$@"
}

export_series()
{
	python3 -B "$PATCH_SERIES" export --source "$UPSTREAM" --base "$BASE" \
		--head HEAD --series-dir "$SERIES_DIR" --tree >/dev/null
}

verify()
{
	UML_DEPENDENCY_MANIFEST="$MANIFEST" \
	UML_DEPENDENCY_PATCH_ROOT="$METADATA" \
		"$PREPARE" --product fixture --root "$1" --cache "$CACHE" --verify
}

# Hash-pinned archive acquisition publishes only authenticated bytes and
# never installs packages. The curl fixture keeps this test fully offline.
mkdir -p "$TEST_ROOT/archive-tools"
cat >"$TEST_ROOT/archive-tools/curl" <<'EOF_ARCHIVE_CURL'
#!/bin/sh
set -eu
while [ "$#" -gt 0 ]; do
	case "$1" in
	--output) destination=$2; shift 2 ;;
	*) shift ;;
	esac
done
cp "$UML_FIXTURE_ARCHIVE" "$destination"
EOF_ARCHIVE_CURL
chmod +x "$TEST_ROOT/archive-tools/curl"
printf 'authenticated archive fixture\n' >"$TEST_ROOT/archive-payload"
ARCHIVE_SHA=$(sha256_file "$TEST_ROOT/archive-payload")
ARCHIVE_MANIFEST="$TEST_ROOT/archives.tsv"
printf 'name\turl\tsha256\nfixture.deb\thttps://fixture.invalid/fixture.deb\t%s\n' \
	"$ARCHIVE_SHA" >"$ARCHIVE_MANIFEST"
env PATH="$TEST_ROOT/archive-tools:$PATH" \
	UML_FIXTURE_ARCHIVE="$TEST_ROOT/archive-payload" \
	"$PREPARE" --archive-manifest "$ARCHIVE_MANIFEST" \
		--cache "$TEST_ROOT/archive-cache" >/dev/null
"$PREPARE" --archive-manifest "$ARCHIVE_MANIFEST" \
	--cache "$TEST_ROOT/archive-cache" --no-fetch >/dev/null
cmp "$TEST_ROOT/archive-payload" \
	"$TEST_ROOT/archive-cache/archives/$ARCHIVE_SHA-fixture.deb" >/dev/null ||
	fail "archive publication changed authenticated bytes"
printf corrupt >"$TEST_ROOT/archive-cache/archives/$ARCHIVE_SHA-fixture.deb"
expect_failure 'corrupt cached archive' "$PREPARE" \
	--archive-manifest "$ARCHIVE_MANIFEST" --cache "$TEST_ROOT/archive-cache" --verify
expect_failure 'absent offline archive' "$PREPARE" \
	--archive-manifest "$ARCHIVE_MANIFEST" --cache "$TEST_ROOT/absent-archive-cache" --no-fetch
expect_failure 'absent verification cache' "$PREPARE" \
	--archive-manifest "$ARCHIVE_MANIFEST" --cache "$TEST_ROOT/absent-verify-cache" --verify
[ ! -e "$TEST_ROOT/absent-verify-cache" ] || fail "archive verification created a cache"
sed 's/fixture.deb/name/g' "$ARCHIVE_MANIFEST" >"$TEST_ROOT/reserved-archives.tsv"
expect_failure 'reserved archive name' "$PREPARE" \
	--archive-manifest "$TEST_ROOT/reserved-archives.tsv" --validate-manifest-only
sed 's/fixture.deb/..\/fixture.deb/g' "$ARCHIVE_MANIFEST" >"$TEST_ROOT/unsafe-archives.tsv"
expect_failure 'unsafe archive name' "$PREPARE" \
	--archive-manifest "$TEST_ROOT/unsafe-archives.tsv" --validate-manifest-only
sed "s/$ARCHIVE_SHA/0000000000000000000000000000000000000000000000000000000000000000/" \
	"$ARCHIVE_MANIFEST" >"$TEST_ROOT/wrong-hash-archives.tsv"
expect_failure 'archive download hash mismatch' \
	env PATH="$TEST_ROOT/archive-tools:$PATH" \
		UML_FIXTURE_ARCHIVE="$TEST_ROOT/archive-payload" "$PREPARE" \
		--archive-manifest "$TEST_ROOT/wrong-hash-archives.tsv" \
		--cache "$TEST_ROOT/rejected-archive-cache"
[ -z "$(find "$TEST_ROOT/rejected-archive-cache/archives" -type f -print)" ] ||
	fail "rejected archive left a published or temporary payload"

CACHE="$TEST_ROOT/cache"
METADATA="$TEST_ROOT/metadata"
UPSTREAM="$TEST_ROOT/upstream"
PREPARED="$TEST_ROOT/prepared"
MANIFEST="$METADATA/manifest.tsv"
SERIES_DIR="$METADATA/patches/patched"
FAILURE_NUMBER=0
mkdir -p "$CACHE/git" "$METADATA/patches" "$UPSTREAM"

git -C "$UPSTREAM" init --quiet
printf '%s\n' baseline >"$UPSTREAM/payload.txt"
mkdir -p "$UPSTREAM/sub"
printf '#!/bin/sh\nexit 0\n' >"$UPSTREAM/sub/tool.sh"
chmod +x "$UPSTREAM/sub/tool.sh"
git -C "$UPSTREAM" add payload.txt sub/tool.sh
git -C "$UPSTREAM" commit --quiet -m 'fixture: upstream base'
BASE=$(git -C "$UPSTREAM" rev-parse HEAD)

# Ref discovery stays in the network-owning workflow, but immutable source
# preparation still accepts only a pinned commit. Redirect this test's HTTPS
# endpoint to the local fixture without changing user Git configuration.
git -C "$UPSTREAM" tag fixture-lightweight "$BASE"
git -C "$UPSTREAM" tag -a fixture-annotated "$BASE" -m 'fixture tag'
resolve_ref()
{
	env GIT_CONFIG_COUNT=1 \
		GIT_CONFIG_KEY_0="url.file://$UPSTREAM.insteadOf" \
		GIT_CONFIG_VALUE_0=https://fixture.invalid/upstream.git \
		"$PREPARE" --resolve-ref https://fixture.invalid/upstream.git "$@"
}
for ref in fixture-lightweight fixture-annotated; do
	[ "$(resolve_ref "refs/tags/$ref")" = "$BASE" ] ||
		fail "dependency ref did not resolve to its peeled commit"
done
expect_failure 'missing dependency ref' resolve_ref refs/tags/absent
expect_failure 'wildcard dependency ref' resolve_ref 'refs/tags/fixture-*'
expect_failure 'unqualified dependency ref' resolve_ref fixture-lightweight
expect_failure 'offline ref resolution' resolve_ref refs/tags/fixture-lightweight --no-fetch
expect_failure 'non-HTTPS ref resolution' \
	"$PREPARE" --resolve-ref "$UPSTREAM" refs/tags/fixture-lightweight

TAG_DESTINATION="$TEST_ROOT/tag-destination"
git clone --quiet --no-tags "$UPSTREAM" "$TAG_DESTINATION"
TAG_OBJECT=$(git -C "$UPSTREAM" rev-parse refs/tags/fixture-annotated)
materialize_tag()
{
	env GIT_CONFIG_COUNT=1 \
		GIT_CONFIG_KEY_0="url.file://$UPSTREAM.insteadOf" \
		GIT_CONFIG_VALUE_0=https://fixture.invalid/upstream.git \
		"$PREPARE" --materialize-tag https://fixture.invalid/upstream.git "$@"
}
expect_failure 'wrong pinned tag object' materialize_tag \
	refs/tags/fixture-annotated "$BASE" "$BASE" "$TAG_DESTINATION"
expect_failure 'wrong peeled tag commit' materialize_tag \
	refs/tags/fixture-annotated "$TAG_OBJECT" "$TAG_OBJECT" "$TAG_DESTINATION"
if git -C "$TAG_DESTINATION" show-ref --verify refs/tags/fixture-annotated >/dev/null 2>&1; then
	fail "failed tag verification published a tag ref"
fi
materialize_tag refs/tags/fixture-annotated "$TAG_OBJECT" "$BASE" "$TAG_DESTINATION"
[ "$(git -C "$TAG_DESTINATION" rev-parse refs/tags/fixture-annotated)" = "$TAG_OBJECT" ] &&
	[ "$(git -C "$TAG_DESTINATION" rev-parse HEAD)" = "$BASE" ] &&
	[ -z "$(git -C "$TAG_DESTINATION" status --porcelain)" ] ||
	fail "tag materialization changed the prepared checkout"
expect_failure 'lightweight tag materialization' materialize_tag \
	refs/tags/fixture-lightweight "$BASE" "$BASE" "$TAG_DESTINATION"
expect_failure 'branch materialization' materialize_tag \
	refs/heads/master "$BASE" "$BASE" "$TAG_DESTINATION"
expect_failure 'offline tag materialization' materialize_tag \
	refs/tags/fixture-annotated "$TAG_OBJECT" "$BASE" "$TAG_DESTINATION" --no-fetch
git -C "$TAG_DESTINATION" update-ref refs/tags/fixture-annotated "$BASE"
expect_failure 'existing mismatched tag' materialize_tag \
	refs/tags/fixture-annotated "$TAG_OBJECT" "$BASE" "$TAG_DESTINATION"

printf '%s\n' patched >"$UPSTREAM/payload.txt"
git -C "$UPSTREAM" commit --quiet -am 'fixture: first maintained change'
printf '%s\n' added >"$UPSTREAM/added.txt"
printf '\000\001\002\377' >"$UPSTREAM/added.bin"
git -C "$UPSTREAM" add added.txt added.bin
git -C "$UPSTREAM" commit --quiet -m 'fixture: second maintained change'
mkdir -p "$SERIES_DIR"
export_series

for name in clean patched; do
	git clone --quiet "$UPSTREAM" "$CACHE/git/$name"
	git -C "$CACHE/git/$name" remote set-url origin \
		"https://example.invalid/$name.git"
	git -C "$CACHE/git/$name" update-ref refs/heads/uml-pin "$BASE"
done

# A cache that an earlier checkout filtered holds commits and trees but no
# blobs. It cannot serve a full source.
git -C "$UPSTREAM" config uploadpack.allowFilter true
git clone --quiet --no-checkout --filter=blob:none "file://$UPSTREAM" \
	"$CACHE/git/filtered"
git -C "$CACHE/git/filtered" remote set-url origin \
	https://example.invalid/filtered.git
git -C "$CACHE/git/filtered" update-ref refs/heads/uml-pin "$BASE"

cat >"$MANIFEST" <<EOF
name	version	products	repository	commit	source_paths	patch_series
clean	1.0	fixture	https://example.invalid/clean.git	$BASE	.	-
patched	1.0	fixture	https://example.invalid/patched.git	$BASE	.	patches/patched/series.tsv
filtered	1.0	filtered-fixture	https://example.invalid/filtered.git	$BASE	.	-
EOF
cp "$MANIFEST" "$MANIFEST.original"
cp -R "$SERIES_DIR" "$TEST_ROOT/series-original"

cp "$(find "$SERIES_DIR" -name '*.patch' -print -quit)" \
	"$METADATA/patches/unlisted.patch"
expect_failure "unlisted dependency patch" \
	env UML_DEPENDENCY_MANIFEST="$MANIFEST" \
		UML_DEPENDENCY_PATCH_ROOT="$METADATA" \
		"$PREPARE" --validate-manifest-only
rm "$METADATA/patches/unlisted.patch"

# An exported CDPATH neither redirects a relative metadata root to a decoy
# with another inventory nor prints into the inventory the preparer compares.
CDPATH_DECOY="$TEST_ROOT/cdpath-decoy"
mkdir -p "$CDPATH_DECOY/metadata/patches/decoy"
printf 'decoy\n' >"$CDPATH_DECOY/metadata/patches/decoy/series.tsv"
CDPATH_OUTPUT=$(
	CDPATH= cd -- "$TEST_ROOT" &&
		CDPATH="$CDPATH_DECOY" UML_DEPENDENCY_MANIFEST="$MANIFEST" \
		UML_DEPENDENCY_PATCH_ROOT=metadata \
		"$PREPARE" --validate-manifest-only
) || fail "an exported CDPATH redirected a relative metadata root"
[ "$CDPATH_OUTPUT" = "validated dependency manifest: $MANIFEST" ] ||
	fail "an exported CDPATH changed the preparer's output: $CDPATH_OUTPUT"

prepare "$PREPARED" >/dev/null
verify "$PREPARED" >/dev/null
# Verification does not refresh a prepared index whose stat data is stale.
touch "$PREPARED/clean/payload.txt"
INDEX_TIME=$(stat -f '%Fm' "$PREPARED/clean/.git/index")
verify "$PREPARED" >/dev/null
[ "$(stat -f '%Fm' "$PREPARED/clean/.git/index")" = "$INDEX_TIME" ] ||
	fail "source verification rewrote a prepared repository's index"
[ "$(cat "$PREPARED/clean/payload.txt")" = baseline ] ||
	fail "unpatched dependency did not use the pinned commit"
[ "$(cat "$PREPARED/patched/payload.txt")" = patched ] ||
	fail "dependency patch series was not replayed"
[ "$(cat "$PREPARED/patched/added.txt")" = added ] ||
	fail "ordered dependency patch series was incomplete"
cmp "$UPSTREAM/added.bin" "$PREPARED/patched/added.bin" >/dev/null ||
	fail "binary dependency patch was not replayed exactly"
[ -x "$PREPARED/clean/sub/tool.sh" ] ||
	fail "prepared Git checkout lost executable status"
[ -z "$(git -C "$PREPARED/patched" remote)" ] ||
	fail "prepared dependency retained a network remote"
[ "$(git -C "$PREPARED/patched" rev-list --count "$BASE..HEAD")" -eq 2 ] ||
	fail "prepared dependency history has the wrong topology"

# Every source is complete: a manifest row that selects source paths is
# refused before anything is fetched or published, however it spells them.
for paths in sub 'sub,other' ./ ..; do
	awk -F '\t' -v paths="$paths" 'BEGIN { OFS = FS }
		$1 == "clean" { $6 = paths } { print }' \
		"$MANIFEST.original" >"$MANIFEST"
	expect_diagnostic "source_paths must be '.'" \
		prepare "$TEST_ROOT/partial-sources"
	[ ! -e "$TEST_ROOT/partial-sources" ] ||
		fail "a refused source selection created a source root"
done
cp "$MANIFEST.original" "$MANIFEST"

# A repository prepared for another source selection is stale, not accepted.
git -C "$PREPARED/clean" config uml.source.paths sub
expect_diagnostic "prepared source paths changed" verify "$PREPARED"
git -C "$PREPARED/clean" config uml.source.paths .
verify "$PREPARED" >/dev/null

# Preparation refuses a filtered cache whether or not it may fetch, and
# repairs nothing: GIT_ALLOW_PROTOCOL keeps a regression off the network.
for fetching in --no-fetch ''; do
	expect_diagnostic "full source is incomplete in the Git cache" \
		env GIT_ALLOW_PROTOCOL=file UML_DEPENDENCY_MANIFEST="$MANIFEST" \
			UML_DEPENDENCY_PATCH_ROOT="$METADATA" \
			"$PREPARE" --product filtered-fixture \
			--root "$TEST_ROOT/filtered-prepared" --cache "$CACHE" $fetching
done
[ ! -e "$TEST_ROOT/filtered-prepared/filtered" ] ||
	fail "a refused filtered cache published a source"
[ "$(git -C "$CACHE/git/filtered" config --bool remote.origin.promisor)" = true ] ||
	fail "preparation repaired a filtered cache"

CONCURRENT="$TEST_ROOT/concurrent"
prepare "$CONCURRENT" >"$TEST_ROOT/concurrent-1.out" 2>&1 &
FIRST_PREPARER=$!
prepare "$CONCURRENT" >"$TEST_ROOT/concurrent-2.out" 2>&1 &
SECOND_PREPARER=$!
if ! wait "$FIRST_PREPARER"; then
	cat "$TEST_ROOT/concurrent-1.out" >&2
	fail "first concurrent preparer failed"
fi
if ! wait "$SECOND_PREPARER"; then
	cat "$TEST_ROOT/concurrent-2.out" >&2
	fail "second concurrent preparer failed"
fi
verify "$CONCURRENT" >/dev/null

printf '%s\n' tampered >>"$PREPARED/clean/payload.txt"
expect_failure "prepared-repository tampering" verify "$PREPARED"
git -C "$PREPARED/clean" restore payload.txt
verify "$PREPARED" >/dev/null

# A hand edit that changes what the series produces no longer matches the
# recorded tree.
for PATCH in "$SERIES_DIR"/*.patch; do
	sed 's/^+added$/+edited/' "$PATCH" >"$PATCH.edited"
	mv "$PATCH.edited" "$PATCH"
done
expect_failure "manual patch-file editing" prepare "$TEST_ROOT/patch-tamper"
[ ! -e "$TEST_ROOT/patch-tamper/patched" ] ||
	fail "a series that misses its recorded tree was published"
rm -rf "$SERIES_DIR"
cp -R "$TEST_ROOT/series-original" "$SERIES_DIR"

tail -n 1 "$MANIFEST.original" >>"$MANIFEST"
expect_failure "duplicate dependency identity" verify "$PREPARED"
cp "$MANIFEST.original" "$MANIFEST"

mkdir "$TEST_ROOT/existing-mismatch"
mkdir "$TEST_ROOT/existing-mismatch/clean"
printf '%s\n' retained >"$TEST_ROOT/existing-mismatch/clean/payload.txt"
expect_failure "replacement of an existing non-Git source" \
	prepare "$TEST_ROOT/existing-mismatch"
[ "$(cat "$TEST_ROOT/existing-mismatch/clean/payload.txt")" = retained ] ||
	fail "failed preparation replaced the mismatched source"

git -C "$CACHE/git/clean" remote set-url origin \
	https://example.invalid/wrong.git
expect_failure "cache origin tampering" prepare "$TEST_ROOT/cache-tamper"
git -C "$CACHE/git/clean" remote set-url origin \
	https://example.invalid/clean.git

mv "$CACHE/git/clean" "$CACHE/git/clean.absent"
expect_failure "missing offline Git cache" prepare "$TEST_ROOT/missing-cache"
mv "$CACHE/git/clean.absent" "$CACHE/git/clean"

# A pinned tree with two paths that differ only by case has no faithful
# checkout on a case-insensitive file system, where Git writes one over the
# other and the repository reads as modified. Preparation names that cause and
# refuses before any checkout, but only for such a tree: the fixtures above
# prepare on the same root. A case-sensitive root holds the tree, so a host
# that offers one runs the complete case; the other kind of host can only
# exercise the refusal, as the kernel source-location test does.
CASE_UPSTREAM="$TEST_ROOT/case-upstream"
CASE_METADATA="$TEST_ROOT/case-metadata"
CASE_MANIFEST="$CASE_METADATA/manifest.tsv"
git init --quiet -b main "$CASE_UPSTREAM"
CASE_UPPER=$(printf 'upper\n' | git -C "$CASE_UPSTREAM" hash-object -w --stdin)
CASE_LOWER=$(printf 'lower\n' | git -C "$CASE_UPSTREAM" hash-object -w --stdin)
# The tree is written without an index or a work tree, which would fold case.
CASE_DOC=$(printf '100644 blob %s\tTest.S\n100644 blob %s\ttest.s\n' \
	"$CASE_UPPER" "$CASE_LOWER" | git -C "$CASE_UPSTREAM" mktree)
CASE_ROOT_TREE=$(printf '040000 tree %s\tDoc\n' "$CASE_DOC" |
	git -C "$CASE_UPSTREAM" mktree)
CASE_COMMIT=$(git -C "$CASE_UPSTREAM" commit-tree -m 'fixture: case-only paths' \
	"$CASE_ROOT_TREE")
git -C "$CASE_UPSTREAM" update-ref refs/heads/main "$CASE_COMMIT"
git clone --quiet --no-checkout "$CASE_UPSTREAM" "$CACHE/git/collide"
git -C "$CACHE/git/collide" remote set-url origin https://example.invalid/collide.git
git -C "$CACHE/git/collide" update-ref refs/heads/uml-pin "$CASE_COMMIT"
mkdir -p "$CASE_METADATA/patches"
printf 'name\tversion\tproducts\trepository\tcommit\tsource_paths\tpatch_series\n' \
	>"$CASE_MANIFEST"
printf 'collide\t1.0\tcase-fixture\thttps://example.invalid/collide.git\t%s\t.\t-\n' \
	"$CASE_COMMIT" >>"$CASE_MANIFEST"
prepare_case()
{
	prepared_root=$1
	shift
	UML_DEPENDENCY_MANIFEST="$CASE_MANIFEST" \
	UML_DEPENDENCY_PATCH_ROOT="$CASE_METADATA" \
		"$PREPARE" --product case-fixture --root "$prepared_root" \
		--cache "$CACHE" --no-fetch "$@"
}
mkdir "$TEST_ROOT/case-probe"
if [ -e "$TEST_ROOT/CASE-PROBE" ]; then
	expect_diagnostic "differ only by case" prepare_case "$TEST_ROOT/case-refused"
	for phrase in 'case-insensitive file system' 'case-sensitive file system'; do
		grep -F -- "$phrase" "$TEST_ROOT/rejected-$FAILURE_NUMBER.err" >/dev/null ||
			fail "the case refusal does not say: $phrase"
	done
	[ ! -e "$TEST_ROOT/case-refused/collide" ] &&
		[ -z "$(find "$TEST_ROOT/case-refused" -mindepth 1 -maxdepth 1 -name '.prepare.*' -print)" ] ||
		fail "a refused case-only tree left a checkout or a staging root"
	# A checkout made some other way reads as modified; verification says why.
	mkdir "$TEST_ROOT/case-modified"
	git clone --quiet --no-checkout "$CASE_UPSTREAM" "$TEST_ROOT/case-modified/collide"
	git -C "$TEST_ROOT/case-modified/collide" remote remove origin
	git -C "$TEST_ROOT/case-modified/collide" switch --detach --quiet "$CASE_COMMIT"
	git -C "$TEST_ROOT/case-modified/collide" config uml.source.repository \
		https://example.invalid/collide.git
	git -C "$TEST_ROOT/case-modified/collide" config uml.source.commit "$CASE_COMMIT"
	git -C "$TEST_ROOT/case-modified/collide" config uml.source.paths .
	git -C "$TEST_ROOT/case-modified/collide" config uml.source.patchSeries -
	expect_diagnostic "reads as modified on this case-insensitive file system" \
		prepare_case "$TEST_ROOT/case-modified" --verify
else
	prepare_case "$TEST_ROOT/case-held" >/dev/null
	prepare_case "$TEST_ROOT/case-held" --verify >/dev/null
	[ "$(cat "$TEST_ROOT/case-held/collide/Doc/Test.S")" = upper ] &&
		[ "$(cat "$TEST_ROOT/case-held/collide/Doc/test.s")" = lower ] ||
		fail "a case-sensitive root did not hold both case-only paths"
fi

# Foreign-owned protocol metadata is authenticated in place, never copied
# into the dependency patch inventory or resolved through arbitrary '..'.
ROOTFS_METADATA="$TEST_ROOT/tools/rootfs/vulkan"
ROOTFS_SERIES="$ROOTFS_METADATA/patches/venus-protocol"
mkdir -p "$ROOTFS_METADATA/patches"
cp -R "$TEST_ROOT/series-original" "$ROOTFS_SERIES"
for name in venus-protocol vulkan-headers; do
	git clone --quiet "$UPSTREAM" "$CACHE/git/$name"
	git -C "$CACHE/git/$name" remote set-url origin \
		"https://example.invalid/$name.git"
	git -C "$CACHE/git/$name" update-ref refs/heads/uml-pin "$BASE"
done
cat >>"$MANIFEST" <<EOF
vulkan-headers	1.0	apple-gpu-vulkan-experiment	https://example.invalid/vulkan-headers.git	$BASE	.	-
venus-protocol	1.0	apple-gpu-vulkan-experiment	https://example.invalid/venus-protocol.git	$BASE	.	rootfs-vulkan:patches/venus-protocol/series.tsv
EOF
cat >"$ROOTFS_METADATA/sources.tsv" <<EOF
component	repository	base_commit	base_tree	release_ref	release_object	series
vulkan-headers	https://example.invalid/vulkan-headers.git	$BASE	-	-	-	-
venus-protocol	https://example.invalid/venus-protocol.git	$BASE	-	-	-	patches/venus-protocol/series.tsv
EOF
cp "$MANIFEST" "$TEST_ROOT/paired-manifest.tsv"
cp "$ROOTFS_METADATA/sources.tsv" "$TEST_ROOT/paired-sources.tsv"
paired_prepare()
{
	UML_DEPENDENCY_MANIFEST="$MANIFEST" UML_DEPENDENCY_PATCH_ROOT="$METADATA" \
		"$PREPARE" --product apple-gpu-vulkan-experiment \
		--root "$TEST_ROOT/paired-prepared" --cache "$CACHE" --no-fetch
}
paired_prepare >/dev/null
[ "$(cat "$TEST_ROOT/paired-prepared/venus-protocol/payload.txt")" = patched ] ||
	fail "foreign-owned protocol topics were not replayed"
[ ! -e "$METADATA/patches/venus-protocol" ] ||
	fail "preparation duplicated foreign-owned protocol metadata"
for invalid in \
	'unknown:patches/venus-protocol/series.tsv' \
	'rootfs-vulkan:../patches/venus-protocol/series.tsv' \
	'rootfs-vulkan:/patches/venus-protocol/series.tsv' \
	'rootfs-vulkan:patches/%2e%2e/venus-protocol/series.tsv'; do
	awk -F '\t' -v replacement="$invalid" 'BEGIN { OFS = FS }
		$1 == "venus-protocol" { $7 = replacement } { print }' \
		"$TEST_ROOT/paired-manifest.tsv" >"$MANIFEST"
	expect_failure "unsafe foreign series $invalid" paired_prepare
done
cp "$TEST_ROOT/paired-manifest.tsv" "$MANIFEST"
mv "$ROOTFS_SERIES" "$TEST_ROOT/foreign-series"
ln -s "$TEST_ROOT/foreign-series" "$ROOTFS_SERIES"
expect_failure "foreign series symlink escape" paired_prepare
rm "$ROOTFS_SERIES"
mv "$TEST_ROOT/foreign-series" "$ROOTFS_SERIES"
for component in venus-protocol vulkan-headers; do
	awk -F '\t' -v component="$component" 'BEGIN { OFS = FS }
		$1 == component { $3 = "0000000000000000000000000000000000000000" }
		{ print }' "$TEST_ROOT/paired-sources.tsv" >"$ROOTFS_METADATA/sources.tsv"
	expect_failure "mismatched paired $component source" paired_prepare
done
cp "$TEST_ROOT/paired-sources.tsv" "$ROOTFS_METADATA/sources.tsv"
awk -F '\t' '$1 != "vulkan-headers"' "$TEST_ROOT/paired-manifest.tsv" >"$MANIFEST"
expect_failure "missing paired SDK declaration" paired_prepare
cp "$TEST_ROOT/paired-manifest.tsv" "$MANIFEST"
paired_prepare >/dev/null

# A layered Mesa series applies the paired guest's topics and then the host's
# own, whose owned last layer records the tree of the whole stack.
ROOTFS_MESA="$ROOTFS_METADATA/patches/mesa"
HOST_MESA="$METADATA/patches/mesa"
MESA_WORK="$TEST_ROOT/mesa-work"
cp -R "$TEST_ROOT/series-original" "$ROOTFS_MESA"
git clone --quiet "$UPSTREAM" "$MESA_WORK"
GUEST_HEAD=$(git -C "$MESA_WORK" rev-parse HEAD)
[ "$(python3 -B "$PATCH_SERIES" recorded-tree --series-dir "$ROOTFS_MESA")" = \
	"$(git -C "$MESA_WORK" rev-parse 'HEAD^{tree}')" ] ||
	fail "the guest layer fixture does not end at the work checkout"
printf '%s\n' host >"$MESA_WORK/host.txt"
git -C "$MESA_WORK" add host.txt
git -C "$MESA_WORK" commit --quiet -m 'fixture: host change'
mkdir -p "$HOST_MESA"
python3 -B "$PATCH_SERIES" export --source "$MESA_WORK" --base "$GUEST_HEAD" \
	--head HEAD --series-dir "$HOST_MESA" --tree >/dev/null
git clone --quiet "$UPSTREAM" "$CACHE/git/mesa"
git -C "$CACHE/git/mesa" remote set-url origin https://example.invalid/mesa.git
git -C "$CACHE/git/mesa" update-ref refs/heads/uml-pin "$BASE"
LAYERED_FIELD='rootfs-vulkan:patches/mesa/series.tsv+patches/mesa/series.tsv'
printf 'mesa\t1.0\tapple-gpu-vulkan-experiment\thttps://example.invalid/mesa.git\t%s\t.\t%s\n' \
	"$BASE" "$LAYERED_FIELD" >>"$MANIFEST"
printf 'mesa\thttps://example.invalid/mesa.git\t%s\t-\t-\t-\tpatches/mesa/series.tsv\n' \
	"$BASE" >>"$ROOTFS_METADATA/sources.tsv"
cp "$MANIFEST" "$TEST_ROOT/layered-manifest.tsv"
cp "$ROOTFS_METADATA/sources.tsv" "$TEST_ROOT/layered-sources.tsv"
layered_prepare()
{
	UML_DEPENDENCY_MANIFEST="$MANIFEST" UML_DEPENDENCY_PATCH_ROOT="$METADATA" \
		"$PREPARE" --product apple-gpu-vulkan-experiment --root "$1" \
		--cache "$CACHE" --no-fetch
}
LAYERED_PREPARED="$TEST_ROOT/layered-prepared"
layered_prepare "$LAYERED_PREPARED" >/dev/null
UML_DEPENDENCY_MANIFEST="$MANIFEST" UML_DEPENDENCY_PATCH_ROOT="$METADATA" \
	"$PREPARE" --product apple-gpu-vulkan-experiment --root "$LAYERED_PREPARED" \
	--cache "$CACHE" --verify >/dev/null
[ "$(cat "$LAYERED_PREPARED/mesa/payload.txt")" = patched ] &&
	[ "$(cat "$LAYERED_PREPARED/mesa/host.txt")" = host ] ||
	fail "a layered series did not replay both layers"
[ "$(git -C "$LAYERED_PREPARED/mesa" rev-parse 'HEAD^{tree}')" = \
	"$(git -C "$MESA_WORK" rev-parse 'HEAD^{tree}')" ] ||
	fail "a layered series did not reproduce the whole stack"
[ "$(git -C "$LAYERED_PREPARED/mesa" rev-list --count "$BASE..HEAD")" -eq 3 ] ||
	fail "a layered series has the wrong topology"
[ ! -e "$METADATA/patches/mesa/fixture-first-maintained-change.patch" ] ||
	fail "preparation duplicated the guest layer"

for invalid in \
	'patches/mesa/series.tsv' \
	'rootfs-vulkan:patches/mesa/series.tsv' \
	'patches/mesa/series.tsv+rootfs-vulkan:patches/mesa/series.tsv' \
	'rootfs-vulkan:patches/mesa/series.tsv++patches/mesa/series.tsv' \
	'rootfs-vulkan:patches/mesa/series.tsv+patches/mesa/series.tsv+' \
	'rootfs-vulkan:patches/venus-protocol/series.tsv+patches/mesa/series.tsv'; do
	awk -F '\t' -v replacement="$invalid" 'BEGIN { OFS = FS }
		$1 == "mesa" { $7 = replacement } { print }' \
		"$TEST_ROOT/layered-manifest.tsv" >"$MANIFEST"
	expect_failure "layered series field $invalid" \
		layered_prepare "$TEST_ROOT/layered-invalid"
done
cp "$TEST_ROOT/layered-manifest.tsv" "$MANIFEST"
awk -F '\t' 'BEGIN { OFS = FS } $1 == "mesa" { $3 = "0000000000000000000000000000000000000000" }
	{ print }' "$TEST_ROOT/layered-sources.tsv" >"$ROOTFS_METADATA/sources.tsv"
expect_failure "mismatched paired mesa source" \
	layered_prepare "$TEST_ROOT/layered-mismatch"
cp "$TEST_ROOT/layered-sources.tsv" "$ROOTFS_METADATA/sources.tsv"

# The host layer's recorded tree names the guest layer it was exported on, so
# a changed guest layer invalidates it instead of producing a new stack.
mv "$ROOTFS_MESA" "$TEST_ROOT/guest-layer"
mkdir -p "$ROOTFS_MESA"
python3 -B "$PATCH_SERIES" export --source "$UPSTREAM" --base "$BASE" \
	--head HEAD~1 --series-dir "$ROOTFS_MESA" --tree >/dev/null
expect_failure "a host layer on a changed guest layer" \
	layered_prepare "$TEST_ROOT/layered-stale"
[ ! -e "$TEST_ROOT/layered-stale/mesa" ] ||
	fail "a layered series that misses its recorded tree was published"
rm -rf "$ROOTFS_MESA"
mv "$TEST_ROOT/guest-layer" "$ROOTFS_MESA"
layered_prepare "$LAYERED_PREPARED" >/dev/null
rm -rf "$HOST_MESA"
cp "$TEST_ROOT/paired-manifest.tsv" "$MANIFEST"
cp "$TEST_ROOT/paired-sources.tsv" "$ROOTFS_METADATA/sources.tsv"

# A changed series leaves an existing prepared source stale. Only --recreate
# replaces it, and only when preparation could have made it.
cp "$MANIFEST.original" "$MANIFEST"
RECREATED="$TEST_ROOT/recreated"
prepare "$RECREATED" >/dev/null
printf '%s\n' third >"$UPSTREAM/third.txt"
git -C "$UPSTREAM" add third.txt
git -C "$UPSTREAM" commit --quiet -m 'fixture: third maintained change'
export_series
expect_failure "stale prepared source" verify "$RECREATED"
expect_failure "replacement without --recreate" prepare "$RECREATED"
grep -q -- '--recreate' "$TEST_ROOT/rejected-$FAILURE_NUMBER.err" ||
	fail "stale-source diagnostic does not name --recreate"
printf '%s\n' local >"$RECREATED/patched/local.txt"
expect_failure "recreation of a dirty source" prepare "$RECREATED" --recreate
[ -f "$RECREATED/patched/local.txt" ] || fail "recreation removed local work"
rm "$RECREATED/patched/local.txt"
git -C "$RECREATED/patched" branch topic
expect_failure "recreation of a source with a branch" prepare "$RECREATED" --recreate
git -C "$RECREATED/patched" branch -D topic >/dev/null
prepare "$RECREATED" --recreate >/dev/null
verify "$RECREATED" >/dev/null
[ "$(cat "$RECREATED/patched/third.txt")" = third ] ||
	fail "recreation did not replay the changed series"
[ -z "$(git -C "$RECREATED/patched" for-each-ref refs/heads)" ] ||
	fail "a prepared source kept a branch copied from the cache"
[ -z "$(find "$RECREATED" -maxdepth 1 -name '.prepare.*' -print)" ] ||
	fail "recreation left its staging directory behind"

printf '%s\n' "dependency preparation tests passed"
