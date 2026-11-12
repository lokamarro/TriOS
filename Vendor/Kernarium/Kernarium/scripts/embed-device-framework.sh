#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Embed a framework that exists for the iOS device alone: the Run Script
# phase of an application target that also builds for the iOS Simulator calls
# it in place of an Embed Frameworks entry.
#
# usage: embed-device-framework.sh XCFRAMEWORK
#
# KosmicKrisp is the Vulkan driver of the device's GPU stack and requires
# Metal 4, which the Simulator does not have, so its XCFramework has a device
# slice alone. Xcode resolves a slice of every XCFramework in an Embed
# Frameworks phase for the SDK it builds, and a platform filter cannot name
# the device apart from the Simulator, so a device-only framework cannot sit
# in that phase of a target the Simulator also builds. This script embeds it
# as the phase does for a device build, with no headers or modules and the
# target's signing identity, and for any other SDK embeds nothing: the
# Simulator's GPU stack has no Vulkan driver to carry.
#
# Xcode runs it with the target's build settings in the environment, on every
# build. It touches the embedded framework only when the framework, or how it
# is signed, changed: a copy and a signature made again on an unchanged build
# alter a file the application's seal covers, and Xcode, which then skips
# signing the application, leaves that seal invalid. A stamp in the target's
# intermediates records the framework, and the signing, it embedded and what
# the embedded framework held afterwards; an embedded framework that matches
# is left as it is.
set -eu

fail()
{
	printf '%s\n' "embed-device-framework.sh: $*" >&2
	exit 1
}

[ "$#" -eq 1 ] || fail "usage: embed-device-framework.sh XCFRAMEWORK"
XCFRAMEWORK=$1
NAME=$(basename -- "$XCFRAMEWORK" .xcframework)

case "${PLATFORM_NAME:-}" in
iphoneos) ;;
*)
	printf '%s\n' "$NAME is a device-only framework: not embedded for ${PLATFORM_NAME:-an unknown SDK}"
	exit 0
	;;
esac

SLICE="$XCFRAMEWORK/ios-arm64/$NAME.framework"
[ -d "$SLICE" ] || fail "$XCFRAMEWORK has no device slice; package it with Kernarium/scripts/package-ios-frameworks.sh"
[ -n "${TARGET_BUILD_DIR:-}" ] && [ -n "${FRAMEWORKS_FOLDER_PATH:-}" ] &&
	[ -n "${TARGET_TEMP_DIR:-}" ] || fail "run it from an Xcode build phase"

DESTINATION="$TARGET_BUILD_DIR/$FRAMEWORKS_FOLDER_PATH/$NAME.framework"
STAMP="$TARGET_TEMP_DIR/embed-device-framework-$NAME.stamp"

# tree_digest DIRECTORY: one digest of the mode, content and path of every
# file and link below DIRECTORY that is embedded, in a fixed order. The names
# are those the copy below leaves out.
tree_digest()
{
	(
		CDPATH= cd -- "$1"
		find . \( -name .DS_Store -o -name Headers -o -name PrivateHeaders \
			-o -name Modules -o -name '*.tbd' \) -prune \
			-o \( -type f -o -type l \) -print | LC_ALL=C sort |
			while IFS= read -r entry; do
				if [ -L "$entry" ]; then
					printf 'link %s %s\n' "$entry" "$(readlink -- "$entry")"
				else
					printf '%s %s %s\n' "$(stat -f %Lp -- "$entry")" \
						"$(/usr/bin/shasum -a 256 <"$entry" | cut -c1-64)" "$entry"
				fi
			done
	) | /usr/bin/shasum -a 256 | cut -c1-64
}

# The framework as embedded: its content, the signing it gets, and this
# script, which decides both.
embed_request()
{
	printf '%s\n%s\n%s\n%s\n%s\n' "$(tree_digest "$SLICE")" \
		"${CODE_SIGNING_ALLOWED:-YES}" "${EXPANDED_CODE_SIGN_IDENTITY:-}" \
		"${OTHER_CODE_SIGN_FLAGS:-}" "$(/usr/bin/shasum -a 256 <"$0" | cut -c1-64)" |
		/usr/bin/shasum -a 256 | cut -c1-64
}

# embedded_up_to_date REQUEST: the stamp names this request and the embedded
# framework still holds what that embedding left.
embedded_up_to_date()
{
	[ -f "$STAMP" ] && [ -d "$DESTINATION" ] || return 1
	[ "$(cat -- "$STAMP")" = "$1 $(tree_digest "$DESTINATION")" ]
}

# This framework bypasses Xcode's Embed Frameworks phase, so its symbols must
# follow the same path explicitly. Xcode gathers that directory into dSYMs in
# the archive; debug symbols never belong inside the application bundle.
if [ "${ACTION:-}" = install ]; then
	SYMBOLS="$XCFRAMEWORK/ios-arm64/dSYMs/$NAME.framework.dSYM"
	[ -d "$SYMBOLS" ] || fail "missing debug symbols: $SYMBOLS; repackage the framework"
	[ -n "${DWARF_DSYM_FOLDER_PATH:-}" ] || fail "the archive has no debug symbols directory"
	mkdir -p -- "$DWARF_DSYM_FOLDER_PATH"
	/usr/bin/rsync -a --delete "$SYMBOLS/" \
		"$DWARF_DSYM_FOLDER_PATH/$NAME.framework.dSYM/"
fi

REQUEST=$(embed_request)
if embedded_up_to_date "$REQUEST"; then
	printf '%s\n' "$NAME is already embedded for the device"
	exit 0
fi

rm -f -- "$STAMP"
mkdir -p -- "$TARGET_BUILD_DIR/$FRAMEWORKS_FOLDER_PATH" "$TARGET_TEMP_DIR"
rm -rf -- "$DESTINATION"
/usr/bin/rsync -a \
	--exclude .DS_Store --exclude Headers --exclude PrivateHeaders \
	--exclude Modules --exclude '*.tbd' \
	"$SLICE/" "$DESTINATION/"

# Sign it on copy, as the Embed Frameworks phase's CodeSignOnCopy does.
if [ "${CODE_SIGNING_ALLOWED:-YES}" = YES ] &&
	[ -n "${EXPANDED_CODE_SIGN_IDENTITY:-}" ]; then
	# shellcheck disable=SC2086
	/usr/bin/codesign --force --sign "$EXPANDED_CODE_SIGN_IDENTITY" \
		${OTHER_CODE_SIGN_FLAGS:-} \
		--preserve-metadata=identifier,entitlements,flags \
		--generate-entitlement-der "$DESTINATION"
fi
printf '%s %s\n' "$REQUEST" "$(tree_digest "$DESTINATION")" >"$STAMP"
printf '%s\n' "embedded $NAME for the device"
