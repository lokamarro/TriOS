#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# A dependency builder resolves relative paths against the directory it was
# started in, including in steps that run elsewhere. The kernel host tools
# build runs here from another directory with relative options, through the
# extraction of the Linux UAPI headers from the cached linux-libc-dev archive.
# A stub stops it at the next step.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-relative-paths.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

TEST_NAME=test-relative-paths.sh
. "$(dirname -- "$0")/../support/expect.sh"

# The builder runs from a fixture repository root, so its preparer and archive
# manifest are the fixture's. The preparer authenticates archives with the real
# one; the fixture sources are not prepared repositories.
FIXTURE="$TEST_ROOT/root"
mkdir -p "$FIXTURE/dependencies/kernel-host-tools"
for script in build-kernel-host-tools.sh build-common.sh; do
	ln -s "$ROOT_DIR/dependencies/$script" "$FIXTURE/dependencies/$script"
done
cat >"$FIXTURE/dependencies/prepare.sh" <<EOF
#!/bin/sh
case " \$* " in
*" --archive-manifest "*) exec "$ROOT_DIR/dependencies/prepare.sh" "\$@" ;;
esac
EOF
chmod +x "$FIXTURE/dependencies/prepare.sh"

# The cached package carries one marker header in each directory the build
# takes from it.
PACKAGE="$TEST_ROOT/package"
for directory in usr/include/linux usr/include/asm-generic \
	usr/lib/linux/uapi/arm64/asm; do
	mkdir -p "$PACKAGE/$directory"
	: >"$PACKAGE/$directory/fixture.h"
done
tar -cJf "$TEST_ROOT/data.tar.xz" -C "$PACKAGE" ./usr
tar --format=argnu -cf "$TEST_ROOT/package.deb" -C "$TEST_ROOT" data.tar.xz
ARCHIVE_SHA=$(shasum -a 256 "$TEST_ROOT/package.deb" | awk '{ print $1 }')
awk -F '\t' -v OFS='\t' -v sha="$ARCHIVE_SHA" '
	/^#/ || $1 == "name" { print; next }
	$1 ~ /^linux-libc-dev_.*\.deb$/ { $3 = sha; print }
' "$ROOT_DIR/dependencies/kernel-host-tools/archives.tsv" \
	>"$FIXTURE/dependencies/kernel-host-tools/archives.tsv"
ARCHIVE_NAME=$(awk -F '\t' '$3 == sha { print $1 }' sha="$ARCHIVE_SHA" \
	"$FIXTURE/dependencies/kernel-host-tools/archives.tsv")
[ -n "$ARCHIVE_NAME" ] || fail "the archive manifest names no linux-libc-dev package"

# Every relative path below is relative to RUN, the build's directory.
RUN="$TEST_ROOT/run"
mkdir -p "$RUN/cache/archives" "$RUN/tools" "$RUN/sources/gawk" \
	"$RUN/sources/musl-obstack" "$RUN/sources/elfutils"
cp "$TEST_ROOT/package.deb" "$RUN/cache/archives/$ARCHIVE_SHA-$ARCHIVE_NAME"
cat >"$RUN/sources/gawk/configure" <<'EOF'
#!/bin/sh
for argument in "$@"; do
	case $argument in --prefix=*) prefix=${argument#--prefix=} ;; esac
done
mkdir -p "$prefix/bin"
printf '#!/bin/sh\n' >"$prefix/bin/gawk"
chmod +x "$prefix/bin/gawk"
EOF
printf '#!/bin/sh\n' >"$RUN/sources/musl-obstack/configure"
for tool in gmake meson ninja bison flex m4; do
	printf '#!/bin/sh\n' >"$RUN/tools/$tool"
done
# elfutils' first step follows the UAPI extraction. It records the marker
# headers the build extracted and stops the build.
cat >"$RUN/tools/autoreconf" <<'EOF'
#!/bin/sh
case $PWD in
*/src/elfutils)
	for header in linux asm-generic asm; do
		[ -f "../../linux-uapi/include/$header/fixture.h" ] &&
			printf '%s\n' "$header"
	done >"$(dirname -- "$0")/../extracted-headers"
	exit 1
	;;
esac
EOF
chmod +x "$RUN/sources/gawk/configure" "$RUN/sources/musl-obstack/configure" \
	"$RUN/tools/"*

# A builder's tool override may name a tool by a relative path, which still
# runs after a change of directory.
(
	CDPATH= cd -- "$RUN"
	. "$ROOT_DIR/dependencies/build-common.sh"
	find_tool RELATIVE_TOOL tools/gmake
	cd /
	"$RELATIVE_TOOL"
) || fail "a build tool named by a relative path did not run in another directory"

CDPATH= cd -- "$RUN"
expect_diagnostic 'elfutils failed' env PATH="$RUN/tools:$PATH" \
	../root/dependencies/build-kernel-host-tools.sh \
	--source-root sources --cache cache --output out/kernel-host-tools \
	--work-root work --jobs 1
[ "$(cat extracted-headers)" = "$(printf 'linux\nasm-generic\nasm')" ] ||
	fail "the build did not extract the UAPI headers from the relative cache"
[ -d work ] && [ -d out ] ||
	fail "the work root and output parent were not created below the build's directory"

printf '%s\n' "dependency relative path tests passed"
