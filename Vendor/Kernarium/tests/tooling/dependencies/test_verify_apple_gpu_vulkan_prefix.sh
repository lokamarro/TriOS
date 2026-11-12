#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
VERIFIER="$ROOT_DIR/dependencies/cmake/VerifyAppleGpuPrefix.cmake"
BUILDER="$ROOT_DIR/dependencies/build-apple-gpu.sh"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-apple-gpu-vulkan-prefix.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM
BASE_PREFIX="$TEST_ROOT/prefix"

TEST_NAME=test-verify-apple-gpu-vulkan-prefix.sh
. "$(dirname -- "$0")/../support/expect.sh"

verify()
{
	platform=${2:-macos}
	cmake -DPREFIX="$1" -DSOURCE_ROOT="$ROOT_DIR" \
		-DPLATFORM="$platform" -P "$VERIFIER"
}

copy_fixture()
{
	cp -R "$BASE_PREFIX" "$1"
}

# make_framework PREFIX FRAMEWORK SOURCE [CLANG-ARGUMENT...]: build a fixture
# framework into PREFIX. INSTALL_NAME and MINIMUM_OS override its install name
# and Info.plist deployment target.
make_framework()
{
	prefix=$1
	framework=$2
	source=$3
	shift 3
	binary="$prefix/Frameworks/$framework.framework/$framework"
	mkdir -p "$(dirname -- "$binary")"
	xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=27.0 \
		-dynamiclib "$source" "$@" \
		-Wl,-install_name,"${INSTALL_NAME:-@rpath/$framework.framework/$framework}" \
		-o "$binary"
	cat >"$(dirname -- "$binary")/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>$framework</string>
<key>CFBundleIdentifier</key><string>org.linux-um-darwin.fixture.$framework</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>LSMinimumSystemVersion</key><string>${MINIMUM_OS:-27.0}</string>
</dict></plist>
EOF
}

# expect_framework_rejection DIAGNOSTIC FRAMEWORK SOURCE [CLANG-ARGUMENT...]:
# a copy of the fixture whose FRAMEWORK is rebuilt from SOURCE must be
# rejected with DIAGNOSTIC.
FRAMEWORK_CASE=0
expect_framework_rejection()
{
	diagnostic=$1
	shift
	FRAMEWORK_CASE=$((FRAMEWORK_CASE + 1))
	CASE_PREFIX="$TEST_ROOT/framework-$FRAMEWORK_CASE"
	copy_fixture "$CASE_PREFIX"
	make_framework "$CASE_PREFIX" "$@"
	expect_cmake_diagnostic "$diagnostic" verify "$CASE_PREFIX"
}

command -v xcrun >/dev/null 2>&1 || fail "xcrun is required"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"
mkdir -p "$BASE_PREFIX/Frameworks" "$BASE_PREFIX/lib" \
	"$BASE_PREFIX/share/uml-dependencies" \
	"$BASE_PREFIX/include/EGL" "$BASE_PREFIX/include/GL" \
	"$BASE_PREFIX/include/KHR" "$BASE_PREFIX/include/epoxy" \
	"$BASE_PREFIX/include/virgl" \
	"$BASE_PREFIX/include/vulkan" "$BASE_PREFIX/include/vk_video"

cat >"$TEST_ROOT/fixture.c" <<'EOF'
int uml_apple_gpu_fixture(void) { return 0; }
EOF
cat >"$TEST_ROOT/mesa-egl.c" <<'EOF'
extern void *uml_mesa_gallium_fixture(void);
void *eglGetPlatformDisplay(unsigned platform, void *display, const long *attributes)
{
    (void)platform;
    (void)display;
    (void)attributes;
    return uml_mesa_gallium_fixture();
}
EOF
cat >"$TEST_ROOT/mesa-gallium.c" <<'EOF'
extern void *vkGetInstanceProcAddr(void *, const char *);
void *uml_mesa_gallium_fixture(void)
{
    return vkGetInstanceProcAddr(0, "vkCreateInstance");
}
EOF
cat >"$TEST_ROOT/virgl.c" <<'EOF'
extern void *vkGetInstanceProcAddr(void *, const char *);
void *uml_direct_vulkan_provider(void)
{
    return vkGetInstanceProcAddr(0, "vkCreateInstance");
}
EOF
cat >"$TEST_ROOT/kosmickrisp.c" <<'EOF'
void *vkGetInstanceProcAddr(void *instance, const char *name)
{
    (void)instance;
    (void)name;
    return 0;
}
EOF
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=27.0 \
	-c "$TEST_ROOT/fixture.c" -o "$TEST_ROOT/fixture.o"
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=27.0 \
	-c "$TEST_ROOT/virgl.c" -o "$TEST_ROOT/virgl.o"
xcrun ar rcs "$BASE_PREFIX/lib/libepoxy.a" "$TEST_ROOT/fixture.o"
xcrun ar rcs "$BASE_PREFIX/lib/libvirglrenderer.a" "$TEST_ROOT/virgl.o"
make_framework "$BASE_PREFIX" KosmicKrisp "$TEST_ROOT/kosmickrisp.c"
make_framework "$BASE_PREFIX" MesaGallium "$TEST_ROOT/mesa-gallium.c" \
	"$BASE_PREFIX/Frameworks/KosmicKrisp.framework/KosmicKrisp"
make_framework "$BASE_PREFIX" MesaEGL "$TEST_ROOT/mesa-egl.c" \
	"$BASE_PREFIX/Frameworks/MesaGallium.framework/MesaGallium"

for header in \
	EGL/egl.h \
	epoxy/egl.h \
	virgl/virglrenderer.h \
	virgl/virgl_hw.h \
	vulkan/vulkan.h; do
	printf '%s\n' fixture >"$BASE_PREFIX/include/$header"
done
KOSMICKRISP_FRAMEWORK="$BASE_PREFIX/Frameworks/KosmicKrisp.framework"
mkdir -p "$KOSMICKRISP_FRAMEWORK/Headers" \
	"$KOSMICKRISP_FRAMEWORK/Modules"
printf '%s\n' '#include <vulkan/vulkan.h>' \
	>"$KOSMICKRISP_FRAMEWORK/Headers/KosmicKrisp.h"
cat >"$KOSMICKRISP_FRAMEWORK/Modules/module.modulemap" <<'EOF'
framework module KosmicKrisp {
    umbrella header "KosmicKrisp.h"
    export *
}
EOF

cp "$ROOT_DIR/dependencies/manifest.tsv" \
	"$BASE_PREFIX/share/uml-dependencies/manifest.tsv"
cp "$ROOT_DIR/dependencies/platforms/macos-gpu-vulkan-arm64.tsv" \
	"$BASE_PREFIX/share/uml-dependencies/macos-gpu-vulkan-arm64.tsv"
series_tree()
{
	awk -F '\t' '$1 == "tree" { print $2 }' "$1/series.tsv"
}
{
	printf 'source\ttree\n'
	for component in libepoxy virglrenderer mesa; do
		printf '%s\t%s\n' "$component" \
			"$(series_tree "$ROOT_DIR/dependencies/patches/$component")"
	done
	printf 'venus-protocol\t%s\n' \
		"$(series_tree "$ROOT_DIR/tools/rootfs/vulkan/patches/venus-protocol")"
} >"$BASE_PREFIX/share/uml-dependencies/source-trees.tsv"
cat >"$BASE_PREFIX/share/uml-dependencies/build-receipt.tsv" <<EOF
field	value
format	2
product	apple-gpu-vulkan-experiment
platform	macos
sdk_name	macosx
sdk_version	fixture
sdk_path	/fixture/MacOSX.sdk
compiler	fixture clang
built_at_utc	2026-08-23T00:00:00Z
venus_protocol_generated_sha256	0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
EOF

verify "$BASE_PREFIX" >/dev/null

# Symbols are optional for older host prefixes, but a prefix that supplies
# them must carry one matching dSYM for every dynamic framework.
CASE_PREFIX="$TEST_ROOT/with-symbols"
copy_fixture "$CASE_PREFIX"
mkdir -p "$CASE_PREFIX/dSYMs"
for framework in KosmicKrisp MesaGallium MesaEGL; do
	case $framework in
	KosmicKrisp) source=kosmickrisp; dependency= ;;
	MesaGallium) source=mesa-gallium; dependency=KosmicKrisp ;;
	MesaEGL) source=mesa-egl; dependency=MesaGallium ;;
	esac
	xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=27.0 -g \
		-c "$TEST_ROOT/$source.c" -o "$TEST_ROOT/$framework.debug.o"
	set --
	if [ -n "$dependency" ]; then
		set -- "$CASE_PREFIX/Frameworks/$dependency.framework/$dependency"
	fi
	make_framework "$CASE_PREFIX" "$framework" "$TEST_ROOT/$framework.debug.o" "$@"
	xcrun dsymutil "$CASE_PREFIX/Frameworks/$framework.framework/$framework" \
		-o "$CASE_PREFIX/dSYMs/$framework.framework.dSYM"
done
verify "$CASE_PREFIX" >/dev/null
# A framework linked from objects built without -g still gets a dSYM from
# dsymutil, which exits 0 and keeps the UUID: the symbols are empty.
cp -R "$CASE_PREFIX" "$TEST_ROOT/empty-symbols"
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=27.0 \
	-c "$TEST_ROOT/kosmickrisp.c" -o "$TEST_ROOT/KosmicKrisp.nodebug.o"
make_framework "$TEST_ROOT/empty-symbols" KosmicKrisp \
	"$TEST_ROOT/KosmicKrisp.nodebug.o"
rm -rf "$TEST_ROOT/empty-symbols/dSYMs/KosmicKrisp.framework.dSYM"
xcrun dsymutil \
	"$TEST_ROOT/empty-symbols/Frameworks/KosmicKrisp.framework/KosmicKrisp" \
	-o "$TEST_ROOT/empty-symbols/dSYMs/KosmicKrisp.framework.dSYM" \
	>/dev/null 2>&1
KOSMICKRISP_BINARY=$TEST_ROOT/empty-symbols/Frameworks/KosmicKrisp.framework/KosmicKrisp
KOSMICKRISP_SYMBOLS=$TEST_ROOT/empty-symbols/dSYMs/KosmicKrisp.framework.dSYM
[ "$(xcrun dwarfdump --uuid "$KOSMICKRISP_BINARY" | awk '{ print $2 }')" = \
	"$(xcrun dwarfdump --uuid "$KOSMICKRISP_SYMBOLS" | awk '{ print $2 }')" ] ||
	fail "the empty-symbols fixture lost its matching UUID"
expect_cmake_diagnostic "KosmicKrisp.framework debug symbols carry no debug information" \
	verify "$TEST_ROOT/empty-symbols"
cp -R "$CASE_PREFIX" "$TEST_ROOT/mismatched-symbols"
rm -rf "$TEST_ROOT/mismatched-symbols/dSYMs/MesaEGL.framework.dSYM"
cp -R "$CASE_PREFIX/dSYMs/MesaGallium.framework.dSYM" \
	"$TEST_ROOT/mismatched-symbols/dSYMs/MesaEGL.framework.dSYM"
expect_cmake_diagnostic "MesaEGL.framework debug symbol UUID does not match" \
	verify "$TEST_ROOT/mismatched-symbols"
rm -rf "$CASE_PREFIX/dSYMs/KosmicKrisp.framework.dSYM"
expect_cmake_diagnostic "Missing debug symbol UUID" verify "$CASE_PREFIX"

expect_failure "wrong platform" verify "$BASE_PREFIX" ios-simulator

# A prefix is current exactly while each source tree matches its series.
for component in virglrenderer mesa venus-protocol; do
	CASE_PREFIX="$TEST_ROOT/stale-$component"
	copy_fixture "$CASE_PREFIX"
	awk -F '\t' -v OFS='\t' -v component="$component" \
		'$1 == component { $2 = "0000000000000000000000000000000000000000" }
		{ print }' "$BASE_PREFIX/share/uml-dependencies/source-trees.tsv" \
		>"$CASE_PREFIX/share/uml-dependencies/source-trees.tsv"
	expect_failure "stale $component source tree" verify "$CASE_PREFIX"
done

CASE_PREFIX="$TEST_ROOT/missing-source-tree"
copy_fixture "$CASE_PREFIX"
grep -v '^libepoxy' "$BASE_PREFIX/share/uml-dependencies/source-trees.tsv" \
	>"$CASE_PREFIX/share/uml-dependencies/source-trees.tsv"
expect_failure "missing source tree" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/extra-source-tree"
copy_fixture "$CASE_PREFIX"
printf 'vulkan-headers\t%s\n' 0000000000000000000000000000000000000000 \
	>>"$CASE_PREFIX/share/uml-dependencies/source-trees.tsv"
expect_failure "unexpected source tree" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/installed-patches"
copy_fixture "$CASE_PREFIX"
mkdir -p "$CASE_PREFIX/share/uml-dependencies/patches"
cp -R "$ROOT_DIR/dependencies/patches/mesa" \
	"$CASE_PREFIX/share/uml-dependencies/patches/"
expect_failure "installed patch files" verify "$CASE_PREFIX"

# The installed platform contract must be the checked-in one.
CASE_PREFIX="$TEST_ROOT/edited-contract"
copy_fixture "$CASE_PREFIX"
sed 's/^helper_process\t.*/helper_process\tenabled/' \
	"$BASE_PREFIX/share/uml-dependencies/macos-gpu-vulkan-arm64.tsv" \
	>"$CASE_PREFIX/share/uml-dependencies/macos-gpu-vulkan-arm64.tsv"
expect_failure "edited platform contract" verify "$CASE_PREFIX"
grep -q 'provenance does not match' "$TEST_ROOT/rejected-$FAILURE_NUMBER.err" ||
	fail "an edited platform contract was not reported as a provenance mismatch"

# Each receipt case rewrites the receipt of RECEIPT_PREFIX. receipt_case
# [AWK-PROGRAM] makes it the base receipt filtered through the awk program,
# over tab-separated fields.
RECEIPT_PREFIX="$TEST_ROOT/receipt-cases"
copy_fixture "$RECEIPT_PREFIX"
receipt_case()
{
	awk -F '\t' -v OFS='\t' "${1:-1}" \
		"$BASE_PREFIX/share/uml-dependencies/build-receipt.tsv" \
		>"$RECEIPT_PREFIX/share/uml-dependencies/build-receipt.tsv"
}

receipt_case '{ print } $1 == "sdk_name" { print }'
expect_cmake_diagnostic \
	"Unexpected or duplicate Apple GPU receipt field: sdk_name" \
	verify "$RECEIPT_PREFIX"
receipt_case '{ print } END { print "network_input", "https://invalid.example/source" }'
expect_cmake_diagnostic \
	"Unexpected or duplicate Apple GPU receipt field: network_input" \
	verify "$RECEIPT_PREFIX"
receipt_case '$1 != "compiler"'
expect_cmake_diagnostic "Apple GPU build receipt is incomplete" \
	verify "$RECEIPT_PREFIX"
receipt_case '$1 == "compiler" { $3 = "extra" } { print }'
expect_cmake_diagnostic "Malformed Apple GPU build receipt row" \
	verify "$RECEIPT_PREFIX"
receipt_case '$1 == "compiler" { $2 = "clang;extra" } { print }'
expect_cmake_diagnostic "Malformed Apple GPU build receipt row" \
	verify "$RECEIPT_PREFIX"
receipt_case 'NR == 3 { print "" } { print }'
verify "$RECEIPT_PREFIX" >/dev/null || fail "a blank receipt row was rejected"
receipt_case '$1 == "format" { $2 = "3" } { print }'
expect_cmake_diagnostic "format is 3, expected 2" verify "$RECEIPT_PREFIX"
receipt_case '$1 == "product" { $2 = "other" } { print }'
expect_cmake_diagnostic \
	"product is other, expected apple-gpu-vulkan-experiment" \
	verify "$RECEIPT_PREFIX"
receipt_case '$1 == "platform" { $2 = "ios" } { print }'
expect_cmake_diagnostic "platform is ios, expected macos" \
	verify "$RECEIPT_PREFIX"

# The protocol identity is exactly 64 lowercase hexadecimal digits.
for identity in unknown \
	0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde \
	0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0 \
	0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef; do
	receipt_case "\$1 == \"venus_protocol_generated_sha256\" { \$2 = \"$identity\" } { print }"
	expect_cmake_diagnostic "Invalid protocol build identity" \
		verify "$RECEIPT_PREFIX"
done

# No field is empty. The SDK, compiler, and build-time fields say so.
for field in sdk_name sdk_version sdk_path compiler built_at_utc; do
	receipt_case "\$1 == \"$field\" { \$2 = \"\" } { print }"
	expect_cmake_diagnostic "Apple GPU build receipt has invalid $field" \
		verify "$RECEIPT_PREFIX"
done
for field in format product platform venus_protocol_generated_sha256; do
	receipt_case "\$1 == \"$field\" { \$2 = \"\" } { print }"
	expect_failure "empty receipt $field" verify "$RECEIPT_PREFIX"
done

CASE_PREFIX="$TEST_ROOT/duplicate-provider"
copy_fixture "$CASE_PREFIX"
cp -R "$CASE_PREFIX/Frameworks/KosmicKrisp.framework" \
	"$CASE_PREFIX/Frameworks/Vulkan.framework"
expect_failure "duplicate Vulkan provider" verify "$CASE_PREFIX"

# Each framework has its own dependency allowlist. The GL pair may use
# libexpat, Foundation, and its two Mesa edges; KosmicKrisp may use only Metal,
# QuartzCore, and the base system libraries.
GALLIUM="$BASE_PREFIX/Frameworks/MesaGallium.framework/MesaGallium"
KOSMICKRISP="$BASE_PREFIX/Frameworks/KosmicKrisp.framework/KosmicKrisp"
CASE_PREFIX="$TEST_ROOT/gl-allowlist"
copy_fixture "$CASE_PREFIX"
make_framework "$CASE_PREFIX" MesaEGL "$TEST_ROOT/mesa-egl.c" "$GALLIUM" \
	-lexpat -framework Foundation
verify "$CASE_PREFIX" >/dev/null ||
	fail "a GL framework using its allowed dependencies was rejected"

expect_framework_rejection \
	"KosmicKrisp.framework dependency: /usr/lib/libexpat.1.dylib" \
	KosmicKrisp "$TEST_ROOT/kosmickrisp.c" -lexpat
expect_framework_rejection \
	"KosmicKrisp.framework dependency: /System/Library/Frameworks/Foundation.framework" \
	KosmicKrisp "$TEST_ROOT/kosmickrisp.c" -framework Foundation
expect_framework_rejection \
	"MesaGallium.framework dependency: @rpath/MesaEGL.framework/MesaEGL" \
	MesaGallium "$TEST_ROOT/mesa-gallium.c" "$KOSMICKRISP" \
	"$BASE_PREFIX/Frameworks/MesaEGL.framework/MesaEGL"
expect_framework_rejection \
	"MesaEGL.framework dependency: @rpath/KosmicKrisp.framework/KosmicKrisp" \
	MesaEGL "$TEST_ROOT/mesa-egl.c" "$GALLIUM" "$KOSMICKRISP"
expect_framework_rejection \
	"MesaEGL.framework dependency: /System/Library/Frameworks/OpenGL.framework" \
	MesaEGL "$TEST_ROOT/mesa-egl.c" "$GALLIUM" -framework OpenGL

# An allowed system framework directory admits any path below it, so no
# framework may name a GL or Vulkan framework or loader anywhere in its closure.
printf '%s\n' 'int uml_apple_gpu_stub(void) { return 0; }' >"$TEST_ROOT/stub.c"
for nested in OpenGL Vulkan; do
	xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=27.0 \
		-dynamiclib "$TEST_ROOT/stub.c" \
		-Wl,-install_name,/System/Library/Frameworks/Metal.framework/Frameworks/$nested.framework/$nested \
		-o "$TEST_ROOT/nested-$nested.dylib"
done
expect_framework_rejection \
	"KosmicKrisp.framework contains a forbidden GL/Vulkan dependency" \
	KosmicKrisp "$TEST_ROOT/kosmickrisp.c" "$TEST_ROOT/nested-OpenGL.dylib"
expect_framework_rejection \
	"MesaEGL.framework contains a forbidden GL/Vulkan dependency" \
	MesaEGL "$TEST_ROOT/mesa-egl.c" "$GALLIUM" "$TEST_ROOT/nested-Vulkan.dylib"

# A dependency is its whole path, which may contain spaces, not its first
# word.
for spaced in "@rpath/KosmicKrisp.framework/KosmicKrisp extra" \
	"@rpath/MesaGallium.framework/MesaGallium extra"; do
	xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=27.0 \
		-dynamiclib "$TEST_ROOT/stub.c" -Wl,-install_name,"$spaced" \
		-o "$TEST_ROOT/spaced-${spaced##*/}.dylib"
done
expect_framework_rejection \
	"KosmicKrisp.framework dependency: @rpath/KosmicKrisp.framework/KosmicKrisp extra" \
	KosmicKrisp "$TEST_ROOT/kosmickrisp.c" \
	"$TEST_ROOT/spaced-KosmicKrisp extra.dylib"
expect_framework_rejection \
	"MesaEGL.framework dependency: @rpath/MesaGallium.framework/MesaGallium extra" \
	MesaEGL "$TEST_ROOT/mesa-egl.c" "$GALLIUM" \
	"$TEST_ROOT/spaced-MesaGallium extra.dylib"

# Install names, load commands, and deployment targets.
expect_framework_rejection "KosmicKrisp.framework has an invalid" \
	KosmicKrisp "$TEST_ROOT/kosmickrisp.c" -Wl,-rpath,@loader_path
expect_framework_rejection "MesaEGL.framework has an invalid" \
	MesaEGL "$TEST_ROOT/mesa-egl.c" "$GALLIUM" -Wl,-rpath,@loader_path
# sh may keep an assignment that prefixes a function call, so these are set
# and unset explicitly.
INSTALL_NAME=@rpath/Other.framework/Other
expect_framework_rejection \
	"KosmicKrisp.framework has an invalid install name" \
	KosmicKrisp "$TEST_ROOT/kosmickrisp.c"
expect_framework_rejection \
	"MesaGallium.framework has an invalid install name" \
	MesaGallium "$TEST_ROOT/mesa-gallium.c" "$KOSMICKRISP"
# The install name is the whole name, not a prefix of it.
INSTALL_NAME="@rpath/KosmicKrisp.framework/KosmicKrisp extra"
expect_framework_rejection \
	"KosmicKrisp.framework has an invalid install name" \
	KosmicKrisp "$TEST_ROOT/kosmickrisp.c"
INSTALL_NAME="@rpath/MesaGallium.framework/MesaGallium extra"
expect_framework_rejection \
	"MesaGallium.framework has an invalid install name" \
	MesaGallium "$TEST_ROOT/mesa-gallium.c" "$KOSMICKRISP"
unset INSTALL_NAME
MINIMUM_OS=26.0
expect_framework_rejection \
	"KosmicKrisp.framework has an invalid deployment target" \
	KosmicKrisp "$TEST_ROOT/kosmickrisp.c"
expect_framework_rejection \
	"MesaGallium.framework has an invalid deployment target" \
	MesaGallium "$TEST_ROOT/mesa-gallium.c" "$KOSMICKRISP"
unset MINIMUM_OS

# Symbols and payloads.
expect_framework_rejection \
	"KosmicKrisp.framework does not export vkGetInstanceProcAddr" \
	KosmicKrisp "$TEST_ROOT/fixture.c"

# Venus calls KosmicKrisp's entry point directly, so neither the renderer nor
# KosmicKrisp loads code at run time. libepoxy may: its Apple topic opens only
# the packaged MesaEGL.framework.
cat >"$TEST_ROOT/kosmickrisp-loader.c" <<'EOF'
#include <dlfcn.h>
void *vkGetInstanceProcAddr(void *instance, const char *name)
{
    (void)instance;
    return dlsym(dlopen("libvulkan.dylib", RTLD_NOW), name);
}
EOF
expect_framework_rejection \
	"KosmicKrisp.framework contains a dynamic-loader fallback" \
	KosmicKrisp "$TEST_ROOT/kosmickrisp-loader.c"
cat >"$TEST_ROOT/virgl-loader.c" <<'EOF'
#include <dlfcn.h>
extern void *vkGetInstanceProcAddr(void *, const char *);
void *uml_direct_vulkan_provider(void)
{
    (void)dlsym(dlopen("libvulkan.dylib", RTLD_NOW), "vkCreateInstance");
    return vkGetInstanceProcAddr(0, "vkCreateInstance");
}
EOF
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=27.0 \
	-c "$TEST_ROOT/virgl-loader.c" -o "$TEST_ROOT/virgl-loader.o"
CASE_PREFIX="$TEST_ROOT/renderer-loader"
copy_fixture "$CASE_PREFIX"
rm "$CASE_PREFIX/lib/libvirglrenderer.a"
xcrun ar rcs "$CASE_PREFIX/lib/libvirglrenderer.a" "$TEST_ROOT/virgl-loader.o"
expect_cmake_diagnostic "libvirglrenderer.a contains a dynamic-loader fallback" \
	verify "$CASE_PREFIX"
cat >"$TEST_ROOT/epoxy-loader.c" <<'EOF'
#include <dlfcn.h>
void *uml_epoxy_fixture(const char *name)
{
    return dlsym(dlopen("@rpath/MesaEGL.framework/MesaEGL", RTLD_NOW), name);
}
EOF
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=27.0 \
	-c "$TEST_ROOT/epoxy-loader.c" -o "$TEST_ROOT/epoxy-loader.o"
CASE_PREFIX="$TEST_ROOT/epoxy-loader"
copy_fixture "$CASE_PREFIX"
rm "$CASE_PREFIX/lib/libepoxy.a"
xcrun ar rcs "$CASE_PREFIX/lib/libepoxy.a" "$TEST_ROOT/epoxy-loader.o"
verify "$CASE_PREFIX" >/dev/null ||
	fail "libepoxy opening its packaged EGL provider was rejected"
expect_framework_rejection \
	"MesaEGL.framework does not export the EGL platform bootstrap" \
	MesaEGL "$TEST_ROOT/fixture.c"
cat >"$TEST_ROOT/mesa-gallium-provider.c" <<'EOF'
extern void *vkGetInstanceProcAddr(void *, const char *);
extern int vkCreateInstance(const void *, const void *, void **);
void *uml_mesa_gallium_fixture(void)
{
    (void)vkCreateInstance(0, 0, 0);
    return vkGetInstanceProcAddr(0, "vkCreateInstance");
}
EOF
expect_framework_rejection \
	"MesaGallium.framework contains a forbidden provider symbol" \
	MesaGallium "$TEST_ROOT/mesa-gallium-provider.c" "$KOSMICKRISP" \
	-Wl,-U,_vkCreateInstance

# The software rasterizer is the Simulator's alone: a Zink build carries no
# software Gallium screen.
cat >"$TEST_ROOT/mesa-gallium-software.c" <<'EOF'
extern void *vkGetInstanceProcAddr(void *, const char *);
void *softpipe_create_screen(void)
{
    return vkGetInstanceProcAddr(0, "vkCreateInstance");
}
void *uml_mesa_gallium_fixture(void)
{
    return softpipe_create_screen();
}
EOF
expect_framework_rejection \
	"MesaGallium.framework carries a software Gallium screen" \
	MesaGallium "$TEST_ROOT/mesa-gallium-software.c" "$KOSMICKRISP"
# Nor does it carry LLVM, which llvmpipe JITs through.
cat >"$TEST_ROOT/mesa-gallium-llvm.c" <<'EOF'
extern void *vkGetInstanceProcAddr(void *, const char *);
void *LLVMLinkInMCJIT(void)
{
    return vkGetInstanceProcAddr(0, "vkCreateInstance");
}
void *uml_mesa_gallium_fixture(void)
{
    return LLVMLinkInMCJIT();
}
EOF
expect_framework_rejection \
	"MesaGallium.framework carries LLVM" \
	MesaGallium "$TEST_ROOT/mesa-gallium-llvm.c" "$KOSMICKRISP"

CASE_PREFIX="$TEST_ROOT/gl-payload"
copy_fixture "$CASE_PREFIX"
mkdir -p "$CASE_PREFIX/Frameworks/MesaEGL.framework/Headers"
printf '%s\n' fixture >"$CASE_PREFIX/Frameworks/MesaEGL.framework/Headers/EGL.h"
expect_cmake_diagnostic "Unexpected MesaEGL.framework payload" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/module-map"
copy_fixture "$CASE_PREFIX"
printf '%s\n' 'module Other {}' \
	>"$CASE_PREFIX/Frameworks/KosmicKrisp.framework/Modules/module.modulemap"
expect_cmake_diagnostic "KosmicKrisp.framework has an invalid module map" \
	verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/invalid-existing-prefix"
copy_fixture "$CASE_PREFIX"
printf '%s\n' ambient >"$CASE_PREFIX/lib/unexpected"
BEFORE=$(find "$CASE_PREFIX" -type f -exec shasum -a 256 {} \; | LC_ALL=C sort)
if "$BUILDER" --platform macos --prefix "$CASE_PREFIX" --verify-only \
	>"$TEST_ROOT/builder-rejected.out" 2>"$TEST_ROOT/builder-rejected.err"; then
	fail "invalid existing Vulkan prefix was incorrectly accepted"
fi
AFTER=$(find "$CASE_PREFIX" -type f -exec shasum -a 256 {} \; | LC_ALL=C sort)
[ "$AFTER" = "$BEFORE" ] || fail "invalid existing Vulkan prefix was modified"

printf '%s\n' "Experimental Apple GPU Vulkan prefix tests passed"
