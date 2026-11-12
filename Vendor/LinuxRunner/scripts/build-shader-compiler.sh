#!/bin/bash
# Pinned Khronos compiler libraries, statically linked; no guest CPU JIT.
set -euo pipefail
cd "$(dirname "$0")/.."
platform=${1:-macosx}
case "$platform" in macosx|iphoneos|iphonesimulator) ;; *) echo 'Unsupported shader compiler platform'; exit 2;; esac
compiler=${LINUXRUNNER_CMAKE:-cmake}
out="$PWD/build/shader-compiler/$platform"
mkdir -p "$out"
sdk=$(/usr/bin/xcrun --sdk "$platform" --show-sdk-path)
arch=${2:-arm64};arch=${arch// /;}
case "$arch" in arm64|x86_64|arm64\;x86_64|x86_64\;arm64) ;; *) echo "Unsupported build architectures: $arch"; exit 2;; esac
flags=(-G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_SYSROOT=$sdk" "-DCMAKE_OSX_ARCHITECTURES=$arch")
if [[ $platform != macosx ]]; then flags+=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0); fi
"$compiler" -S deps/glslang -B "$out/glslang" "${flags[@]}" -DENABLE_OPT=OFF -DENABLE_GLSLANG_BINARIES=OFF -DBUILD_TESTING=OFF -DENABLE_HLSL=OFF -DENABLE_PCH=OFF
"$compiler" --build "$out/glslang" -j 6
"$compiler" -S deps/spirv-cross -B "$out/cross" "${flags[@]}" -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_C_API=OFF -DSPIRV_CROSS_ENABLE_HLSL=OFF -DSPIRV_CROSS_ENABLE_CPP=OFF
"$compiler" --build "$out/cross" -j 6
# Compile a build-local correction; do not modify the pinned submodule.
python3 scripts/prepare-spirv-msl.py deps/spirv-cross/spirv_msl.cpp "$out/spirv_msl.cpp"
mslflags=(-std=c++11 -O3 -DNDEBUG -isysroot "$sdk" -Ideps/spirv-cross)
IFS=';' read -ra architectures <<< "$arch"
for architecture in "${architectures[@]}"; do mslflags+=(-arch "$architecture"); done
case "$platform" in iphoneos) mslflags+=(-miphoneos-version-min=15.0);; iphonesimulator) mslflags+=(-mios-simulator-version-min=15.0);; esac
/usr/bin/xcrun --sdk "$platform" clang++ "${mslflags[@]}" -c "$out/spirv_msl.cpp" -o "$out/spirv_msl.o"
python3 scripts/prepare-glslang-builtins.py deps/glslang/glslang/MachineIndependent/Initialize.cpp "$out/Initialize.cpp"
builtinflags=("${mslflags[@]}" -std=c++17 -fno-rtti -fno-exceptions -DENABLE_OPT=0 -DENABLE_SPIRV -DGLSLANG_OSINCLUDE_UNIX -Ideps/glslang -I"$out/glslang/include" -iquote deps/glslang/glslang/MachineIndependent)
/usr/bin/xcrun --sdk "$platform" clang++ "${builtinflags[@]}" -c "$out/Initialize.cpp" -o "$out/Initialize.o"
cp "$out/glslang/glslang/libglslang.a" "$out/libglslang-patched.a"
/usr/bin/ar -d "$out/libglslang-patched.a" Initialize.cpp.o
/usr/bin/libtool -static -o "$out/liblrshader.a" "$out/libglslang-patched.a" "$out/Initialize.o" "$out/glslang/glslang/libglslang-default-resource-limits.a" "$out/glslang/SPIRV/libSPIRV.a" "$out/spirv_msl.o" "$out/cross/libspirv-cross-glsl.a" "$out/cross/libspirv-cross-core.a"
