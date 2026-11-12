#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
VERIFIER="$ROOT_DIR/dependencies/verify-runtime-dependencies.sh"
TEST_PARENT=${TMPDIR:-/tmp}
TEST_ROOT=$(mktemp -d "${TEST_PARENT%/}/uml-runtime-dependencies.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

TEST_NAME='runtime dependency verifier'
. "$(dirname -- "$0")/../support/expect.sh"

"$VERIFIER" --binary /usr/bin/true >/dev/null

cat >"$TEST_ROOT/library.c" <<'EOF'
int uml_runtime_dependency_fixture(void) { return 0; }
EOF
cat >"$TEST_ROOT/main.c" <<'EOF'
int uml_runtime_dependency_fixture(void);
int main(void) { return uml_runtime_dependency_fixture(); }
EOF
xcrun --sdk macosx clang -arch arm64 -dynamiclib \
	-Wl,-install_name,"$TEST_ROOT/libambient.dylib" \
	"$TEST_ROOT/library.c" -o "$TEST_ROOT/libambient.dylib"
xcrun --sdk macosx clang -arch arm64 "$TEST_ROOT/main.c" \
	"$TEST_ROOT/libambient.dylib" -o "$TEST_ROOT/ambient-client"
expect_diagnostic 'non-system dynamic dependency' "$VERIFIER" \
	--binary "$TEST_ROOT/ambient-client"

PREFIX="$TEST_ROOT/prefix"

cat >"$TEST_ROOT/opengl-main.c" <<'EOF'
typedef void *CGLContextObj;
extern CGLContextObj CGLGetCurrentContext(void);
int main(void) { return CGLGetCurrentContext() != 0; }
EOF
xcrun --sdk macosx clang -arch arm64 "$TEST_ROOT/opengl-main.c" \
	-framework OpenGL -o "$TEST_ROOT/opengl-client"
expect_diagnostic 'forbidden graphics dependency' "$VERIFIER" \
	--binary "$TEST_ROOT/opengl-client"

mkdir -p -- "$PREFIX/Frameworks/KosmicKrisp.framework/Headers" \
	"$PREFIX/Frameworks/KosmicKrisp.framework/Modules" \
	"$PREFIX/Frameworks/MesaEGL.framework" \
	"$PREFIX/Frameworks/MesaGallium.framework"
cat >"$TEST_ROOT/kosmickrisp.c" <<'EOF'
int uml_kosmickrisp_fixture(void) { return 29; }
EOF
cat >"$TEST_ROOT/mesa-egl.c" <<'EOF'
int uml_mesa_egl_fixture(void) { return 31; }
EOF
cat >"$TEST_ROOT/mesa-gallium.c" <<'EOF'
int uml_mesa_gallium_fixture(void) { return 37; }
EOF
cat >"$TEST_ROOT/vulkan-framework-main.c" <<'EOF'
#include <stdio.h>
int uml_mesa_egl_fixture(void);
int uml_mesa_gallium_fixture(void);
int uml_kosmickrisp_fixture(void);
int main(void)
{
	puts("Virgl Zink/KosmicKrisp provider: hardware");
	return uml_mesa_egl_fixture() + uml_mesa_gallium_fixture() +
	       uml_kosmickrisp_fixture() == 0;
}
EOF
cat >"$TEST_ROOT/vulkan-framework-main-missing-marker.c" <<'EOF'
int uml_mesa_egl_fixture(void);
int uml_mesa_gallium_fixture(void);
int uml_kosmickrisp_fixture(void);
int main(void)
{
	return uml_mesa_egl_fixture() + uml_mesa_gallium_fixture() +
	       uml_kosmickrisp_fixture() == 0;
}
EOF
for framework in MesaEGL MesaGallium; do
	case "$framework" in
	MesaEGL) source="$TEST_ROOT/mesa-egl.c" ;;
	MesaGallium) source="$TEST_ROOT/mesa-gallium.c" ;;
	esac
	xcrun --sdk macosx clang -arch arm64 -dynamiclib \
		-Wl,-install_name,"@rpath/$framework.framework/$framework" \
		"$source" -o "$PREFIX/Frameworks/$framework.framework/$framework"
	cat >"$PREFIX/Frameworks/$framework.framework/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict><key>CFBundleExecutable</key><string>$framework</string></dict></plist>
EOF
done
xcrun --sdk macosx clang -arch arm64 -dynamiclib \
	-Wl,-install_name,@rpath/KosmicKrisp.framework/KosmicKrisp \
	"$TEST_ROOT/kosmickrisp.c" \
	-o "$PREFIX/Frameworks/KosmicKrisp.framework/KosmicKrisp"
cat >"$PREFIX/Frameworks/KosmicKrisp.framework/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict><key>CFBundleExecutable</key><string>KosmicKrisp</string></dict></plist>
EOF
printf '%s\n' 'fixture header' \
	>"$PREFIX/Frameworks/KosmicKrisp.framework/Headers/KosmicKrisp.h"
printf '%s\n' 'framework module KosmicKrisp {}' \
	>"$PREFIX/Frameworks/KosmicKrisp.framework/Modules/module.modulemap"
VULKAN_RUNTIME="$TEST_ROOT/vulkan-runtime"
mkdir -p -- "$VULKAN_RUNTIME/Frameworks"
for framework in KosmicKrisp MesaEGL MesaGallium; do
	cp -R "$PREFIX/Frameworks/$framework.framework" \
		"$VULKAN_RUNTIME/Frameworks/"
done
xcrun --sdk macosx clang -arch arm64 "$TEST_ROOT/vulkan-framework-main.c" \
	-F"$PREFIX/Frameworks" -framework MesaEGL -framework MesaGallium \
	-framework KosmicKrisp -Wl,-rpath,@executable_path/Frameworks \
	-o "$VULKAN_RUNTIME/apple-gpu-vulkan-client"
xcrun --sdk macosx clang -arch arm64 \
	"$TEST_ROOT/vulkan-framework-main-missing-marker.c" \
	-F"$PREFIX/Frameworks" -framework MesaEGL -framework MesaGallium \
	-framework KosmicKrisp -Wl,-rpath,@executable_path/Frameworks \
	-o "$VULKAN_RUNTIME/apple-gpu-vulkan-client-missing-marker"
expect_diagnostic 'unauthenticated Apple GPU dependency' "$VERIFIER" \
	--binary "$VULKAN_RUNTIME/apple-gpu-vulkan-client"
verify_vulkan_runtime()
{
	"$VERIFIER" --binary "$VULKAN_RUNTIME/apple-gpu-vulkan-client" \
		--apple-gpu-prefix "$PREFIX" \
		--framework-directory "$VULKAN_RUNTIME/Frameworks"
}
verify_vulkan_runtime >/dev/null

# An exported CDPATH neither redirects a relative prefix to a decoy with
# another payload nor prints into the payload listings the verifier compares.
CDPATH_DECOY="$TEST_ROOT/cdpath-decoy"
mkdir -p -- "$CDPATH_DECOY/prefix/Frameworks"
for framework in KosmicKrisp MesaEGL MesaGallium; do
	cp -R "$PREFIX/Frameworks/$framework.framework" \
		"$CDPATH_DECOY/prefix/Frameworks/"
	: >"$CDPATH_DECOY/prefix/Frameworks/$framework.framework/decoy"
done
CDPATH_OUTPUT=$(
	CDPATH= cd -- "$TEST_ROOT" &&
		CDPATH="$CDPATH_DECOY" "$VERIFIER" \
		--binary vulkan-runtime/apple-gpu-vulkan-client \
		--apple-gpu-prefix prefix \
		--framework-directory vulkan-runtime/Frameworks
) || fail "an exported CDPATH redirected a relative Apple GPU prefix"
[ "$CDPATH_OUTPUT" = \
	'verified production Apple GPU runtime closure: vulkan-runtime/apple-gpu-vulkan-client' ] ||
	fail "an exported CDPATH changed the verifier's output: $CDPATH_OUTPUT"

expect_diagnostic 'host GL provider marker' "$VERIFIER" \
	--binary "$VULKAN_RUNTIME/apple-gpu-vulkan-client-missing-marker" \
	--apple-gpu-prefix "$PREFIX" \
	--framework-directory "$VULKAN_RUNTIME/Frameworks"

mkdir -p -- "$VULKAN_RUNTIME/duplicate"
cp -R "$PREFIX/Frameworks/MesaEGL.framework" "$VULKAN_RUNTIME/duplicate/"
expect_diagnostic 'exactly one MesaEGL.framework copy' "$VERIFIER" \
	--binary "$VULKAN_RUNTIME/apple-gpu-vulkan-client" \
	--apple-gpu-prefix "$PREFIX" \
	--framework-directory "$VULKAN_RUNTIME/Frameworks"
rm -rf -- "$VULKAN_RUNTIME/duplicate"

touch "$VULKAN_RUNTIME/Frameworks/MesaGallium.framework/unexpected"
expect_diagnostic 'payload is not exact' "$VERIFIER" \
	--binary "$VULKAN_RUNTIME/apple-gpu-vulkan-client" \
	--apple-gpu-prefix "$PREFIX" \
	--framework-directory "$VULKAN_RUNTIME/Frameworks"
rm -- "$VULKAN_RUNTIME/Frameworks/MesaGallium.framework/unexpected"
verify_vulkan_runtime >/dev/null

printf '\0' >>"$VULKAN_RUNTIME/Frameworks/KosmicKrisp.framework/KosmicKrisp"
expect_diagnostic 'does not match the authenticated prefix' "$VERIFIER" \
	--binary "$VULKAN_RUNTIME/apple-gpu-vulkan-client" \
	--apple-gpu-prefix "$PREFIX" \
	--framework-directory "$VULKAN_RUNTIME/Frameworks"

printf '%s\n' 'runtime dependency closure fixture passed'
