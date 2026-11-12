#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root=${UML_XCFRAMEWORK_BUILD_DIR:-"$project_root/.build/xcframework"}
output=${UML_XCFRAMEWORK_OUTPUT:-"$build_root/UMLEmulator.xcframework"}
toolchain="$project_root/cmake/toolchains/Apple.cmake"
dependency_prefix=${UML_DEPENDENCY_PREFIX:-"$project_root/.build/dependencies/macos-arm64-27"}
xcframework_stager="$project_root/scripts/create-apple-xcframework.py"

fail()
{
	printf '%s\n' "scripts/build-xcframework.sh: $*" >&2
	exit 1
}

canonical_destination()
{
	destination=$1
	parent=$(dirname -- "$destination")
	leaf=$(basename -- "$destination")
	case "$leaf" in
	''|.|..|/) fail "refusing unsafe package destination: $destination" ;;
	esac
	mkdir -p -- "$parent"
	parent=$(CDPATH= cd -- "$parent" && pwd -P)
	printf '%s/%s\n' "$parent" "$leaf"
}

clean_build_environment()
{
	env \
		CFLAGS= \
		CXXFLAGS= \
		OBJCFLAGS= \
		CPPFLAGS= \
		ASMFLAGS= \
		LDFLAGS= \
		"$@"
}

# A packaged header tree holds exactly the public headers, as regular files
# with their source's bytes.
verify_public_headers()
{
	header_root=$1
	symlink=$(find "$header_root" -type l -print -quit)
	[ -z "$symlink" ] || fail "public header tree contains a symlink: $symlink"
	expected=$(printf '%s\n' \
		"$header_root/uml/abi_identity.h" "$header_root/uml/emulator.h")
	[ "$(find "$header_root" -type f | LC_ALL=C sort)" = "$expected" ] ||
		fail "public header tree contains an unexpected or missing file: $header_root"
	for header in abi_identity.h emulator.h; do
		cmp "$project_root/darwin/api/include/uml/$header" \
			"$header_root/uml/$header" ||
			fail "public header differs from its source: $header_root/uml/$header"
	done
}

verify_archive()
{
	platform=$1
	archive=$2
	audit_dir=$3

	[ -f "$archive" ] || fail "missing $platform archive: $archive"
	architectures=$(xcrun lipo -archs "$archive")
	[ "$architectures" = arm64 ] ||
		fail "$platform archive has unexpected architectures: $architectures"

	mkdir -p -- "$audit_dir"
	xcrun nm -gU "$archive" > "$audit_dir/global-defined.txt"
	# The functions the public headers declare are the whole API.
	python3 -B "$project_root/scripts/check-public-api.py" \
		--headers "$project_root/darwin/api/include/uml" \
		--defined "$audit_dir/global-defined.txt" ||
		fail "$platform archive does not implement the exact public C API"

	# A product that runs a guest force-loads the archive, because the kernel
	# reaches the host bridge through dlsym(). Every first-party symbol that
	# any member references must therefore be defined by another member.
	xcrun nm -g -P "$archive" > "$audit_dir/global-symbols.txt"
	awk '
		NF >= 2 && $2 == "U" { undefined[$1] = 1; next }
		NF >= 2 { defined[$1] = 1 }
		END {
			for (symbol in undefined) {
				if (symbol in defined)
					continue
				if (symbol ~ /^__Z.*3uml/ || symbol ~ /^_(darwin_)?uml_/) {
					printf("unresolved first-party symbol: %s\n",
					       symbol) > "/dev/stderr"
					failed = 1
				}
			}
			exit failed
		}
	' "$audit_dir/global-symbols.txt" ||
		fail "$platform archive does not carry every first-party object it references"

	# The x86-64 JIT and the FEXCore it embeds are macOS-only.
	if [ "$platform" != macos ]; then
		python3 -B "$project_root/scripts/check-no-fexcore.py" "$archive" ||
			fail "$platform archive carries FEXCore"
	fi
}

# Configure and build the external consumers against the package the
# trailing -D options name, and run them where the host can.
build_consumer()
{
	platform=$1
	generator=$2
	consumer_dir=$3
	shift 3
	clean_build_environment cmake \
		-S "$project_root/tests/consumer/apple" \
		-B "$consumer_dir" -G "$generator" \
		-DCMAKE_TOOLCHAIN_FILE="$toolchain" \
		-DUML_APPLE_PLATFORM="$platform" \
		-DCMAKE_C_FLAGS:STRING= \
		-DCMAKE_CXX_FLAGS:STRING= \
		-DCMAKE_OBJC_FLAGS:STRING= \
		-DCMAKE_EXE_LINKER_FLAGS:STRING= \
		"$@"
	cmake --build "$consumer_dir" --config Release \
		--target uml-emulator-external-consumer \
			uml-emulator-external-c-consumer \
			uml-emulator-external-objc-consumer --parallel
	if [ "$platform" = macos ]; then
		binaries=$consumer_dir
		[ "$generator" = Ninja ] || binaries="$consumer_dir/Release"
		"$binaries/uml-emulator-external-consumer"
		"$binaries/uml-emulator-external-c-consumer"
		"$binaries/uml-emulator-external-objc-consumer"
	fi
}

build_slice()
{
	platform=$1
	build_dir="$work_root/$platform"
	network_provider=OFF
	if [ "$platform" = macos ]; then
		# Build the app-owned provider beside the neutral callback library.
		network_provider=ON
	fi
	clean_build_environment cmake -S "$project_root" -B "$build_dir" -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE="$toolchain" \
		-DUML_APPLE_PLATFORM="$platform" \
		-DUML_BUILD_CLI=OFF \
		-DUML_BUILD_KERNEL_RUNNER=OFF \
		-DUML_BUILD_UNIT_TESTS=OFF \
		-DUML_BUILD_INTEGRATION_TESTS=OFF \
		-DUML_BUILD_TOOLING_TESTS=ON \
		-DUML_BUILD_GPU=OFF \
		-DUML_BUILD_NETWORK_PROVIDER="$network_provider" \
		-DUML_DEPENDENCY_PREFIX="$dependency_prefix" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_FLAGS:STRING= \
		-DCMAKE_CXX_FLAGS:STRING= \
		-DCMAKE_ASM_FLAGS:STRING= \
		-DCMAKE_EXE_LINKER_FLAGS:STRING= \
		-DCMAKE_STATIC_LINKER_FLAGS:STRING=

	set -- uml_emulator architecture-audit
	if [ "$platform" = macos ]; then
		set -- "$@" uml_network_provider
	fi
	cmake --build "$build_dir" --config Release --target "$@" --parallel

	library="$build_dir/darwin/emulator/libuml_emulator.a"
	verify_archive "$platform" "$library" "$build_dir/package-audit/built"

	install_dir="$build_dir/install"
	cmake --install "$build_dir" --config Release --prefix "$install_dir"
	installed_library="$install_dir/lib/libuml_emulator.a"
	# The stager refuses an install tree with other headers or a symlink,
	# and the final package's headers are compared with their sources.
	[ -f "$install_dir/lib/cmake/UMLEmulator/UMLEmulatorConfig.cmake" ] ||
		fail "$platform install is missing its CMake package metadata"
	verify_archive "$platform" "$installed_library" \
		"$build_dir/package-audit/installed"
	build_consumer "$platform" Ninja "$build_dir/installed-external-consumer" \
		-DUML_EMULATOR_PACKAGE_PREFIX="$install_dir" \
		-DCMAKE_BUILD_TYPE=Release

	case "$platform" in
	macos)
		macos_install=$install_dir
		;;
	ios)
		ios_install=$install_dir
		;;
	ios-simulator)
		ios_simulator_install=$install_dir
		;;
	esac
}

verify_final_slice()
{
	platform=$1
	identifier=$2
	slice_root="$staged_output/$identifier"
	final_library="$slice_root/libuml_emulator.a"
	final_headers="$slice_root/Headers"
	verify_archive "$platform" "$final_library" \
		"$work_root/final-audit/$platform"
	verify_public_headers "$final_headers"
	build_consumer "$platform" Ninja "$work_root/final-consumer/$platform" \
		-DUML_EMULATOR_LIBRARY="$final_library" \
		-DUML_EMULATOR_HEADERS="$final_headers" \
		-DCMAKE_BUILD_TYPE=Release
}

build_root=$(canonical_destination "$build_root")
output=$(canonical_destination "$output")
if [ "$(basename -- "$output")" != UMLEmulator.xcframework ]; then
	fail "XCFramework output must be named UMLEmulator.xcframework: $output"
fi
case "$build_root" in
"$output"|"$output"/*)
	fail "XCFramework output may not contain its slice build root: $output"
	;;
esac
mkdir -p -- "$build_root"

work_root=''
package_stage=''
cleanup()
{
	if [ -n "$work_root" ] && [ -d "$work_root" ]; then
		rm -rf -- "$work_root"
	fi
	if [ -n "$package_stage" ] && [ -d "$package_stage" ]; then
		rm -rf -- "$package_stage"
	fi
}
trap cleanup EXIT HUP INT TERM
work_root=$(mktemp -d "$build_root/.xcframework-slices.XXXXXX")
package_stage=$(mktemp -d "$build_root/.xcframework-output.XXXXXX")
staged_output="$package_stage/UMLEmulator.xcframework"

macos_install=''
ios_install=''
ios_simulator_install=''
build_slice macos
build_slice ios
build_slice ios-simulator

# Stage the documented v1.0 layout directly so all supported static-library
# slices and public headers can be consumed through one XCFramework.
python3 -B "$xcframework_stager" \
	--slice "macos=$macos_install" \
	--slice "ios=$ios_install" \
	--slice "ios-simulator=$ios_simulator_install" \
	--output "$staged_output"

# The staged Info.plist describes exactly the three supported arm64 slices.
if ! python3 -B - "$staged_output/Info.plist" <<'EOF'
import plistlib
import sys

with open(sys.argv[1], "rb") as stream:
    libraries = plistlib.load(stream)["AvailableLibraries"]
slices = sorted(
    (library["SupportedPlatform"], library.get("SupportedPlatformVariant", ""),
     library["LibraryIdentifier"], library["SupportedArchitectures"],
     library["LibraryPath"], library["HeadersPath"])
    for library in libraries)
expected = [
    (platform, variant, identifier, ["arm64"], "libuml_emulator.a", "Headers")
    for platform, variant, identifier in (
        ("ios", "", "ios-arm64"),
        ("ios", "simulator", "ios-arm64-simulator"),
        ("macos", "", "macos-arm64"))]
if slices != expected:
    sys.exit(f"unexpected XCFramework slices: {slices}")
EOF
then
	fail "XCFramework Info.plist does not describe the three supported slices"
fi

verify_final_slice macos macos-arm64
verify_final_slice ios ios-arm64
verify_final_slice ios-simulator ios-arm64-simulator
build_consumer macos Xcode "$work_root/final-xcode-consumer/macos" \
	-DUML_EMULATOR_XCFRAMEWORK="$staged_output"

# An existing package is exchanged with the staged one in a single rename;
# the previous package then sits in the stage, which cleanup removes. A first
# publish is exclusive, so a run that loses the race fails instead of moving
# its package inside the winner's.
publisher="$project_root/dependencies/publish-prefix.py"
if [ -e "$output" ] || [ -L "$output" ]; then
	python3 -B "$publisher" --swap "$staged_output" "$output" ||
		fail "could not replace $output atomically"
else
	python3 -B "$publisher" "$staged_output" "$output" ||
		fail "could not publish $output; another run may have published it first"
fi
printf '%s\n' "$output"
