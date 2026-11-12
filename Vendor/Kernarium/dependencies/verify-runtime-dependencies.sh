#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

BINARY=
APPLE_GPU_PREFIX=
FRAMEWORK_DIRECTORY=

fail()
{
	printf '%s\n' "dependencies/verify-runtime-dependencies.sh: $*" >&2
	exit 1
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--binary)
		[ "$#" -ge 2 ] || fail "--binary requires a path"
		BINARY=$2
		shift 2
		;;
	--apple-gpu-prefix)
		[ "$#" -ge 2 ] || fail "--apple-gpu-prefix requires a path"
		APPLE_GPU_PREFIX=$2
		shift 2
		;;
	--framework-directory)
		[ "$#" -ge 2 ] || fail "--framework-directory requires a path"
		FRAMEWORK_DIRECTORY=$2
		shift 2
		;;
	*) fail "unknown option: $1" ;;
	esac
done

# The Apple GPU product renders OpenGL with Mesa's Zink Gallium driver on the
# KosmicKrisp Vulkan driver.
GL_FRAMEWORKS='MesaEGL MesaGallium'
HOST_GL_MARKER='Virgl Zink/KosmicKrisp provider: hardware'

[ -f "$BINARY" ] || fail "linked production executable is missing: $BINARY"
case "$APPLE_GPU_PREFIX:$FRAMEWORK_DIRECTORY" in
':') ;;
*:*)
	[ -n "$APPLE_GPU_PREFIX" ] && [ -n "$FRAMEWORK_DIRECTORY" ] ||
		fail "Apple GPU prefix and framework directory must be specified together"
	;;
esac
command -v xcrun >/dev/null 2>&1 || fail "xcrun is required"
OTOOL=$(xcrun --find otool)
[ -x "$OTOOL" ] || fail "xcrun returned an invalid otool"
STRINGS=$(xcrun --find strings)
[ -x "$STRINGS" ] || fail "xcrun returned an invalid strings tool"

forbidden_dependency()
{
	case "$1" in
	*OpenGL*|*OpenGLES*|*Vulkan*) return 0 ;;
	esac
	return 1
}

binary_dependencies=$(
	"$OTOOL" -L "$BINARY" | awk 'NR > 1 { print $1 }'
)
printf '%s\n' "$binary_dependencies" |
while IFS= read -r dependency; do
	[ -n "$dependency" ] || continue
	forbidden_dependency "$dependency" &&
		fail "production executable has a forbidden graphics dependency: $dependency"
	case "$dependency" in
	/System/Library/*|/usr/lib/*) ;;
	@rpath/MesaEGL.framework/MesaEGL|\
	@rpath/MesaGallium.framework/MesaGallium|\
	@rpath/KosmicKrisp.framework/KosmicKrisp)
		[ -n "$APPLE_GPU_PREFIX" ] ||
			fail "production executable has an unauthenticated Apple GPU dependency: $dependency"
		;;
	*) fail "production executable has a non-system dynamic dependency: $dependency" ;;
	esac
done

[ -n "$APPLE_GPU_PREFIX" ] || {
	printf '%s\n' "verified production runtime dependency closure: $BINARY"
	exit 0
}

"$STRINGS" "$BINARY" |
	grep -F -x "$HOST_GL_MARKER" >/dev/null ||
	fail "production executable is missing the host GL provider marker"

[ -d "$APPLE_GPU_PREFIX" ] && [ ! -L "$APPLE_GPU_PREFIX" ] ||
	fail "Apple GPU prefix is missing or symbolic: $APPLE_GPU_PREFIX"
[ -d "$FRAMEWORK_DIRECTORY" ] && [ ! -L "$FRAMEWORK_DIRECTORY" ] ||
	fail "embedded framework directory is missing or symbolic: $FRAMEWORK_DIRECTORY"

BINARY_DIRECTORY=$(CDPATH= cd -- "$(dirname -- "$BINARY")" && pwd -P)
FRAMEWORK_PARENT=$(CDPATH= cd -- "$(dirname -- "$FRAMEWORK_DIRECTORY")" && pwd -P)
FRAMEWORK_DIRECTORY="$FRAMEWORK_PARENT/$(basename -- "$FRAMEWORK_DIRECTORY")"
[ "$FRAMEWORK_DIRECTORY" = "$BINARY_DIRECTORY/Frameworks" ] ||
	fail "Apple GPU frameworks must be embedded beside the executable in Frameworks"
SOURCE_FRAMEWORKS="$APPLE_GPU_PREFIX/Frameworks"
[ -d "$SOURCE_FRAMEWORKS" ] && [ ! -L "$SOURCE_FRAMEWORKS" ] ||
	fail "verified Apple GPU framework source is missing"

embedded_symlink=$(find "$FRAMEWORK_DIRECTORY" -type l -print -quit)
[ -z "$embedded_symlink" ] ||
	fail "embedded Apple GPU frameworks contain a symbolic link: $embedded_symlink"
source_symlink=$(find "$SOURCE_FRAMEWORKS" -type l -print -quit)
[ -z "$source_symlink" ] ||
	fail "source Apple GPU frameworks contain a symbolic link: $source_symlink"

embedded_frameworks=$(
	CDPATH= cd -- "$FRAMEWORK_DIRECTORY"
	find . -mindepth 1 -maxdepth 1 -print | LC_ALL=C sort
)
frameworks="$GL_FRAMEWORKS KosmicKrisp"
expected_frameworks=$(
	for framework in $frameworks; do
		printf './%s.framework\n' "$framework"
	done | LC_ALL=C sort
)
[ "$embedded_frameworks" = "$expected_frameworks" ] ||
	fail "embedded Apple GPU framework set is not exact: $embedded_frameworks"

for framework in $frameworks; do
	source="$SOURCE_FRAMEWORKS/$framework.framework"
	embedded="$FRAMEWORK_DIRECTORY/$framework.framework"
	[ -d "$source" ] && [ -d "$embedded" ] ||
		fail "$framework.framework is missing from the source or runtime"
	source_payload=$(
		CDPATH= cd -- "$source"
		find . -mindepth 1 -print | LC_ALL=C sort
	)
	embedded_payload=$(
		CDPATH= cd -- "$embedded"
		find . -mindepth 1 -print | LC_ALL=C sort
	)
	[ "$source_payload" = "$embedded_payload" ] ||
		fail "$framework.framework payload is not exact: $embedded_payload"
	printf '%s\n' "$source_payload" |
	while IFS= read -r relative; do
		[ -f "$source/$relative" ] || continue
		cmp -s "$source/$relative" "$embedded/$relative" ||
			fail "embedded $framework.framework does not match the authenticated prefix"
	done

	install_name=$("$OTOOL" -D "$embedded/$framework" |
		awk 'NR > 1 { print $1; exit }')
	[ "$install_name" = "@rpath/$framework.framework/$framework" ] ||
		fail "$framework.framework has an invalid install name: $install_name"
	framework_dependencies=$(
		"$OTOOL" -L "$embedded/$framework" | awk 'NR > 1 { print $1 }'
	)
	printf '%s\n' "$framework_dependencies" |
	while IFS= read -r dependency; do
		[ -n "$dependency" ] || continue
		forbidden_dependency "$dependency" &&
			fail "$framework.framework has a forbidden graphics dependency: $dependency"
		case "$dependency" in
		@rpath/$framework.framework/$framework|\
		@rpath/MesaGallium.framework/MesaGallium|\
		@rpath/KosmicKrisp.framework/KosmicKrisp|\
		/usr/lib/libc++.1.dylib|/usr/lib/libSystem.B.dylib|\
		/usr/lib/libobjc.A.dylib|/usr/lib/libz.1.dylib|\
		/usr/lib/libexpat.1.dylib|\
		/System/Library/Frameworks/AppKit.framework/*|\
		/System/Library/Frameworks/CoreFoundation.framework/*|\
		/System/Library/Frameworks/CoreGraphics.framework/*|\
		/System/Library/Frameworks/Foundation.framework/*|\
		/System/Library/Frameworks/IOKit.framework/*|\
		/System/Library/Frameworks/IOSurface.framework/*|\
		/System/Library/Frameworks/Metal.framework/*|\
		/System/Library/Frameworks/QuartzCore.framework/*) ;;
		*) fail "$framework.framework has an unexpected dependency: $dependency" ;;
		esac
	done

	copy_count=$(find "$BINARY_DIRECTORY" -type d \
		-name "$framework.framework" -print | awk 'END { print NR + 0 }')
	[ "$copy_count" -eq 1 ] ||
		fail "runtime must contain exactly one $framework.framework copy"
done

for framework in $frameworks; do
	count=$(printf '%s\n' "$binary_dependencies" |
		awk -v expected="@rpath/$framework.framework/$framework" \
			'$0 == expected { count++ } END { print count + 0 }')
	[ "$count" -eq 1 ] ||
		fail "production executable must load exactly one $framework.framework"
done

rpaths=$("$OTOOL" -l "$BINARY" | awk '
	$1 == "cmd" { command = $2; next }
	command == "LC_RPATH" && $1 == "path" { print $2; command = "" }
')
[ "$rpaths" = '@executable_path/Frameworks' ] ||
	fail "production executable has an invalid runtime search path: $rpaths"

printf '%s\n' "verified production Apple GPU runtime closure: $BINARY"
