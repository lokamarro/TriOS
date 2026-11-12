#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the fixed LineageOS 24 UML product: page-agnostic arm64 userspace with
# a 32-bit armeabi-v7a second architecture for applications, on the 4 KiB
# kernel.
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ANDROID_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
METADATA="$ANDROID_ROOT/metadata/lineage.env"
VERIFIER="$ANDROID_ROOT/tools/verify-aosp-patches.py"
MESA_VENUS_VERIFIER="$ANDROID_ROOT/tools/verify-mesa-venus-source.py"
TARGET_FILES_VERIFIER="$ANDROID_ROOT/tools/verify-target-files.py"
FIRST_BOOT_TOOL="$ANDROID_ROOT/tools/precompile-first-boot-apps.py"
OUTER_ROOT=$(CDPATH= cd -- "$ANDROID_ROOT/../.." && pwd)
KERNEL_COMMON_ROOT="$OUTER_ROOT/kernel/common"
KERNEL_ARTIFACT_VERIFIER="$KERNEL_COMMON_ROOT/artifacts/verify-kernel-artifact.py"

AOSP_SOURCE=
OUT_DIR=
KERNEL_ARTIFACT=
JOBS=
BUILD_NUMBER_VALUE=

# shellcheck disable=SC1090
source "$SCRIPT_DIR/common.sh"

usage()
{
	cat <<'EOF'
Usage: android/lineage-24/build/build-aosp.sh \
       --aosp-source ABSOLUTE_PATH \
       --out ABSOLUTE_PATH \
       --kernel-artifact ABSOLUTE_PATH [--jobs N] [--build-number N]

Run locally on the x86_64 Linux builder after applying the routed AOSP patch
series. The kernel is a published Android kernel artifact of the tracked
mainline source, whose vmlinux is the runtime image without DWARF. The output
directory must be an ignored directory inside the AOSP Repo
workspace; the pinned Android 17 Soong tree rejects generated host-tool paths
from an external absolute OUT_DIR. Reusing the same output path lets the
Android build system select and preserve its compatible incremental state; a
missing output path is initialized automatically.

--build-number sets Android's BUILD_NUMBER, which becomes the build's
ro.build.version.incremental and the number of its fingerprint. A release
carries its own number: it is how a data partition tells a newer system from
an older one, so numbers rise from release to release. A build without one
keeps AOSP's eng.<user> and cannot be published.
EOF
}

verify_dexpreopt_config()
{
	python3 -B - "$1" <<'PY'
import json
import sys
from pathlib import Path

config_path = Path(sys.argv[1])
try:
    with config_path.open(encoding="utf-8") as source:
        config = json.load(source)
except (OSError, json.JSONDecodeError) as error:
    raise SystemExit(f"cannot read generated dexpreopt config {config_path}: {error}")

expected = {
    "DisablePreopt": False,
    "DisablePreoptBootImages": False,
    "OnlyPreoptArtBootImage": False,
    "HasSystemOther": False,
}
for key, value in expected.items():
    if config.get(key) is not value:
        raise SystemExit(
            f"generated dexpreopt config has {key}={config.get(key)!r}, "
            f"expected {value!r}"
        )
print(f"verified generated dexpreopt policy: {config_path}")
PY
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--aosp-source)
		[[ $# -ge 2 ]] || fail "--aosp-source requires a path"
		AOSP_SOURCE=$2
		shift 2
		;;
	--out)
		[[ $# -ge 2 ]] || fail "--out requires a path"
		OUT_DIR=$2
		shift 2
		;;
	--kernel-artifact)
		[[ $# -ge 2 ]] || fail "--kernel-artifact requires a path"
		KERNEL_ARTIFACT=$2
		shift 2
		;;
	--jobs)
		[[ $# -ge 2 ]] || fail "--jobs requires a positive integer"
		JOBS=$2
		shift 2
		;;
	--build-number)
		[[ $# -ge 2 ]] || fail "--build-number requires a positive integer"
		BUILD_NUMBER_VALUE=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[[ -n "$AOSP_SOURCE" ]] || fail "--aosp-source is required"
[[ -n "$OUT_DIR" ]] || fail "--out is required"
[[ -n "$KERNEL_ARTIFACT" ]] || fail "--kernel-artifact is required"
absolute_path "AOSP source" "$AOSP_SOURCE"
absolute_path "output directory" "$OUT_DIR"
absolute_path "kernel artifact" "$KERNEL_ARTIFACT"
[[ "$(uname -s)" == Linux ]] || fail "builder host must be Linux"
[[ "$(uname -m)" == x86_64 ]] || fail "builder host must be x86_64"
command -v zip >/dev/null 2>&1 || fail "zip is not available on PATH"
command -v python3 >/dev/null 2>&1 || fail "python3 is not available on PATH"
[[ -d "$AOSP_SOURCE/.repo/manifests" ]] || fail "AOSP Repo metadata is missing"
[[ -d "$KERNEL_ARTIFACT" && ! -L "$KERNEL_ARTIFACT" ]] ||
	fail "kernel artifact must be a regular directory: $KERNEL_ARTIFACT"
KERNEL_ELF="$KERNEL_ARTIFACT/kernel/vmlinux"
KERNEL_CONFIG="$KERNEL_ARTIFACT/kernel/config"
KERNEL_RELEASE_FILE="$KERNEL_ARTIFACT/kernel/kernel.release"
if [[ -n "$BUILD_NUMBER_VALUE" ]]; then
	[[ "$BUILD_NUMBER_VALUE" =~ ^[1-9][0-9]{0,9}$ ]] ||
		fail "the build number must be a positive integer of at most ten digits"
fi
case "$OUT_DIR" in
"$AOSP_SOURCE"/*) ;;
*) fail "output directory must be inside the AOSP workspace for this source tag" ;;
esac
if [[ -z "$JOBS" ]]; then
	JOBS=$(getconf _NPROCESSORS_ONLN)
fi
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "jobs must be a positive integer"
[[ -f "$METADATA" ]] || fail "AOSP metadata is missing"
[[ -x "$VERIFIER" ]] || fail "AOSP patch verifier is missing"
[[ -x "$MESA_VENUS_VERIFIER" ]] || fail "Mesa Venus source verifier is missing"
[[ -x "$TARGET_FILES_VERIFIER" ]] || fail "target-files verifier is missing"
[[ -f "$FIRST_BOOT_TOOL" ]] || fail "first-boot precompile tool is missing"
[[ -f "$KERNEL_ARTIFACT_VERIFIER" ]] || fail "kernel artifact verifier is missing"
# shellcheck disable=SC1090
source "$METADATA"

"$MESA_VENUS_VERIFIER" --android-root "$ANDROID_ROOT" \
	--aosp-source "$AOSP_SOURCE"
"$VERIFIER" --android-root "$ANDROID_ROOT" \
	--aosp-source "$AOSP_SOURCE" --expect final
python3 -B "$KERNEL_ARTIFACT_VERIFIER" --artifact "$KERNEL_ARTIFACT" \
	--common-root "$KERNEL_COMMON_ROOT" \
	--kernel-metadata "$OUTER_ROOT/kernel/mainline-6.18/patches/metadata.env" \
	--abi-metadata "$KERNEL_COMMON_ROOT/abi/identity.env"
SOONG_BUILD_GRAPH="$OUT_DIR/soong/build.${LINEAGE24_PRODUCT}.ninja"
mkdir -p -- "$OUT_DIR"
# One output directory per build variant: a switch between user and userdebug is
# then incremental, where reusing one directory would rebuild everything. The
# directory records the variant it was first used for and refuses the other.
VARIANT_MARK=$OUT_DIR/.lineage24-build-variant
if [[ -f "$VARIANT_MARK" ]]; then
	[[ "$(<"$VARIANT_MARK")" == "$LINEAGE24_BUILD_VARIANT" ]] ||
		fail "output directory holds a $(<"$VARIANT_MARK") build: use one directory per build variant"
else
	printf '%s\n' "$LINEAGE24_BUILD_VARIANT" >"$VARIANT_MARK"
fi
if [[ -f "$SOONG_BUILD_GRAPH" ]]; then
	printf '%s\n' "reusing incremental AOSP output: $OUT_DIR"
else
	printf '%s\n' "bootstrapping one-time AOSP output: $OUT_DIR"
fi
OUT_DIR_ABSOLUTE=$OUT_DIR
# This tag's test_package path validation treats absolute generated paths as
# module source paths. The verified suffix names the same in-tree directory
# while keeping Soong's generated paths relative to the Repo root.
OUT_DIR=${OUT_DIR#"$AOSP_SOURCE"/}
export OUT_DIR
export SOONG_NINJA=ninja
if [[ -n "$BUILD_NUMBER_VALUE" ]]; then
	export BUILD_NUMBER=$BUILD_NUMBER_VALUE
fi
# Do not force a Soong analysis mode here. The pinned build environment owns
# that choice, and changing modes on a populated OUT_DIR invalidates otherwise
# reusable intermediates.

CDPATH= cd -- "$AOSP_SOURCE"
# shellcheck disable=SC1091
source build/envsetup.sh
lunch "${LINEAGE24_PRODUCT}-${LINEAGE24_RELEASE_CONFIG}-${LINEAGE24_BUILD_VARIANT}"

[[ "$(get_build_var TARGET_PRODUCT)" == "$LINEAGE24_PRODUCT" ]] ||
	fail "lunch selected the wrong product"
[[ "$(get_build_var TARGET_BUILD_VARIANT)" == "$LINEAGE24_BUILD_VARIANT" ]] ||
	fail "lunch selected the wrong build variant"
[[ "$(get_build_var TARGET_ARCH)" == arm64 ]] || fail "product is not arm64"
# The second architecture uses the softfp armeabi-v7a ABI and its armeabi
# fallback. Both use the same arm runtime; IGNORE_PREFER32_ON_DEVICE keeps
# native daemons 64-bit. Applications run from zygote64_32.
[[ "$(get_build_var TARGET_2ND_ARCH)" == arm ]] || fail "product has no arm secondary architecture"
[[ "$(get_build_var TARGET_2ND_ARCH_VARIANT)" == armv8-a ]] ||
	fail "secondary architecture variant is not armv8-a"
[[ "$(get_build_var TARGET_2ND_CPU_VARIANT)" == cortex-a53 ]] ||
	fail "secondary CPU variant is not cortex-a53"
[[ "$(get_build_var TARGET_2ND_CPU_ABI)" == armeabi-v7a ]] ||
	fail "secondary ABI is not armeabi-v7a"
[[ "$(get_build_var TARGET_2ND_CPU_ABI2)" == armeabi ]] ||
	fail "secondary ABI fallback is not armeabi"
[[ "$(get_build_var TARGET_SUPPORTS_32_BIT_APPS)" == true ]] ||
	fail "product does not support 32-bit applications"
[[ -z "$(get_build_var ZYGOTE_FORCE_64)" ]] ||
	fail "ZYGOTE_FORCE_64 would leave no 32-bit ABI"
[[ "$(get_build_var IGNORE_PREFER32_ON_DEVICE)" == true ]] ||
	fail "native daemons must stay 64-bit (IGNORE_PREFER32_ON_DEVICE)"
[[ "$(get_build_var TARGET_MAX_PAGE_SIZE_SUPPORTED)" == "$LINEAGE24_MAX_PAGE_SIZE_SUPPORTED" ]] ||
	fail "product maximum page size differs from metadata"
[[ "$(get_build_var TARGET_NO_BIONIC_PAGE_SIZE_MACRO)" == true ]] ||
	fail "product exposes a compile-time bionic page-size macro"
[[ "$(get_build_var TARGET_CHECK_PREBUILT_MAX_PAGE_SIZE)" == true ]] ||
	fail "product does not check prebuilt ELF alignment"
[[ "$(get_build_var TARGET_BOOTS_16K)" != true ]] ||
	fail "product selects the 16 KiB boot contract on a 4 KiB kernel"
# The board is the device's own, not Cuttlefish's, and the SoC identity is the
# product's odm property file (overlay/config/odm.prop). The target-files
# verifier checks the properties the images carry.
[[ "$(get_build_var TARGET_BOOTLOADER_BOARD_NAME)" == andarium ]] ||
	fail "the board must be named andarium, not the inherited Cuttlefish board"
[[ " $(get_build_var TARGET_ODM_PROP) " == *" device/kernarium/andarium/config/odm.prop "* ]] ||
	fail "TARGET_ODM_PROP must name the product's SoC identity file"
for variable in \
	BOARD_SYSTEMIMAGE_FILE_SYSTEM_TYPE \
	BOARD_SYSTEM_EXTIMAGE_FILE_SYSTEM_TYPE \
	BOARD_PRODUCTIMAGE_FILE_SYSTEM_TYPE \
	BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE \
	BOARD_ODMIMAGE_FILE_SYSTEM_TYPE; do
	[[ "$(get_build_var "$variable")" == erofs ]] ||
		fail "$variable must select erofs for the immutable GPT"
done
# The compressor and the cluster size are the two parameters of lineage_andarium.mk
# and reach every image alike: nothing per partition, no extended option, and
# the default 4 KiB block that the guest's 4 KiB pages require.
[[ "$(get_build_var BOARD_EROFS_COMPRESSOR)" == "$(get_build_var UML_ANDROID_EROFS_COMPRESSOR)" &&
	"$(get_build_var BOARD_EROFS_COMPRESSOR)" == zstd,* ]] ||
	fail "BOARD_EROFS_COMPRESSOR must be the product's zstd compressor"
[[ "$(get_build_var BOARD_EROFS_PCLUSTER_SIZE)" == "$(get_build_var UML_ANDROID_EROFS_PCLUSTER_SIZE)" &&
	"$(get_build_var BOARD_EROFS_PCLUSTER_SIZE)" =~ ^[0-9]+$ ]] ||
	fail "BOARD_EROFS_PCLUSTER_SIZE must be the product's cluster size"
[[ "$(get_build_var BOARD_EROFS_COMPRESS_HINTS)" == device/kernarium/andarium/config/erofs_compress_hints.txt ]] ||
	fail "BOARD_EROFS_COMPRESS_HINTS must be the product's hints file"
for variable in \
	BOARD_EROFS_BLOCKSIZE \
	BOARD_EROFS_SHARE_DUP_BLOCKS \
	BOARD_EROFS_USE_LEGACY_COMPRESSION \
	BOARD_EROFS_ENABLE_DEDUPE \
	BOARD_SYSTEMIMAGE_EROFS_BLOCKSIZE; do
	[[ -z "$(get_build_var "$variable")" ]] ||
		fail "$variable must be empty: the images use mkfs.erofs defaults beyond the two parameters"
done
for partition in SYSTEM SYSTEM_EXT PRODUCT VENDOR ODM; do
	for suffix in COMPRESSOR COMPRESS_HINTS PCLUSTER_SIZE BLOCKSIZE ENABLE_DEDUPE; do
		[[ -z "$(get_build_var "BOARD_${partition}IMAGE_EROFS_${suffix}")" ]] ||
			fail "BOARD_${partition}IMAGE_EROFS_${suffix} must be empty: one compressor for every partition"
	done
done
# Every APEX payload is EROFS with the partitions' compressor and cluster size.
[[ "$(get_build_var PRODUCT_DEFAULT_APEX_PAYLOAD_TYPE)" == erofs ]] ||
	fail "PRODUCT_DEFAULT_APEX_PAYLOAD_TYPE must select erofs"
[[ "$(get_build_var PRODUCT_DEFAULT_APEX_PAYLOAD_EROFS_COMPRESSOR)" == "$(get_build_var UML_ANDROID_EROFS_COMPRESSOR)" ]] ||
	fail "the APEX payload compressor must be the product's"
[[ "$(get_build_var PRODUCT_DEFAULT_APEX_PAYLOAD_EROFS_PCLUSTER_SIZE)" == "$(get_build_var UML_ANDROID_EROFS_PCLUSTER_SIZE)" ]] ||
	fail "the APEX payload cluster size must be the product's"
[[ "$(get_build_var PRODUCT_DEFAULT_APEX_PAYLOAD_EROFS_COMPRESS_HINTS)" == device/kernarium/andarium/config/erofs_apex_payload_hints.txt ]] ||
	fail "the APEX payload compress hints must be the product's"
[[ "$(get_build_var PRODUCT_COMPRESSED_APEX)" == false ]] ||
	fail "PRODUCT_COMPRESSED_APEX must be false: the payloads are compressed, not the files"
for variable in \
	PRODUCT_USE_DYNAMIC_PARTITIONS \
	PRODUCT_USE_DYNAMIC_PARTITION_SIZE \
	PRODUCT_BUILD_SUPER_PARTITION \
	PRODUCT_BUILD_SUPER_EMPTY_IMAGE \
	PRODUCT_BUILD_SYSTEM_OTHER_IMAGE \
	PRODUCT_VIRTUAL_AB_OTA \
	PRODUCT_VIRTUAL_AB_COMPRESSION \
	PRODUCT_OTA_FORCE_NON_AB_PACKAGE \
	AB_OTA_UPDATER \
	BOARD_AVB_ENABLE; do
	[[ "$(get_build_var "$variable")" == false ]] ||
		fail "$variable must be false for the fixed UBD product"
done
[[ -z "$(get_build_var BOARD_USES_SYSTEM_OTHER_ODEX)" ]] ||
	fail "BOARD_USES_SYSTEM_OTHER_ODEX must be empty without a system_other image"
for variable in \
	PRODUCT_VIRTUAL_AB_COMPRESSION_METHOD \
	PRODUCT_VIRTUAL_AB_COMPRESSION_FACTOR \
	BOARD_SUPER_PARTITION_SIZE \
	BOARD_SUPER_PARTITION_GROUPS \
	AB_OTA_PARTITIONS \
	AB_OTA_POSTINSTALL_CONFIG; do
	[[ -z "$(get_build_var "$variable")" ]] ||
		fail "$variable must be empty for the fixed UBD product"
done
[[ "$(get_build_var TARGET_SKIP_OTA_PACKAGE)" == true ]] ||
	fail "TARGET_SKIP_OTA_PACKAGE must be true for the fixed UBD product"
# The board file's partition sizes are the image builder's limits: it refuses a
# larger image. The release packs each image at its own size, so they are
# ceilings and not a layout.
for variable in \
	BOARD_SYSTEMIMAGE_PARTITION_SIZE \
	BOARD_SYSTEM_EXTIMAGE_PARTITION_SIZE \
	BOARD_PRODUCTIMAGE_PARTITION_SIZE \
	BOARD_VENDORIMAGE_PARTITION_SIZE \
	BOARD_ODMIMAGE_PARTITION_SIZE \
	BOARD_METADATAIMAGE_PARTITION_SIZE \
	BOARD_USERDATAIMAGE_PARTITION_SIZE \
	BOARD_INIT_BOOT_IMAGE_PARTITION_SIZE \
	BOARD_RECOVERYIMAGE_PARTITION_SIZE; do
	[[ "$(get_build_var "$variable")" =~ ^[1-9][0-9]*$ ]] ||
		fail "$variable must state the image builder's size limit"
done
[[ "$(get_build_var TARGET_NO_RECOVERY)" == false ]] ||
	fail "TARGET_NO_RECOVERY must be false for the recovery partition"
[[ "$(get_build_var PRODUCT_BUILD_RECOVERY_IMAGE)" == true ]] ||
	fail "PRODUCT_BUILD_RECOVERY_IMAGE must be true"
[[ "$(get_build_var BOARD_EXCLUDE_KERNEL_FROM_RECOVERY_IMAGE)" == true ]] ||
	fail "recovery must use the separately built Android kernel"
[[ "$(get_build_var BOARD_USES_FULL_RECOVERY_IMAGE)" == true ]] ||
	fail "recovery must remain a full standalone partition image"
[[ "$(get_build_var BOARD_BOOT_HEADER_VERSION)" == 4 ]] ||
	fail "recovery must use boot image header v4"
[[ "$(get_build_var BOARD_INIT_BOOT_HEADER_VERSION)" == 4 ]] ||
	fail "init_boot must use boot image header v4"
for property in $(get_build_var PRODUCT_VENDOR_PROPERTIES); do
	case "$property" in
	ro.virtual_ab.*) fail "Virtual A/B property remains enabled: $property" ;;
	external_storage.projid.enabled=*)
		fail "project-quota property remains enabled: $property"
		;;
	esac
done
[[ -z "$(get_build_var PRODUCT_QUOTA_PROJID)" ]] ||
	fail "PRODUCT_QUOTA_PROJID must be empty for the UML userdata filesystem"
for variable in \
	PRODUCT_BUILD_SYSTEM_DLKM_IMAGE \
	PRODUCT_BUILD_VENDOR_DLKM_IMAGE \
	PRODUCT_BUILD_ODM_DLKM_IMAGE; do
	[[ "$(get_build_var "$variable")" == false ]] ||
		fail "$variable must be false for the built-in-only kernel"
done
for variable in \
	BOARD_USES_SYSTEM_DLKMIMAGE \
	BOARD_USES_VENDOR_DLKMIMAGE \
	BOARD_USES_ODM_DLKMIMAGE \
	BOARD_SYSTEM_DLKMIMAGE_FILE_SYSTEM_TYPE \
	BOARD_VENDOR_DLKMIMAGE_FILE_SYSTEM_TYPE \
	BOARD_ODM_DLKMIMAGE_FILE_SYSTEM_TYPE; do
	[[ -z "$(get_build_var "$variable")" ]] ||
		fail "$variable must be empty for the built-in-only kernel"
done
for variable in \
	BOARD_GENERIC_RAMDISK_KERNEL_MODULES \
	BOARD_GENERIC_RAMDISK_KERNEL_MODULES_LOAD \
	BOARD_RECOVERY_KERNEL_MODULES \
	BOARD_VENDOR_RAMDISK_KERNEL_MODULES \
	BOARD_VENDOR_KERNEL_MODULES \
	BOARD_SYSTEM_KERNEL_MODULES \
	BOARD_ODM_KERNEL_MODULES \
	BOARD_KERNEL_MODULES_16K; do
	[[ -z "$(get_build_var "$variable")" ]] ||
		fail "$variable must be empty for the built-in-only kernel"
done

[[ "$(get_build_var TRUSTY_SYSTEM_VM)" == disabled ]] ||
	fail "TRUSTY_SYSTEM_VM must be disabled for the UML guest"
PRODUCT_PACKAGES=" $(get_build_var PRODUCT_PACKAGES) "
[[ "$PRODUCT_PACKAGES" == *" android.hardware.sensors-service.multihal "* ]] ||
	fail "the Android motion sensor service is absent"
[[ "$PRODUCT_PACKAGES" == *" android.hardware.sensors@2.1-impl.andarium "* ]] ||
	fail "the Andarium motion sensor backend is absent"
for package in \
	android.hardware.health-service.example \
	android.hardware.health-service.example_recovery \
	com.android.bt \
	com.android.devicelock \
	com.android.hardware.gatekeeper.nonsecure \
	com.android.hardware.keymint.rust_nonsecure \
	com.android.nfcservices \
	com.android.rkpd \
	com.android.telephonycore \
	com.android.tethering \
	com.android.uwb \
	com.android.virt \
	com.android.wifi \
	e2fsck \
	Camelot \
	Etar \
	ExactCalculator \
	Glimpse \
	init.zygote64_32.rc \
	LatinIME \
	Launcher3QuickStep \
	LineageParts \
	LineageSetupWizard \
	LineageSettingsProvider \
	resize2fs \
	Seedvault \
	ThemePicker \
	Twelve; do
	[[ "$PRODUCT_PACKAGES" == *" $package "* ]] ||
		fail "required UML product package is absent: $package"
done
# The curated LineageOS application set leaves out the updater and the apps
# for hardware this machine lacks. LineageOS's switches (no camera, no AudioFX,
# no charger, no fastbootd) keep most out of the package set; the rest are
# excluded by name.
for package in \
	Aperture \
	AudioFX \
	Canvas \
	F-Droid \
	F-DroidPrivilegedExtension \
	fastbootd \
	Jelly \
	lineage_charger_animation \
	lineage_charger_animation_vendor \
	Profiles \
	QuickAccessWallet \
	Recorder \
	Updater; do
	[[ "$PRODUCT_PACKAGES" != *" $package "* ]] ||
		fail "curated-out LineageOS package is present: $package"
done
if [[ "$LINEAGE24_BUILD_VARIANT" == user ]]; then
	[[ "$PRODUCT_PACKAGES" != *" su "* ]] ||
		fail "a user build must not carry su"
else
	[[ "$PRODUCT_PACKAGES" == *" su "* ]] ||
		fail "a debug build must carry su for the development tooling"
	[[ -z "$(get_build_var PRODUCT_NOT_DEBUGGABLE_IN_USERDEBUG)" ]] ||
		fail "a debug build must be debuggable"
fi
[[ "$PRODUCT_PACKAGES" == *" adb_root "* ]] ||
	fail "the LineageOS adb root service is absent"
[[ "$PRODUCT_PACKAGES" == *" suspend_blocker "* ]] ||
	fail "the suspend blocker that holds the guest awake for an adb host is absent"
[[ "$PRODUCT_PACKAGES" == *" kernarium_net_setup "* ]] ||
	fail "the network setup tool that configures eth0 and the virtual Wi-Fi is absent"
[[ "$PRODUCT_PACKAGES" == *" kernarium_wifi_network_script "* ]] ||
	fail "the script that saves the virtual Wi-Fi network is absent"
[[ "$PRODUCT_PACKAGES" == *" kernarium_reclaim_boot_apps_script "* ]] ||
	fail "the script that reclaims the cached boot-started apps is absent"
[[ "$PRODUCT_PACKAGES" == *" kernarium_time_zone_script "* ]] ||
	fail "the script that applies the host's time zone is absent"
[[ "$PRODUCT_PACKAGES" == *" com.android.hardware.wifi "* ]] ||
	fail "the Wi-Fi HAL that the virtual Wi-Fi needs is absent"
[[ "$PRODUCT_PACKAGES" == *" com.google.cf.wpa_supplicant "* ]] ||
	fail "the Wi-Fi supplicant that the virtual Wi-Fi needs is absent"
[[ "$PRODUCT_PACKAGES" == *" KernariumWifiOverlay "* ]] ||
	fail "the Wi-Fi overlay that turns MAC randomization off is absent"
[[ "$PRODUCT_PACKAGES" == *" KernariumPinnerOverlay "* ]] ||
	fail "the overlay that releases the home app's memory pin is absent"
[[ "$PRODUCT_PACKAGES" == *" SecureElement "* ]] ||
	fail "the OMAPI service that SEService clients bind on demand is absent"
[[ "$PRODUCT_PACKAGES" == *" EasterEgg "* ]] ||
	fail "the easter egg that the platform logo screen opens is absent"
[[ "$PRODUCT_PACKAGES" == *" com.android.hardware.thermal "* ]] ||
	fail "the example thermal HAL that the thermal manager needs is absent"
[[ "$PRODUCT_PACKAGES" == *" com.android.hardware.cas "* ]] ||
	fail "the default CAS HAL that MediaCas needs is absent"
[[ "$PRODUCT_PACKAGES" == *" libclearkeycasplugin "* ]] ||
	fail "the ClearKey CAS plugin that the CAS HAL loads is absent"
PRODUCT_PACKAGES_EXCLUDE=" $(get_build_var PRODUCT_PACKAGES_EXCLUDE) "
for package in \
	BasicDreams \
	BluetoothMidiService \
	Calendar \
	Camera2 \
	Contacts \
	CuttlefishService \
	DeskClock \
	DeviceDiagnostics \
	Dialer \
	DynamicSystemInstallationService \
	Gallery2 \
	QuickSearchBox \
	TeleService \
	Telecom \
	TelephonyProvider \
	com.android.compos \
	com.android.hardware.authsecret \
	com.android.hardware.graphics.composer.ranchu \
	android.hardware.health-service.cuttlefish_recovery \
	com.android.hardware.contexthub \
	com.android.hardware.input.processor \
	com.android.hardware.net.nlinterceptor \
	com.android.hardware.rebootescrow \
	com.android.hardware.security.authgraph \
	com.android.hardware.security.secretkeeper \
	com.android.hardware.tetheroffload \
	com.android.hardware.usb \
	com.android.vndk.v31 \
	com.android.vndk.v32 \
	com.android.vndk.v33 \
	com.android.vndk.v34 \
	com.google.cf.bt \
	com.google.cf.confirmationui \
	com.google.cf.health \
	com.google.cf.identity \
	com.google.cf.ir \
	com.google.cf.wifi \
	CuttlefishTetheringOverlay \
	CuttlefishWifiOverlay \
	gsi_tool \
	gsid \
	messaging \
	pmg_daemon \
	snapshotctl \
	snapuserd \
	snapuserd_ramdisk \
	socket_vsock_proxy \
	tombstone_transmit \
	update_engine \
	update_engine_client \
	update_verifier \
	wificond \
	wificond_compat_symlink_module; do
	[[ "$PRODUCT_PACKAGES_EXCLUDE" == *" $package "* ]] ||
		fail "unneeded product package is not excluded: $package"
	[[ "$PRODUCT_PACKAGES" != *" $package "* ]] ||
		fail "excluded product package remains resolved: $package"
done

[[ -z "$(get_build_var PRODUCT_EXTRA_VNDK_VERSIONS)" ]] ||
	fail "PRODUCT_EXTRA_VNDK_VERSIONS must be empty for the fixed vendor image"
PRODUCT_VENDOR_PROPERTIES=" $(get_build_var PRODUCT_VENDOR_PROPERTIES) "
[[ "$PRODUCT_VENDOR_PROPERTIES" != *" ro.rebootescrow.device="* ]] ||
	fail "the unsupported reboot-escrow backing property remains present"
for property in \
	ro.hardware.audio.tinyalsa.period_count=4 \
	ro.hardware.audio.tinyalsa.period_size_multiplier=4; do
	[[ "$PRODUCT_VENDOR_PROPERTIES" == *" $property "* ]] ||
		fail "required UML audio property is absent: $property"
done

PRODUCT_PRODUCT_PROPERTIES=" $(get_build_var PRODUCT_PRODUCT_PROPERTIES) "
for property in \
	aaudio.mmap_policy=1 \
	aaudio.mmap_exclusive_policy=1; do
	[[ "$PRODUCT_PRODUCT_PROPERTIES" == *" $property "* ]] ||
		fail "required UML audio property is absent: $property"
done
[[ "$PRODUCT_PRODUCT_PROPERTIES" == *" ro.lmk.gpu_reclaim=true "* ]] ||
	fail "required lmkd property is absent: ro.lmk.gpu_reclaim=true"
[[ "$(get_build_var LOCAL_USE_VENDOR_AUDIO_CONFIGURATION)" == true ]] ||
	fail "the UML product must own its minimal primary audio policy"
for package in \
	com.android.hardware.gatekeeper.cf_remote \
	com.android.hardware.keymint.rust_cf_guest_trusty_nonsecure \
	com.android.hardware.keymint.rust_cf_remote \
	trusty_security_vm.elf \
	trusty_security_vm_launcher \
	trusty_security_vm_launcher.rc; do
	[[ "$PRODUCT_PACKAGES" != *" $package "* ]] ||
		fail "unsupported Trusty or remote security package is present: $package"
done
[[ "$PRODUCT_PACKAGES" != *" dlkm_loader "* ]] ||
	fail "dlkm_loader is forbidden for the built-in-only kernel"
for package in \
	android.hardware.boot-service.default_recovery \
	checkpoint_gc \
	com.android.hardware.boot \
	update_engine_sideload; do
	[[ "$PRODUCT_PACKAGES" != *" $package "* ]] ||
		fail "Cuttlefish A/B package is forbidden for the fixed UBD product: $package"
done
for package in \
	ThreadNetworkDemoApp \
	android.hardware.sensors@2.1-impl.cuttlefish \
	com.android.hardware.neuralnetworks \
	com.android.hardware.threadnetwork \
	com.android.hardware.uwb \
	com.google.cf.light \
	com.google.cf.oemlock \
	cuttlefish_overlay_uwb \
	cuttlefish_overlay_uwb_gsi \
	cuttlefish_sensor_injection; do
	[[ "$PRODUCT_PACKAGES" != *" $package "* ]] ||
		fail "unsupported Cuttlefish HAL package is present: $package"
done

# target-files always carries a kernel input. Stage only the published Android
# artifact's runtime image in the output tree; the product's init_boot and
# recovery images deliberately exclude it, and no AOSP source project owns a
# copy. The build's own vmlinux keeps the DWARF that pahole reads and would not
# fit the boot image.
PRODUCT_OUT=$(get_build_var PRODUCT_OUT)
case "$PRODUCT_OUT" in
"$OUT_DIR"/*) ;;
*) fail "PRODUCT_OUT is outside the selected output directory: $PRODUCT_OUT" ;;
esac
mkdir -p -- "$PRODUCT_OUT"
if ! cmp -s -- "$KERNEL_ELF" "$PRODUCT_OUT/kernel"; then
	install -m 0755 -- "$KERNEL_ELF" "$PRODUCT_OUT/kernel"
fi

# publish-release.py consumes the packaged VINTF checker from this output, and
# signs with sign_target_files_apks and the tools it runs, so a fresh output
# builds those host tools.
# First boot compiles nothing. Soong preopts the applications it installs but
# never one inside an APEX, so ART Service would compile them on every fresh
# /data. After a first pass, the host dex2oat compiles them with the flags and
# class loader context ART Service uses on the device; a second pass installs
# the artifacts through the product's PRODUCT_COPY_FILES fragment. An unchanged
# output skips the second pass.
FIRST_BOOT_OAT=$OUT_DIR/first-boot-oat
if [[ -f "$FIRST_BOOT_OAT/first-boot-oat.mk" ]]; then
	export LINEAGE24_FIRST_BOOT_OAT_MK=$FIRST_BOOT_OAT/first-boot-oat.mk
fi
m -j"$JOBS" target-files-package check_target_files_vintf \
	sign_target_files_apks otatools
rm -rf -- "$FIRST_BOOT_OAT.new"
python3 -B "$FIRST_BOOT_TOOL" build --aosp-source "$AOSP_SOURCE" --out-dir "$OUT_DIR" \
	--product "$(basename -- "$PRODUCT_OUT")" --output "$AOSP_SOURCE/$FIRST_BOOT_OAT.new" \
	--previous "$AOSP_SOURCE/$FIRST_BOOT_OAT" --jobs "$JOBS"
if [[ -d "$FIRST_BOOT_OAT" ]] && diff -r -q -- "$FIRST_BOOT_OAT" "$FIRST_BOOT_OAT.new" >/dev/null; then
	rm -rf -- "$FIRST_BOOT_OAT.new"
else
	rm -rf -- "$FIRST_BOOT_OAT"
	mv -- "$FIRST_BOOT_OAT.new" "$FIRST_BOOT_OAT"
	export LINEAGE24_FIRST_BOOT_OAT_MK=$FIRST_BOOT_OAT/first-boot-oat.mk
	m -j"$JOBS" target-files-package
fi
DEXPREOPT_CONFIG="$OUT_DIR_ABSOLUTE/soong/dexpreopt-${LINEAGE24_PRODUCT}.config"
verify_dexpreopt_config "$DEXPREOPT_CONFIG"
TARGET_FILES="$PRODUCT_OUT/obj/PACKAGING/target_files_intermediates/${LINEAGE24_PRODUCT}-target_files.zip"
[[ -f "$TARGET_FILES" ]] || fail "target-files archive is missing: $TARGET_FILES"
python3 -B "$FIRST_BOOT_TOOL" verify --host-bin "$OUT_DIR_ABSOLUTE/host/linux-x86/bin" \
	--target-files-dir "${TARGET_FILES%.zip}" --artifacts "$AOSP_SOURCE/$FIRST_BOOT_OAT"
"$TARGET_FILES_VERIFIER" --target-files "$TARGET_FILES" \
	--kernel-elf "$KERNEL_ELF" \
	--kernel-config "$KERNEL_CONFIG" \
	--kernel-release-file "$KERNEL_RELEASE_FILE"
HOST_BIN="$OUT_DIR_ABSOLUTE/host/linux-x86/bin"
VINTF_CHECKER="$HOST_BIN/check_target_files_vintf"
[[ -x "$VINTF_CHECKER" ]] || fail "packaged VINTF checker is missing: $VINTF_CHECKER"
PATH="$HOST_BIN:/usr/bin:/bin" "$VINTF_CHECKER" "$TARGET_FILES"
[[ -e "$HOST_BIN/sign_target_files_apks" ]] ||
	fail "AOSP release signing tool is missing: $HOST_BIN/sign_target_files_apks"
"$VERIFIER" --android-root "$ANDROID_ROOT" \
	--aosp-source "$AOSP_SOURCE" --expect final
printf '%s\n' "built AOSP 17 UML target files under: $OUT_DIR_ABSOLUTE"
