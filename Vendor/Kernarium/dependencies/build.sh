#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the authenticated dependency source trees into one private, static
# prefix. Package-manager prefixes and user package registries are excluded;
# only build tools and Apple SDK platform libraries come from the host.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE_ROOT=${UML_DEPENDENCY_SOURCE_ROOT:-"$ROOT_DIR/external/sources"}
PREFIX=${UML_DEPENDENCY_PREFIX:-"$ROOT_DIR/.build/dependencies/macos-arm64-27"}
WORK_ROOT=${UML_DEPENDENCY_BUILD_ROOT:-"$ROOT_DIR/.build/dependency-build"}
MANIFEST=${UML_DEPENDENCY_MANIFEST:-"$ROOT_DIR/dependencies/manifest.tsv"}
PREPARE_SCRIPT="$ROOT_DIR/dependencies/prepare.sh"
JOBS=${UML_DEPENDENCY_JOBS:-8}
DEPLOYMENT_TARGET=27.0
CHECK_MANIFEST_ONLY=0
BUILD_RECIPES='zlib|1.3.2
pcre2|10.49
libffi|3.8.0
gnulib|2026-10-31
libiconv|1.19.1
proxy-libintl|0.5
glib|2.90.1
gvdb|2b42fc7
libslirp|4.9.7
libvncserver|0.9.15
fex|FEX-2610
fmt|12.2.1
xxhash|0.8.3
unordered-dense|4.8.1'

fail()
{
	printf '%s\n' "dependencies/build.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: dependencies/build.sh [options]

Options:
  --source-root PATH  Verified source trees (default: external/sources)
  --prefix PATH       Private installation prefix
  --work-root PATH    Temporary build parent
  --jobs NUMBER       Parallel build jobs (default: 8)
  --check-manifest-only
                      Check manifest/build coverage without building
  -h, --help          Show this help

An already valid prefix is reused. An invalid prefix is never overwritten;
move it aside after reviewing the verifier's diagnostic, then run again.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--source-root)
		[ "$#" -ge 2 ] || fail "--source-root requires a path"
		SOURCE_ROOT=$2
		shift 2
		;;
	--prefix)
		[ "$#" -ge 2 ] || fail "--prefix requires a path"
		PREFIX=$2
		shift 2
		;;
	--work-root)
		[ "$#" -ge 2 ] || fail "--work-root requires a path"
		WORK_ROOT=$2
		shift 2
		;;
	--jobs)
		[ "$#" -ge 2 ] || fail "--jobs requires a positive integer"
		JOBS=$2
		shift 2
		;;
	--check-manifest-only)
		CHECK_MANIFEST_ONLY=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

verify_build_manifest()
{
	manifest_recipes=$(awk -F '\t' '
		/^#/ || /^[[:space:]]*$/ { next }
		$1 == "name" { header++; next }
		header == 1 && index("," $3 ",", ",macos,") {
			print $1 "|" $2
		}
		END { if (header != 1) exit 2 }
	' "$MANIFEST") || fail "could not read dependency recipes from $MANIFEST"
	[ "$manifest_recipes" = "$BUILD_RECIPES" ] || {
		printf '%s\n' "implemented dependency recipes:" "$BUILD_RECIPES" >&2
		printf '%s\n' "manifest dependency recipes:" "$manifest_recipes" >&2
		fail "manifest rows and implemented dependency builds differ"
	}
}

[ -r "$MANIFEST" ] || fail "dependency manifest is unavailable: $MANIFEST"
UML_DEPENDENCY_MANIFEST="$MANIFEST" \
UML_DEPENDENCY_PATCH_ROOT="$ROOT_DIR/dependencies" \
	"$PREPARE_SCRIPT" --validate-manifest-only >/dev/null
verify_build_manifest
BUILD_DEPENDENCIES=$(printf '%s\n' "$BUILD_RECIPES" | awk -F '|' '{print $1}')
[ "$CHECK_MANIFEST_ONLY" -eq 0 ] || {
	printf '%s\n' "dependency manifest covers every implemented build"
	exit 0
}

case "$JOBS" in
*[!0-9]*|'') fail "jobs must be a positive integer" ;;
esac
[ "$JOBS" -gt 0 ] || fail "jobs must be a positive integer"
[ "$(uname -s)" = Darwin ] || fail "the host dependency build requires macOS"
case "$(uname -m)" in
arm64|aarch64) ;;
*) fail "the host dependency build currently requires arm64" ;;
esac

. "$ROOT_DIR/dependencies/build-common.sh"

find_tool CMAKE "${UML_DEPENDENCY_CMAKE:-cmake}"
if [ -x "$ROOT_DIR/.build/gpu-tools-venv/bin/meson" ]; then
	MESON="$ROOT_DIR/.build/gpu-tools-venv/bin/meson"
else
	find_tool MESON "${UML_DEPENDENCY_MESON:-meson}"
fi
find_tool NINJA "${UML_DEPENDENCY_NINJA:-ninja}"
find_tool MAKE "${UML_DEPENDENCY_MAKE:-gmake}" make
find_tool CC "${UML_DEPENDENCY_CC:-clang}"
find_tool CXX "${UML_DEPENDENCY_CXX:-clang++}"
find_tool PKG_CONFIG "${UML_DEPENDENCY_PKG_CONFIG:-pkg-config}"
find_tool PYTHON "${UML_DEPENDENCY_PYTHON:-python3}"
find_tool MESON_CMAKE_BLOCKER /usr/bin/false
find_tool XCRUN xcrun
find_tool INSTALL install
find_tool RSYNC rsync
find_tool AUTORECONF autoreconf
find_tool AUTOCONF autoconf
find_tool AUTOMAKE automake
find_tool ACLOCAL aclocal
find_tool AUTOPOINT autopoint
find_tool LIBTOOLIZE glibtoolize libtoolize
find_tool GPERF gperf

verify_prefix()
{
	"$ROOT_DIR/dependencies/verify-prefix.sh" \
		--prefix "$1" --manifest "$MANIFEST" \
		--deployment-target "$DEPLOYMENT_TARGET"
}

if [ -e "$PREFIX" ] || [ -L "$PREFIX" ]; then
	verify_prefix "$PREFIX" ||
		fail "existing prefix is invalid and was left untouched: $PREFIX"
	printf '%s\n' "reusing verified dependency prefix: $PREFIX"
	exit 0
fi

"$PREPARE_SCRIPT" --product macos --root "$SOURCE_ROOT" --verify

SDK_ROOT=$($XCRUN --sdk macosx --show-sdk-path)
[ -d "$SDK_ROOT" ] || fail "xcrun returned an invalid macOS SDK path"
SDK_VERSION=$($XCRUN --sdk macosx --show-sdk-version)
[ -n "$SDK_VERSION" ] || fail "xcrun returned an empty macOS SDK version"
MESON_DIR=$(dirname -- "$MESON")

PREFIX_PARENT=$(dirname -- "$PREFIX")
mkdir -p -- "$WORK_ROOT" "$PREFIX_PARENT"
WORK_ROOT=$(CDPATH= cd -- "$WORK_ROOT" && pwd -P)
PREFIX_PARENT=$(CDPATH= cd -- "$PREFIX_PARENT" && pwd -P)
PREFIX="$PREFIX_PARENT/$(basename -- "$PREFIX")"
TEMP_ROOT=
IMAGE_ROOT=
STAGE=
cleanup()
{
	if [ -n "${IMAGE_ROOT:-}" ] && [ -d "$IMAGE_ROOT" ]; then
		rm -rf -- "$IMAGE_ROOT"
	fi
	if [ -n "${TEMP_ROOT:-}" ] && [ -d "$TEMP_ROOT" ]; then
		rm -rf -- "$TEMP_ROOT"
	fi
}
trap cleanup EXIT HUP INT TERM

TEMP_ROOT=$(mktemp -d "$WORK_ROOT/source-build.XXXXXX")
# Configure with PREFIX so installed metadata contains its final location, but
# install below a sibling DESTDIR image.  The image's PREFIX subtree remains a
# same-filesystem directory that can be verified and published atomically.
IMAGE_ROOT=$(mktemp -d "$PREFIX_PARENT/.uml-dependency-image.XXXXXX")
STAGE="$IMAGE_ROOT$PREFIX"
SOURCES="$TEMP_ROOT/sources"
BUILDS="$TEMP_ROOT/builds"
mkdir -p -- "$STAGE" "$SOURCES" "$BUILDS"

copy_source()
{
	name=$1
	[ -d "$SOURCE_ROOT/$name" ] || fail "prepared source is missing: $name"
	mkdir -p -- "$SOURCES/$name"
	$RSYNC -a --exclude .git "$SOURCE_ROOT/$name/" "$SOURCES/$name/"
}

for dependency in $BUILD_DEPENDENCIES; do
	copy_source "$dependency"
done

PRIVATE_CPPFLAGS="-I$STAGE/include"
PRIVATE_CFLAGS="-O3 -DNDEBUG -I$STAGE/include"
PRIVATE_CXXFLAGS="-O3 -DNDEBUG -I$STAGE/include"
PRIVATE_LDFLAGS="-L$STAGE/lib"
PRIVATE_PKG_CONFIG="$STAGE/lib/pkgconfig"

private_env()
{
	env \
		PATH="$MESON_DIR:$PATH" \
		CC="$CC" CXX="$CXX" \
		CPPFLAGS="$PRIVATE_CPPFLAGS" \
		CFLAGS="$PRIVATE_CFLAGS" \
		CXXFLAGS="$PRIVATE_CXXFLAGS" \
		LDFLAGS="$PRIVATE_LDFLAGS" \
		DESTDIR= \
		PKG_CONFIG="$PKG_CONFIG" \
		PKG_CONFIG_PATH= \
		PKG_CONFIG_LIBDIR="$PRIVATE_PKG_CONFIG" \
		PKG_CONFIG_SYSROOT_DIR="$IMAGE_ROOT" \
		PKG_CONFIG_DIR= \
		PKG_CONFIG_SYSTEM_INCLUDE_PATH= \
		PKG_CONFIG_SYSTEM_LIBRARY_PATH= \
		CMAKE_PREFIX_PATH= \
		CMAKE_INCLUDE_PATH= \
		CMAKE_LIBRARY_PATH= \
		CMAKE_FRAMEWORK_PATH= \
		CMAKE_APPBUNDLE_PATH= \
		CMAKE_TOOLCHAIN_FILE= \
		CPATH= C_INCLUDE_PATH= CPLUS_INCLUDE_PATH= OBJC_INCLUDE_PATH= \
		LIBRARY_PATH= \
		ACLOCAL_PATH= CONFIG_SITE= \
		MAKEINFO=true \
		SDKROOT="$SDK_ROOT" \
		MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
		"$@"
}

private_install_env()
{
	private_env env DESTDIR="$IMAGE_ROOT" "$@"
}

# Meson may fall back from pkg-config to a host CMake package search even for
# optional dependencies.  CMake-based dependency discovery is disabled for
# Meson only; the project invokes its pinned CMake tool directly where needed.
meson_env()
{
	private_env env CMAKE="$MESON_CMAKE_BLOCKER" "$MESON" "$@"
}

meson_install_env()
{
	private_install_env env CMAKE="$MESON_CMAKE_BLOCKER" "$MESON" "$@"
}

require_private_pkg_config()
{
	package=$1
	if ! package_prefix=$(private_env env PKG_CONFIG_SYSROOT_DIR= \
		"$PKG_CONFIG" \
		--variable=prefix "$package"); then
		fail "$package is unavailable from the private dependency prefix"
	fi
	[ "$package_prefix" = "$PREFIX" ] ||
		fail "$package resolved outside the private prefix: $package_prefix"
	if ! package_cflags=$(private_env "$PKG_CONFIG" \
		--cflags-only-I "$package"); then
		fail "$package staged include flags are unavailable"
	fi
	case " $package_cflags " in
	*" -I$STAGE/"*) ;;
	*) fail "$package include flags do not use the staged image: $package_cflags" ;;
	esac
	if ! package_libs=$(private_env "$PKG_CONFIG" \
		--libs-only-L "$package"); then
		fail "$package staged library flags are unavailable"
	fi
	case " $package_libs " in
	*" -L$STAGE/lib "*) ;;
	*) fail "$package library flags do not use the staged image: $package_libs" ;;
	esac
}

cmake_setup()
{
	source=$1
	build=$2
	shift 2
	private_env "$CMAKE" -S "$source" -B "$build" -G Ninja \
		-DCMAKE_MAKE_PROGRAM="$NINJA" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_INSTALL_LIBDIR=lib \
		-DCMAKE_OSX_SYSROOT="$SDK_ROOT" \
		-DCMAKE_OSX_ARCHITECTURES=arm64 \
		-DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
		-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
		-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF \
		-DCMAKE_IGNORE_PREFIX_PATH='/opt/homebrew;/usr/local;/opt/local' \
		"$@"
}

cmake_build_install()
{
	build=$1
	private_env "$CMAKE" --build "$build" --parallel "$JOBS"
	private_install_env "$CMAKE" --install "$build"
}

printf '%s\n' "building zlib 1.3.2"
cmake_setup "$SOURCES/zlib" "$BUILDS/zlib" \
	-DZLIB_BUILD_SHARED=OFF \
	-DZLIB_BUILD_STATIC=ON \
	-DZLIB_BUILD_TESTING=OFF
cmake_build_install "$BUILDS/zlib"

printf '%s\n' "building PCRE2 10.49"
cmake_setup "$SOURCES/pcre2" "$BUILDS/pcre2" \
	-DBUILD_SHARED_LIBS=OFF \
	-DBUILD_STATIC_LIBS=ON \
	-DPCRE2_BUILD_PCRE2_8=ON \
	-DPCRE2_BUILD_PCRE2_16=OFF \
	-DPCRE2_BUILD_PCRE2_32=OFF \
	-DPCRE2_BUILD_PCRE2GREP=OFF \
	-DPCRE2_BUILD_TESTS=OFF \
	-DCMAKE_DISABLE_FIND_PACKAGE_BZip2=ON \
	-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON \
	-DCMAKE_DISABLE_FIND_PACKAGE_Readline=ON \
	-DCMAKE_DISABLE_FIND_PACKAGE_Editline=ON \
	-DPCRE2_SUPPORT_LIBZ=OFF \
	-DPCRE2_SUPPORT_LIBBZ2=OFF \
	-DPCRE2_SUPPORT_LIBREADLINE=OFF \
	-DPCRE2_SUPPORT_LIBEDIT=OFF
cmake_build_install "$BUILDS/pcre2"

printf '%s\n' "building libffi 3.8.0"
(
	CDPATH= cd -- "$SOURCES/libffi"
	private_env ./autogen.sh
)
mkdir -p -- "$BUILDS/libffi"
(
	CDPATH= cd -- "$BUILDS/libffi"
	private_env "$SOURCES/libffi/configure" \
		--prefix="$PREFIX" --libdir="$PREFIX/lib" \
		--disable-shared --enable-static --disable-docs \
		--disable-multi-os-directory
	private_env "$MAKE" -j "$JOBS"
	private_env "$MAKE" DESTDIR="$IMAGE_ROOT" install
)

printf '%s\n' "building GNU libiconv 1.19.1"
# The pinned developer Makefiles name automake-1.18/aclocal-1.18 explicitly.
# Current Homebrew exposes its backward-compatible Automake only through the
# unversioned commands. Supply those names inside this disposable build tree;
# they are build tools and never enter the published prefix.
AUTOMAKE_COMPAT="$BUILDS/automake-compat"
mkdir -p -- "$AUTOMAKE_COMPAT"
ln -s "$AUTOMAKE" "$AUTOMAKE_COMPAT/automake-1.18"
ln -s "$ACLOCAL" "$AUTOMAKE_COMPAT/aclocal-1.18"
(
	CDPATH= cd -- "$SOURCES/libiconv"
	private_env env PATH="$AUTOMAKE_COMPAT:$PATH" \
		MAKE="$MAKE" GNULIB_SRCDIR="$SOURCES/gnulib" \
		./autogen.sh
)
mkdir -p -- "$BUILDS/libiconv"
(
	CDPATH= cd -- "$BUILDS/libiconv"
	private_env "$SOURCES/libiconv/configure" \
		--prefix="$PREFIX" --libdir="$PREFIX/lib" \
		--disable-shared --enable-static --disable-nls
	private_env "$MAKE" -j "$JOBS"
	private_env "$MAKE" DESTDIR="$IMAGE_ROOT" install
)

printf '%s\n' "building GLib 2.90.1"
require_private_pkg_config libpcre2-8
require_private_pkg_config libffi
mkdir -p -- "$SOURCES/glib/subprojects/proxy-libintl-0.5"
cp -R "$SOURCES/proxy-libintl/." \
	"$SOURCES/glib/subprojects/proxy-libintl-0.5"
mkdir -p -- "$SOURCES/glib/subprojects/gvdb"
cp -R "$SOURCES/gvdb/." "$SOURCES/glib/subprojects/gvdb"
# GLib compiles its locale, character-set alias, module, libexec and state
# directories in from its prefix, which here is the final one. It is configured
# with the neutral prefix instead, so that its libraries hold no path of this
# build and the prefix can be renamed, and installed into an image that is
# merged into the stage; its pkg-config files name the final prefix, where the
# dependants of this build find them.
meson_env setup "$BUILDS/glib" "$SOURCES/glib" \
	--prefix="$NO_HOST_PREFIX" --libdir=lib --buildtype=release \
	--default-library=static --wrap-mode=nodownload \
	--force-fallback-for=intl \
	-Dtests=false -Dinstalled_tests=false -Dnls=disabled \
	-Dintrospection=disabled -Ddocumentation=false -Dman-pages=disabled \
	-Dselinux=disabled -Dlibmount=disabled -Dsysprof=disabled \
	-Dxattr=false -Ddtrace=disabled -Dsystemtap=disabled \
	-Dlibelf=disabled -Dglib_debug=disabled -Dbsymbolic_functions=false
meson_env compile -C "$BUILDS/glib"
GLIB_IMAGE="$TEMP_ROOT/glib-image"
private_env env DESTDIR="$GLIB_IMAGE" CMAKE="$MESON_CMAKE_BLOCKER" \
	"$MESON" install -C "$BUILDS/glib"
merge_neutral_image "$GLIB_IMAGE" "$STAGE" "$PREFIX"

printf '%s\n' "building libslirp 4.9.7"
# libslirp's project-level static option selects a non-installed helper target.
# Let Meson's default-library setting make the installable library target static.
require_private_pkg_config glib-2.0
meson_env setup "$BUILDS/libslirp" "$SOURCES/libslirp" \
	--prefix="$PREFIX" --libdir=lib --buildtype=release \
	--default-library=static --wrap-mode=nofallback \
	-Dstatic=false -Doss-fuzz=false -Dllvm-fuzz=false -Dfuzz-reproduce=false
meson_env compile -C "$BUILDS/libslirp"
meson_install_env install -C "$BUILDS/libslirp"

printf '%s\n' "building pristine LibVNCServer 0.9.15"
cmake_setup "$SOURCES/libvncserver" "$BUILDS/libvncserver" \
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	-DLIBVNCSERVER_INSTALL=ON \
	-DBUILD_SHARED_LIBS=OFF \
	-DWITH_EXAMPLES=OFF -DWITH_TESTS=OFF \
	-DWITH_ZLIB=ON \
	-DZLIB_LIBRARY="$STAGE/lib/libz.a" \
	-DZLIB_INCLUDE_DIR="$STAGE/include" \
	-DWITH_LZO=OFF -DWITH_JPEG=OFF -DWITH_PNG=OFF \
	-DWITH_SDL=OFF -DWITH_GTK=OFF -DWITH_LIBSSHTUNNEL=OFF \
	-DWITH_GNUTLS=OFF -DWITH_OPENSSL=OFF -DWITH_SYSTEMD=OFF \
	-DWITH_GCRYPT=OFF -DWITH_FFMPEG=OFF \
	-DWITH_TIGHTVNC_FILETRANSFER=OFF -DWITH_WEBSOCKETS=OFF \
	-DWITH_SASL=OFF -DWITH_XCB=OFF -DWITH_QT=OFF
cmake_build_install "$BUILDS/libvncserver"

printf '%s\n' "building Berkeley SoftFloat 3e and 128-bit Cephes from FEX FEX-2610"
cmake_setup "$ROOT_DIR/dependencies/softfloat" "$BUILDS/softfloat" \
	-DUML_FEX_SOURCE_DIR="$SOURCES/fex"
cmake_build_install "$BUILDS/softfloat"

printf '%s\n' "building FEXCore FEX-2610 with fmt 12.2.1 and xxHash 0.8.3"
# FEX builds fmt, xxHash, and unordered_dense as its own submodules, which the
# verified rows fill in. Only FEXCore and those libraries are built: the
# shared library, frontends, and install rules stay out of the prefix. FEXCore
# takes the default directories of its thunks from CMAKE_INSTALL_PREFIX, which
# nothing installs here, so it is the neutral prefix rather than the final one.
# FEXCore calls the prefix's SoftFloat, which has no preserve_all ABI.
FEX_COMMIT=$(awk -F '\t' '$1 == "fex" { print $5 }' "$MANIFEST")
for submodule in fmt:fmt xxhash:xxhash unordered-dense:unordered_dense; do
	mkdir -p -- "$SOURCES/fex/External/${submodule#*:}"
	cp -R "$SOURCES/${submodule%%:*}/." \
		"$SOURCES/fex/External/${submodule#*:}"
done
cmake_setup "$SOURCES/fex" "$BUILDS/fex" \
	-DFEXCORE_EMBEDDED=ON \
	-DOVERRIDE_VERSION=FEX-2610 \
	-DOVERRIDE_HASH="$FEX_COMMIT" \
	-DPython_EXECUTABLE="$PYTHON" \
	-DTUNE_CPU=none -DTUNE_ARCH=generic \
	-DHAS_CLANG_PRESERVE_ALL=OFF \
	-DENABLE_LTO=OFF -DENABLE_CCACHE=OFF \
	-DENABLE_FEX_ALLOCATOR=OFF -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF \
	-DENABLE_OFFLINE_TELEMETRY=OFF -DENABLE_GDB_SYMBOLS=OFF \
	-DBUILD_TESTING=OFF -DBUILD_FEXCONFIG=OFF -DBUILD_THUNKS=OFF \
	-DCMAKE_INSTALL_PREFIX="$NO_HOST_PREFIX" \
	-DCMAKE_DISABLE_FIND_PACKAGE_fmt=ON \
	-DCMAKE_DISABLE_FIND_PACKAGE_xxhash=ON \
	-DCMAKE_DISABLE_FIND_PACKAGE_unordered_dense=ON
private_env "$CMAKE" --build "$BUILDS/fex" --parallel "$JOBS" \
	--target FEXCore FEXCore_Base JemallocDummy fmt xxhash
for archive in FEXCore/Source/libFEXCore.a FEXCore/Source/libFEXCore_Base.a \
	FEXCore/Source/libJemallocDummy.a External/fmt/libfmt.a \
	External/xxhash/cmake_unofficial/libxxhash.a; do
	$INSTALL -p -m 0644 "$BUILDS/fex/$archive" "$STAGE/lib/"
done
# The public headers are FEX's own install set, the generated IR and
# configuration tables included, and the headers they include.
mkdir -p -- "$STAGE/include/FEXCore" "$STAGE/include/fmt" \
	"$STAGE/include/ankerl"
$RSYNC -a "$SOURCES/fex/FEXCore/include/FEXCore/" "$STAGE/include/FEXCore/"
$RSYNC -a "$BUILDS/fex/include/FEXCore/" "$STAGE/include/FEXCore/"
$RSYNC -a "$SOURCES/fmt/include/fmt/" "$STAGE/include/fmt/"
$RSYNC -a "$SOURCES/unordered-dense/include/ankerl/" "$STAGE/include/ankerl/"
$INSTALL -p -m 0644 "$SOURCES/xxhash/xxhash.h" "$STAGE/include/"

# The emulator imports exact archives and headers rather than upstream CMake
# packages.  Remove unused exports because they can serialize build-only paths
# to staged dependencies (for example an imported zlib archive).
rm -rf -- "$STAGE/lib/cmake" "$STAGE/share/cmake"

# Libtool metadata is not consumed by the imported CMake targets and can retain
# build-only linker search paths.  Keep pkg-config metadata and static archives.
find "$STAGE" -type f -name '*.la' -delete

LICENSE_ROOT="$STAGE/share/uml-dependencies/licenses"
mkdir -p -- "$LICENSE_ROOT"
for dependency in $BUILD_DEPENDENCIES; do
	mkdir -p -- "$LICENSE_ROOT/$dependency"
	for candidate in "$SOURCES/$dependency"/COPYING* \
		"$SOURCES/$dependency"/LICENSE*; do
		[ -f "$candidate" ] || continue
		cp "$candidate" "$LICENSE_ROOT/$dependency/"
	done
done

RECEIPT_DIR="$STAGE/share/uml-dependencies"
mkdir -p -- "$RECEIPT_DIR"
cp "$MANIFEST" "$RECEIPT_DIR/manifest.tsv"
# A patched source is identified by the tree its series produces, which the
# verified preparation above left checked out.
patched_sources=$(awk -F '\t' '
	/^#/ || /^[[:space:]]*$/ || $1 == "name" { next }
	index("," $3 ",", ",macos,") && $7 != "-" { print $1 }
' "$MANIFEST")
printf 'source\ttree\n' > "$RECEIPT_DIR/source-trees.tsv"
for patched in $patched_sources; do
	patched_tree=$(git -C "$SOURCE_ROOT/$patched" rev-parse 'HEAD^{tree}') ||
		fail "could not read the prepared tree of $patched"
	printf '%s\t%s\n' "$patched" "$patched_tree" \
		>> "$RECEIPT_DIR/source-trees.tsv"
done
{
	printf 'schema\t2\n'
	printf 'platform\tmacos\n'
	printf 'architecture\tarm64\n'
	printf 'linkage\tstatic\n'
	printf 'deployment_target\t%s\n' "$DEPLOYMENT_TARGET"
	printf 'sdk_version\t%s\n' "$SDK_VERSION"
	printf 'built_at_utc\t%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
	printf 'compiler\t%s\n' "$($CC --version | sed -n '1p')"
} > "$RECEIPT_DIR/receipt.tsv"

# The exclusive rename never replaces, or moves below, a prefix that a
# concurrent build published first; that winner is verified and reused.
publish_verified "$STAGE" "$PREFIX" verify_prefix "private dependency prefix"
