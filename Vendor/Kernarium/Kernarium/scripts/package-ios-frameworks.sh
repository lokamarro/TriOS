#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Package the verified iOS framework slices used by the Kernarium and Andarium
# Xcode projects. All output is generated below .build by default.
#
# --sdk device (the default) and --sdk simulator select the slices; both may
# be named, and the XCFrameworks then carry both, so Xcode picks the slice for
# its destination. The device stack renders with Mesa Zink on KosmicKrisp,
# which requires Metal 4. The iOS Simulator exposes no Metal 4 device, so its
# GPU stack is Mesa's CPU rasterizer with no Vulkan driver: the Simulator
# slices carry no KosmicKrisp, and KosmicKrisp.xcframework has a device slice
# alone. Only the Simulator's slices may carry the software renderer; the
# device slices are checked for it. An iOS device build also runs on an Apple
# silicon Mac in Designed for iPad mode when no device is attached.
#
# --automation packages the automation Linux runtimes, which boot host
# payloads (Kernarium/README.md), in place of the release ones. The Linux
# runtime is checked: the release runtime must not read a payload or contain
# the setting that starts the background job with the guest, and the
# automation runtime must. --android-automation likewise packages the
# automation Android runtime, whose launch may turn ADB authorization off
# and start the background job with the guest, and checks that the release
# Android runtime carries neither setting.
#
# --runtimes android packages the Android runtime alone, for Andarium, whose
# builds need not build the Linux runtime (--automation, which packages it,
# conflicts with it). The output directory is shared: Kernarium needs the
# default full set, so package that again before building Kernarium.
#
# --app-store packages the App Store Android runtime, built from the
# kernarium-ios-device-android-app-store preset, for the device alone and for
# Andarium's App Store configuration: it reads no environment variable that
# tunes, observes or drives a run, and the packaging fails if the runtime
# carries the name of one. It conflicts with both automation options, with the
# Simulator and with --runtimes all.
#
# No packaged framework keeps the linker's debug map: it names the object files
# of the build host (N_OSO stabs, absolute paths of the checkout and the
# dependency trees) and would ship in the application. Each framework's dSYM,
# made from the unstripped binary, keeps the symbols, and `strip -S` keeps the
# binary's UUID that the two share. An App Store packaging also checks that no
# framework still has it.
#
# Every embedded framework carries a PrivacyInfo.xcprivacy
# (Kernarium/PrivacyManifests) in each slice, and the device slices are checked
# with scripts/check-privacy-manifests.py, which fails a binary that uses a
# required-reason API the manifest does not declare, and a framework
# Info.plist with an empty version or no MinimumOSVersion.
#
# KernariumTests also links HostBattery.xcframework: the Apple battery
# provider's archive, as the Android runtime links it, with its header. The
# application neither links nor embeds it, and the test bundle loads no second
# runtime to reach it. HostVideoEncoder.xcframework is the video encoder's
# archive, packaged the same way for the capability tests. Both have a slice
# for each packaged SDK, built with the runtime of that SDK.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUTPUT=${KERNARIUM_XCFRAMEWORK_OUTPUT:-"$ROOT_DIR/.build/kernarium-xcframeworks"}

fail()
{
	printf '%s\n' "Kernarium/scripts/package-ios-frameworks.sh: $*" >&2
	exit 1
}

AUTOMATION=0
ANDROID_AUTOMATION=0
APP_STORE=0
RUNTIMES=all
SDKS=
USAGE="usage: package-ios-frameworks.sh [--sdk device|simulator]... [--automation] [--android-automation] [--app-store] [--runtimes all|android]"
while [ "$#" -gt 0 ]; do
	case "$1" in
	--sdk)
		[ "$#" -ge 2 ] || fail "$USAGE"
		case "$2" in
		device | simulator) ;;
		*) fail "--sdk takes device or simulator: $2" ;;
		esac
		case " $SDKS " in
		*" $2 "*) fail "--sdk $2 is named twice" ;;
		esac
		SDKS="$SDKS $2"
		shift
		;;
	--automation) AUTOMATION=1 ;;
	--android-automation) ANDROID_AUTOMATION=1 ;;
	--app-store) APP_STORE=1 ;;
	--runtimes)
		[ "$#" -ge 2 ] || fail "$USAGE"
		RUNTIMES=$2
		shift
		;;
	*) fail "$USAGE" ;;
	esac
	shift
done
case "$RUNTIMES" in
all) ;;
android)
	[ "$AUTOMATION" -eq 0 ] ||
		fail "--automation packages the Linux runtime; it conflicts with --runtimes android"
	;;
*) fail "--runtimes takes all or android: $RUNTIMES" ;;
esac
if [ "$APP_STORE" -eq 1 ]; then
	[ "$RUNTIMES" = android ] ||
		fail "--app-store packages the Android runtime alone; add --runtimes android"
	[ "$AUTOMATION" -eq 0 ] && [ "$ANDROID_AUTOMATION" -eq 0 ] ||
		fail "--app-store conflicts with the automation options"
	case " $SDKS " in
	*" simulator "*) fail "--app-store packages the device alone" ;;
	esac
fi

find_tool()
{
	variable=$1
	shift
	for candidate in "$@"; do
		tool_path=$(command -v "$candidate" 2>/dev/null || true)
		if [ -n "$tool_path" ]; then
			eval "$variable=\$tool_path"
			return
		fi
	done
	fail "required packaging tool is unavailable: $*"
}

[ -n "$SDKS" ] || SDKS=device
has_sdk()
{
	case " $SDKS " in
	*" $1 "*) return 0 ;;
	esac
	return 1
}

find_tool XCODEBUILD xcodebuild
find_tool CMAKE cmake
find_tool OTOOL otool
find_tool NM nm
find_tool STRIP strip
find_tool PYTHON python3

RUNTIME_4K="KRuntime_6_18_4K"

# The Android runtime carries an independently accepted Android kernel
# artifact. Rootfs release updates do not rename this framework and kernel-only
# updates do not invalidate an existing guest bundle.
RUNTIME_ANDROID_4K="KRuntime_Android_6_18_4K"

RUNTIME_TREE=kernarium-runtime-ios-device
[ "$AUTOMATION" -eq 0 ] || RUNTIME_TREE=kernarium-runtime-ios-device-automation
ANDROID_TREE=kernarium-runtime-ios-device-android-4k
[ "$ANDROID_AUTOMATION" -eq 0 ] ||
	ANDROID_TREE=kernarium-runtime-ios-device-android-4k-automation
[ "$APP_STORE" -eq 0 ] ||
	ANDROID_TREE=kernarium-runtime-ios-device-android-4k-app-store
DEVICE_RUNTIME_4K="$ROOT_DIR/.build/$RUNTIME_TREE/Kernarium/Runtime/$RUNTIME_4K.framework"
DEVICE_RUNTIME_ANDROID="$ROOT_DIR/.build/$ANDROID_TREE/Kernarium/Runtime/$RUNTIME_ANDROID_4K.framework"
# KERNARIUM_DEVICE_GPU_PREFIX and KERNARIUM_SIMULATOR_GPU_PREFIX name a GPU
# prefix other than the default one, for a build that holds its own.
DEVICE_GPU_PREFIX=${KERNARIUM_DEVICE_GPU_PREFIX:-"$ROOT_DIR/.build/dependencies/apple-gpu-vulkan-experiment/ios-device-arm64"}
DEVICE_GPU="$DEVICE_GPU_PREFIX/Frameworks"
# The Simulator's runtimes are the same presets for its SDK, and its GPU prefix
# is the software variant.
SIMULATOR_RUNTIME_TREE=kernarium-runtime-ios-simulator
[ "$AUTOMATION" -eq 0 ] ||
	SIMULATOR_RUNTIME_TREE=kernarium-runtime-ios-simulator-automation
SIMULATOR_ANDROID_TREE=kernarium-runtime-ios-simulator-android-4k
[ "$ANDROID_AUTOMATION" -eq 0 ] ||
	SIMULATOR_ANDROID_TREE=kernarium-runtime-ios-simulator-android-4k-automation
SIMULATOR_RUNTIME_4K="$ROOT_DIR/.build/$SIMULATOR_RUNTIME_TREE/Kernarium/Runtime/$RUNTIME_4K.framework"
SIMULATOR_RUNTIME_ANDROID="$ROOT_DIR/.build/$SIMULATOR_ANDROID_TREE/Kernarium/Runtime/$RUNTIME_ANDROID_4K.framework"
SIMULATOR_GPU_PREFIX=${KERNARIUM_SIMULATOR_GPU_PREFIX:-"$ROOT_DIR/.build/dependencies/apple-gpu-vulkan-experiment/ios-simulator-arm64"}
SIMULATOR_GPU="$SIMULATOR_GPU_PREFIX/Frameworks"
STORAGE="$ROOT_DIR/.build/dependencies/ios-storage/GPTFDisk.xcframework"
ZSTD="$ROOT_DIR/.build/dependencies/ios-zstd/LibZstd.xcframework"
HOST_BATTERY="$ROOT_DIR/.build/$ANDROID_TREE/darwin/platform/libuml_host_battery.a"
HOST_BATTERY_HEADER="$ROOT_DIR/darwin/platform/host_battery.hpp"
ANDROID_RUNTIME_TREE="$ROOT_DIR/.build/$ANDROID_TREE"
HOST_VIDEO_ENCODER="$ANDROID_RUNTIME_TREE/darwin/platform/libuml_video_encoder.a"
HOST_VIDEO_ENCODER_HEADERS="$ROOT_DIR/darwin/platform/video_encoder.hpp $ROOT_DIR/darwin/platform/video_decoder.hpp"
SIMULATOR_HOST_BATTERY="$ROOT_DIR/.build/$SIMULATOR_ANDROID_TREE/darwin/platform/libuml_host_battery.a"
SIMULATOR_ANDROID_RUNTIME_TREE="$ROOT_DIR/.build/$SIMULATOR_ANDROID_TREE"
SIMULATOR_HOST_VIDEO_ENCODER="$SIMULATOR_ANDROID_RUNTIME_TREE/darwin/platform/libuml_video_encoder.a"

# Mesa supplies the host GL provider as MesaEGL and MesaGallium. The device
# stack also has the Vulkan driver they render on, KosmicKrisp; the Simulator's
# has none.
GL_FRAMEWORKS="MesaEGL MesaGallium"
GPU_FRAMEWORKS="$GL_FRAMEWORKS KosmicKrisp"

# Packaging is an independent trust boundary: revalidate the complete source,
# contract, provenance, binary-platform, and provider-negative policy before
# consuming a GPU prefix.
if has_sdk device; then
	"$ROOT_DIR/dependencies/build-apple-gpu.sh" --platform ios \
		--prefix "$DEVICE_GPU_PREFIX" --verify-only
fi
"$ROOT_DIR/dependencies/build-ios-storage.sh"
"$ROOT_DIR/dependencies/build-ios-zstd.sh"
if has_sdk simulator; then
	"$ROOT_DIR/dependencies/build-apple-gpu.sh" --platform ios-simulator \
		--prefix "$SIMULATOR_GPU_PREFIX" --verify-only
fi

for framework in \
	"$STORAGE" \
	"$ZSTD"; do
	[ -d "$framework" ] || fail "missing framework slice: $framework"
done
if has_sdk device; then
	[ -d "$DEVICE_RUNTIME_ANDROID" ] ||
		fail "missing framework slice: $DEVICE_RUNTIME_ANDROID"
	[ "$RUNTIMES" = android ] || [ -d "$DEVICE_RUNTIME_4K" ] ||
		fail "missing framework slice: $DEVICE_RUNTIME_4K"
	[ -f "$HOST_BATTERY" ] || fail "missing host battery archive: $HOST_BATTERY"
	# The encoder archive is a static library of the runtime tree; it is built
	# here when the runtime itself has not linked it yet.
	"$CMAKE" --build "$ANDROID_RUNTIME_TREE" --target uml_video_encoder ||
		fail "could not build the host video encoder archive"
	[ -f "$HOST_VIDEO_ENCODER" ] || fail "missing host video encoder archive: $HOST_VIDEO_ENCODER"
	for framework in $GPU_FRAMEWORKS; do
		[ -d "$DEVICE_GPU/$framework.framework" ] ||
			fail "missing framework slice: $DEVICE_GPU/$framework.framework"
	done
fi
if has_sdk simulator; then
	[ -d "$SIMULATOR_RUNTIME_ANDROID" ] ||
		fail "missing framework slice: $SIMULATOR_RUNTIME_ANDROID"
	[ "$RUNTIMES" = android ] || [ -d "$SIMULATOR_RUNTIME_4K" ] ||
		fail "missing framework slice: $SIMULATOR_RUNTIME_4K"
	for framework in $GL_FRAMEWORKS; do
		[ -d "$SIMULATOR_GPU/$framework.framework" ] ||
			fail "missing framework slice: $SIMULATOR_GPU/$framework.framework"
	done
	[ -f "$SIMULATOR_HOST_BATTERY" ] ||
		fail "missing host battery archive: $SIMULATOR_HOST_BATTERY"
	"$CMAKE" --build "$SIMULATOR_ANDROID_RUNTIME_TREE" --target uml_video_encoder ||
		fail "could not build the host video encoder archive"
	[ -f "$SIMULATOR_HOST_VIDEO_ENCODER" ] ||
		fail "missing host video encoder archive: $SIMULATOR_HOST_VIDEO_ENCODER"
fi

OUTPUT_PARENT=$(dirname -- "$OUTPUT")
mkdir -p -- "$OUTPUT_PARENT"
OUTPUT_PARENT=$(CDPATH= cd -- "$OUTPUT_PARENT" && pwd -P)
OUTPUT="$OUTPUT_PARENT/$(basename -- "$OUTPUT")"
STAGE=$(mktemp -d "$OUTPUT_PARENT/.kernarium-xcframeworks.XXXXXX")
cleanup()
{
	if [ -n "${STAGE:-}" ] && [ -d "$STAGE" ]; then
		"$CMAKE" -E remove_directory "$STAGE"
	fi
}
trap cleanup EXIT HUP INT TERM

# create_xcframework NAME DEVICE-FRAMEWORK SIMULATOR-FRAMEWORK DEVICE-DSYM SIMULATOR-DSYM:
# each selected slice carries its original dSYM so Xcode copies it into an
# archive. An empty framework leaves its SDK out.
create_xcframework()
{
	name=$1
	device_framework=$2
	simulator_framework=$3
	device_symbols=$4
	simulator_symbols=$5
	set -- -create-xcframework
	if [ -n "$device_framework" ]; then
		[ -d "$device_symbols" ] ||
			fail "missing debug symbols: $device_symbols; rebuild the framework"
		set -- "$@" -framework "$device_framework" -debug-symbols "$device_symbols"
	fi
	if [ -n "$simulator_framework" ]; then
		[ -d "$simulator_symbols" ] ||
			fail "missing debug symbols: $simulator_symbols; rebuild the framework"
		set -- "$@" -framework "$simulator_framework" -debug-symbols "$simulator_symbols"
	fi
	"$XCODEBUILD" "$@" \
		-output "$STAGE/$name.xcframework"
	[ -f "$STAGE/$name.xcframework/Info.plist" ] ||
		fail "$name XCFramework was not created"
}

# A framework's slice of an SDK, empty for an SDK that is not packaged.
device_slice()
{
	if has_sdk device; then printf '%s' "$1"; fi
}
simulator_slice()
{
	if has_sdk simulator; then printf '%s' "$1"; fi
}

# Every slice of every packaged SDK: the framework slices of the runtimes and
# the GPU stack, each built for its own platform (Mach-O platform 2 for the
# device, 7 for the Simulator).
PACKAGED_SLICES=
has_sdk device && PACKAGED_SLICES="$PACKAGED_SLICES ios-arm64:2"
has_sdk simulator && PACKAGED_SLICES="$PACKAGED_SLICES ios-arm64-simulator:7"
RUNTIME_FRAMEWORKS="$RUNTIME_ANDROID_4K"
if [ "$RUNTIMES" = all ]; then
	RUNTIME_FRAMEWORKS="$RUNTIME_4K $RUNTIME_ANDROID_4K"
	create_xcframework "$RUNTIME_4K" \
		"$(device_slice "$DEVICE_RUNTIME_4K")" \
		"$(simulator_slice "$SIMULATOR_RUNTIME_4K")" \
		"$DEVICE_RUNTIME_4K.dSYM" "$SIMULATOR_RUNTIME_4K.dSYM"
fi
create_xcframework "$RUNTIME_ANDROID_4K" \
	"$(device_slice "$DEVICE_RUNTIME_ANDROID")" \
	"$(simulator_slice "$SIMULATOR_RUNTIME_ANDROID")" \
	"$DEVICE_RUNTIME_ANDROID.dSYM" "$SIMULATOR_RUNTIME_ANDROID.dSYM"
for framework in $GL_FRAMEWORKS; do
	create_xcframework "$framework" \
		"$(device_slice "$DEVICE_GPU/$framework.framework")" \
		"$(simulator_slice "$SIMULATOR_GPU/$framework.framework")" \
		"$DEVICE_GPU_PREFIX/dSYMs/$framework.framework.dSYM" \
		"$SIMULATOR_GPU_PREFIX/dSYMs/$framework.framework.dSYM"
done
# KosmicKrisp needs Metal 4, so it has a device slice alone.
if has_sdk device; then
	create_xcframework KosmicKrisp "$DEVICE_GPU/KosmicKrisp.framework" "" \
		"$DEVICE_GPU_PREFIX/dSYMs/KosmicKrisp.framework.dSYM" ""
fi
# install_privacy_manifest FRAMEWORK MANIFEST: the manifest goes into every
# slice of the framework's XCFramework.
PRIVACY_MANIFESTS="$ROOT_DIR/Kernarium/PrivacyManifests"
install_privacy_manifest()
{
	for slice_framework in "$STAGE/$1.xcframework"/*/"$1.framework"; do
		[ -d "$slice_framework" ] || fail "$1 XCFramework has no framework slice"
		cp -- "$PRIVACY_MANIFESTS/$2.xcprivacy" \
			"$slice_framework/PrivacyInfo.xcprivacy" ||
			fail "could not install the privacy manifest of $1"
	done
}
# strip_debug_maps FRAMEWORK...: removes the debug stabs from every slice of
# each XCFramework, once its dSYM has been taken into the XCFramework.
strip_debug_maps()
{
	for stripped_framework in "$@"; do
		for slice_framework in "$STAGE/$stripped_framework.xcframework"/*/"$stripped_framework.framework"; do
			[ -f "$slice_framework/$stripped_framework" ] ||
				fail "$stripped_framework XCFramework has no framework binary in $slice_framework"
			"$STRIP" -S "$slice_framework/$stripped_framework" ||
				fail "could not strip the debug map of $slice_framework"
		done
	done
}
strip_debug_maps $RUNTIME_FRAMEWORKS $GL_FRAMEWORKS
if has_sdk device; then
	strip_debug_maps KosmicKrisp
fi
# A runtime's dSYM names the kernel's functions at their place in the carrier
# section (scripts/kernel-dsym-symbols.py), which is how a crash report
# symbolizes a panic's frames; its binary never does.
for slice_platform in $PACKAGED_SLICES; do
	slice=${slice_platform%%:*}
	for framework in $RUNTIME_FRAMEWORKS; do
		binary="$STAGE/$framework.xcframework/$slice/$framework.framework/$framework"
		symbols="$STAGE/$framework.xcframework/$slice/dSYMs/$framework.framework.dSYM/Contents/Resources/DWARF/$framework"
		[ -f "$symbols" ] || fail "missing debug symbols: $symbols"
		"$NM" -a "$symbols" | LC_ALL=C grep -q ' ___uml_kernel_symbols$' ||
			fail "the dSYM of $framework does not name the kernel's functions; rebuild the framework"
		if "$NM" -a "$binary" | LC_ALL=C grep -q ' ___uml_kernel_symbols$'; then
			fail "the binary of $framework carries the kernel's function symbols"
		fi
	done
done
for framework in $RUNTIME_FRAMEWORKS; do
	install_privacy_manifest "$framework" KRuntime
done
for framework in $GPU_FRAMEWORKS; do
	[ ! -d "$STAGE/$framework.xcframework" ] ||
		install_privacy_manifest "$framework" "$framework"
done
"$CMAKE" -E copy_directory "$STORAGE" \
	"$STAGE/GPTFDisk.xcframework"
"$CMAKE" -E copy_directory "$ZSTD" \
	"$STAGE/LibZstd.xcframework"
# The two archives serve KernariumTests: each slice is the archive of the
# runtime tree of its SDK, and an SDK that is not packaged leaves its slice out.
# create_archive_xcframework NAME HEADER... -- DEVICE-ARCHIVE SIMULATOR-ARCHIVE
create_archive_xcframework()
{
	name=$1
	shift
	headers="$STAGE/$name-headers"
	mkdir -p -- "$headers"
	while [ "$1" != -- ]; do
		cp -- "$1" "$headers/"
		shift
	done
	shift
	slices=
	for archive in "$1" "$2"; do
		[ -z "$archive" ] ||
			slices="$slices -library $archive -headers $headers"
	done
	# shellcheck disable=SC2086
	"$XCODEBUILD" -create-xcframework $slices \
		-output "$STAGE/$name.xcframework"
	"$CMAKE" -E remove_directory "$headers"
	[ -f "$STAGE/$name.xcframework/Info.plist" ] ||
		fail "$name XCFramework was not created"
}
create_archive_xcframework HostBattery "$HOST_BATTERY_HEADER" -- \
	"$(device_slice "$HOST_BATTERY")" \
	"$(simulator_slice "$SIMULATOR_HOST_BATTERY")"
# shellcheck disable=SC2086
create_archive_xcframework HostVideoEncoder $HOST_VIDEO_ENCODER_HEADERS -- \
	"$(device_slice "$HOST_VIDEO_ENCODER")" \
	"$(simulator_slice "$SIMULATOR_HOST_VIDEO_ENCODER")"

packaged_frameworks()
{
	slice=$1
	case "$slice" in
	ios-arm64) printf '%s ' $RUNTIME_FRAMEWORKS $GPU_FRAMEWORKS ;;
	*) printf '%s ' $RUNTIME_FRAMEWORKS $GL_FRAMEWORKS ;;
	esac
}
for slice_platform in $PACKAGED_SLICES; do
	slice=${slice_platform%%:*}
	platform=${slice_platform##*:}
	for framework in $(packaged_frameworks "$slice"); do
		binary="$STAGE/$framework.xcframework/$slice/$framework.framework/$framework"
		[ -f "$binary" ] || fail "missing packaged framework binary: $binary"
		binary_platform=$("$OTOOL" -l "$binary" | awk '
			$1 == "cmd" { command = $2; next }
			command == "LC_BUILD_VERSION" && $1 == "platform" {
				print $2
				exit
			}
		')
		[ "$binary_platform" = "$platform" ] ||
			fail "$framework is built for platform $binary_platform, not the $slice slice's $platform"
	done
	# Each archive's objects are built for the slice's own platform.
	for archive in HostBattery:libuml_host_battery HostVideoEncoder:libuml_video_encoder; do
		library="$STAGE/${archive%%:*}.xcframework/$slice/${archive##*:}.a"
		[ -f "$library" ] || fail "missing packaged host archive: $library"
		archive_platforms=$("$OTOOL" -l "$library" | awk '
			$1 == "cmd" { command = $2; next }
			command == "LC_BUILD_VERSION" && $1 == "platform" { print $2 }
		' | sort -u)
		[ "$archive_platforms" = "$platform" ] ||
			fail "${archive##*:}.a is built for platform $archive_platforms, not the $slice slice's $platform"
	done
	for framework in $RUNTIME_FRAMEWORKS; do
		binary="$STAGE/$framework.xcframework/$slice/$framework.framework/$framework"
		rpaths=$("$OTOOL" -l "$binary" | awk '
			$1 == "cmd" { command = $2; next }
			command == "LC_RPATH" && $1 == "path" {
				print $2
				command = ""
			}
		')
		[ "$rpaths" = '@loader_path/..' ] ||
			fail "$framework has an invalid runtime search path: $rpaths"
	done
done
# KosmicKrisp has no Simulator slice, so an SDK set holds none for it.
if has_sdk simulator; then
	[ ! -e "$STAGE/KosmicKrisp.xcframework/ios-arm64-simulator" ] ||
		fail "KosmicKrisp has a Simulator slice, which needs Metal 4"
fi
# The software renderer is the Simulator's alone: no device slice carries it,
# and the Simulator's GL provider does.
if has_sdk device; then
	for framework in $RUNTIME_FRAMEWORKS $GPU_FRAMEWORKS; do
		"$PYTHON" -B "$ROOT_DIR/scripts/check-software-renderer.py" --absent \
			"$STAGE/$framework.xcframework/ios-arm64" ||
			fail "a device slice of $framework carries the software renderer"
	done
fi
if has_sdk simulator; then
	"$PYTHON" -B "$ROOT_DIR/scripts/check-software-renderer.py" --present \
		"$STAGE/MesaGallium.xcframework/ios-arm64-simulator" ||
		fail "the Simulator's MesaGallium does not carry the software renderer"
fi
# Each packaged slice's runtimes are the release or the automation build the
# options name.
for slice_platform in $PACKAGED_SLICES; do
	slice=${slice_platform%%:*}
	case "$slice" in
	ios-arm64) preset=kernarium-ios-device ;;
	*) preset=kernarium-ios-simulator ;;
	esac
	if [ "$RUNTIMES" = all ]; then
		binary="$STAGE/$RUNTIME_4K.xcframework/$slice/$RUNTIME_4K.framework/$RUNTIME_4K"
		if LC_ALL=C grep -a -q KERNARIUM_BOOT_PAYLOAD "$binary"; then
			[ "$AUTOMATION" -eq 1 ] ||
				fail "$RUNTIME_4K is an automation runtime; package it with --automation"
		else
			[ "$AUTOMATION" -eq 0 ] ||
				fail "$RUNTIME_4K is a release runtime; build the $preset-automation preset"
		fi
		# The settings that start the background job with the guest and delay
		# the GPU hold are in the automation runtimes alone.
		if LC_ALL=C grep -a -q KERNARIUM_BACKGROUND_JOB_AT_START "$binary"; then
			[ "$AUTOMATION" -eq 1 ] ||
				fail "$RUNTIME_4K carries the background job setting; only an automation runtime may"
		else
			[ "$AUTOMATION" -eq 0 ] ||
				fail "$RUNTIME_4K is an automation runtime without the background job setting; rebuild it"
		fi
		if LC_ALL=C grep -a -q KERNARIUM_BACKGROUND_HOLD_DELAY_MS "$binary"; then
			[ "$AUTOMATION" -eq 1 ] ||
				fail "$RUNTIME_4K carries the GPU hold delay setting; only an automation runtime may"
		else
			[ "$AUTOMATION" -eq 0 ] ||
				fail "$RUNTIME_4K is an automation runtime without the GPU hold delay setting; rebuild it"
		fi
	fi

	binary="$STAGE/$RUNTIME_ANDROID_4K.xcframework/$slice/$RUNTIME_ANDROID_4K.framework/$RUNTIME_ANDROID_4K"
	if LC_ALL=C grep -a -q KERNARIUM_ANDROID_ADB_AUTH "$binary"; then
		[ "$ANDROID_AUTOMATION" -eq 1 ] ||
			fail "$RUNTIME_ANDROID_4K is an automation runtime; package it with --android-automation"
	else
		[ "$ANDROID_AUTOMATION" -eq 0 ] ||
			fail "$RUNTIME_ANDROID_4K is a release runtime; build the $preset-android-automation preset"
	fi
	if LC_ALL=C grep -a -q KERNARIUM_BACKGROUND_JOB_AT_START "$binary"; then
		[ "$ANDROID_AUTOMATION" -eq 1 ] ||
			fail "$RUNTIME_ANDROID_4K carries the background job setting; only an automation runtime may"
	else
		[ "$ANDROID_AUTOMATION" -eq 0 ] ||
			fail "$RUNTIME_ANDROID_4K is an automation runtime without the background job setting; rebuild it"
	fi
	if LC_ALL=C grep -a -q KERNARIUM_BACKGROUND_HOLD_DELAY_MS "$binary"; then
		[ "$ANDROID_AUTOMATION" -eq 1 ] ||
			fail "$RUNTIME_ANDROID_4K carries the GPU hold delay setting; only an automation runtime may"
	else
		[ "$ANDROID_AUTOMATION" -eq 0 ] ||
			fail "$RUNTIME_ANDROID_4K is an automation runtime without the GPU hold delay setting; rebuild it"
	fi
done

# An App Store runtime carries the name of no environment knob, and no framework
# of it keeps a trace of the host that built it.
if [ "$APP_STORE" -eq 1 ]; then
	"$PYTHON" -B "$ROOT_DIR/Andarium/scripts/check-app-store-bundle.py" --runtime \
		"$STAGE/$RUNTIME_ANDROID_4K.xcframework/ios-arm64/$RUNTIME_ANDROID_4K.framework" ||
		fail "$RUNTIME_ANDROID_4K carries an environment knob or a trace of its build host; build the kernarium-ios-device-android-app-store preset"
	for framework in $GPU_FRAMEWORKS; do
		"$PYTHON" -B "$ROOT_DIR/Andarium/scripts/check-app-store-bundle.py" --host-traces \
			"$STAGE/$framework.xcframework/ios-arm64/$framework.framework" ||
			fail "$framework keeps a trace of its build host"
	done
fi

# The storage and compression cores carry no Metal dependency, so their
# XCFrameworks keep both published slices.
for platform in ios-arm64 ios-arm64-simulator; do
	binary="$STAGE/GPTFDisk.xcframework/$platform/GPTFDisk.framework/GPTFDisk"
	[ -f "$binary" ] || fail "missing packaged GPT fdisk core: $binary"
	binary="$STAGE/LibZstd.xcframework/$platform/LibZstd.framework/LibZstd"
	[ -f "$binary" ] || fail "missing packaged zstd decompressor: $binary"
done

# The device slices are what an upload carries: each must declare the
# required-reason APIs its binary uses and carry a complete Info.plist.
if has_sdk device; then
	for framework in $RUNTIME_FRAMEWORKS $GPU_FRAMEWORKS; do
		"$PYTHON" -B "$ROOT_DIR/scripts/check-privacy-manifests.py" \
			"$STAGE/$framework.xcframework/ios-arm64/$framework.framework" ||
			fail "the device slice of $framework fails the privacy manifest check"
	done
fi

# The x86-64 JIT and the FEXCore it embeds are macOS-only.
"$PYTHON" -B "$ROOT_DIR/scripts/check-no-fexcore.py" "$STAGE" ||
	fail "a packaged iOS framework carries FEXCore"

# Publish with one atomic rename, so the XCFrameworks are never absent and a
# concurrent run is never moved below another's tree: the first publish is an
# exclusive rename, and a later one swaps the new tree in, which leaves the
# tree it replaced at the stage path for cleanup to remove.
publish=0
"$PYTHON" "$ROOT_DIR/dependencies/publish-prefix.py" "$STAGE" "$OUTPUT" ||
	publish=$?
case $publish in
0) STAGE= ;;
1)
	publish=0
	"$PYTHON" "$ROOT_DIR/dependencies/publish-prefix.py" \
		--swap "$STAGE" "$OUTPUT" || publish=$?
	[ "$publish" -eq 0 ] ||
		fail "could not replace the published XCFrameworks: $OUTPUT"
	if [ -L "$STAGE" ]; then
		rm -f -- "$STAGE"
		STAGE=
	fi
	;;
*) fail "could not publish the XCFrameworks: $OUTPUT" ;;
esac
if [ "$APP_STORE" -eq 1 ]; then
	printf '%s\n' "published App Store Android runtime XCFrameworks: $OUTPUT"
elif [ "$RUNTIMES" = android ]; then
	printf '%s\n' "published Android runtime XCFrameworks: $OUTPUT"
elif [ "$AUTOMATION" -eq 1 ] || [ "$ANDROID_AUTOMATION" -eq 1 ]; then
	printf '%s\n' "published Kernarium automation XCFrameworks: $OUTPUT"
else
	printf '%s\n' "published Kernarium XCFrameworks: $OUTPUT"
fi
