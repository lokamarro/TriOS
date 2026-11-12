# SPDX-License-Identifier: GPL-2.0-or-later
#
# Helpers the dependency builders share. Sourcing this file only defines
# names. A caller defines fail() and ROOT_DIR, and sets PYTHON before it
# publishes.

# find_tool VARIABLE CANDIDATE...: set VARIABLE to the path of the first
# candidate on PATH. A tool found through a relative path is made absolute,
# because builders run their tools in other directories.
find_tool()
{
	variable=$1
	shift
	for candidate in "$@"; do
		tool_path=$(command -v "$candidate" 2>/dev/null || true)
		if [ -n "$tool_path" ]; then
			case $tool_path in
			/*) ;;
			*/*) tool_path="$PWD/$tool_path" ;;
			esac
			eval "$variable=\$tool_path"
			return
		fi
	done
	fail "required build tool is unavailable: $*"
}

# publish_verified STAGE DESTINATION VERIFY LABEL: verify the stage with the
# VERIFY function and rename it to DESTINATION, which must not exist. A
# destination that a concurrent build published first (publish-prefix.py
# status 1) is verified and reused, and the stage is left for the caller's
# cleanup; any other publisher failure fails. STAGE is cleared once the
# stage has become the destination.
publish_verified()
{
	publish_stage=$1
	publish_destination=$2
	publish_verify=$3
	publish_label=$4
	"$publish_verify" "$publish_stage"
	publish_status=0
	"$PYTHON" "$ROOT_DIR/dependencies/publish-prefix.py" \
		"$publish_stage" "$publish_destination" || publish_status=$?
	case $publish_status in
	0)
		STAGE=
		printf '%s\n' "published $publish_label: $publish_destination"
		;;
	1)
		"$publish_verify" "$publish_destination" ||
			fail "concurrently published $publish_label is invalid: $publish_destination"
		printf '%s\n' \
			"reusing concurrently published $publish_label: $publish_destination"
		;;
	*)
		fail "could not publish $publish_label: $publish_destination"
		;;
	esac
}

# NO_HOST_PREFIX is the installation prefix of a dependency whose binaries
# ship. A dependency compiles in directories it derives from its prefix (a
# locale, libexec or configuration directory), so a prefix under the staging
# directory, or the final prefix, would put that machine's layout in every
# binary built from it. Nothing on an Apple product has this directory: what
# reads a directory compiled in from it finds nothing, as it found nothing
# under a staging directory that was deleted or a prefix that never held it.
NO_HOST_PREFIX=/nonexistent

# merge_neutral_image IMAGE STAGE PKGCONFIG-PREFIX: a dependency configured with
# --prefix "$NO_HOST_PREFIX" and installed with DESTDIR=IMAGE holds its files
# below IMAGE$NO_HOST_PREFIX. Copy them to the same places in STAGE, and make
# the pkg-config files of the image name PKGCONFIG-PREFIX, the prefix the
# dependants find it under while they build, instead of the neutral one.
# RSYNC and fail() are the caller's.
merge_neutral_image()
{
	merge_image=$1
	merge_stage=$2
	merge_pkgconfig_prefix=$3
	[ -d "$merge_image$NO_HOST_PREFIX" ] ||
		fail "the install image has no $NO_HOST_PREFIX: $merge_image"
	merge_outside=$(find "$merge_image" -mindepth 1 -maxdepth 1 \
		! -name "${NO_HOST_PREFIX#/}" -print -quit)
	[ -z "$merge_outside" ] ||
		fail "the install image holds a file outside $NO_HOST_PREFIX: $merge_outside"
	for merge_pc in "$merge_image$NO_HOST_PREFIX"/lib/pkgconfig/*.pc; do
		[ -f "$merge_pc" ] || continue
		sed -e "s|^prefix=$NO_HOST_PREFIX\$|prefix=$merge_pkgconfig_prefix|" \
			"$merge_pc" > "$merge_pc.prefix" ||
			fail "could not rewrite the prefix of $merge_pc"
		mv -f -- "$merge_pc.prefix" "$merge_pc"
	done
	$RSYNC -a "$merge_image$NO_HOST_PREFIX/" "$merge_stage/" ||
		fail "could not merge the install image into $merge_stage"
}
