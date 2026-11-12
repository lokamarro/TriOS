#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import functools
import hashlib
import io
import re
import stat
import struct
import sys
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parent))

import android_image  # noqa: E402
import default_wallpaper  # noqa: E402
from release_common import (  # noqa: E402
    BUILD_VARIANTS,
    OTA_CERTIFICATE_ARCHIVES,
    configured_build_variant,
    ota_certificate_member,
    sha256_path,
)


ET_EXEC = 2
ET_DYN = 3
MAX_PROGRAM_HEADERS_SIZE = 1 << 20
APEX_PAYLOAD_MAGIC_SIZE = android_image.EROFS_SUPERBLOCK_OFFSET + android_image.EROFS_SUPERBLOCK_SIZE
EXT4_MAGIC_OFFSET = 1024 + 0x38
EXT4_MAGIC = b"\x53\xef"
# The one APEX whose payload stays ext4: Soong installs the checked-in presigned CTS shim
# (system/apex/shim/prebuilts), which no product setting recompresses.
APEX_EXT4_PAYLOADS = frozenset({"SYSTEM/apex/com.android.apex.cts.shim.apex"})
# The Mesa Virgl GLES stack: the EGL and GLES libraries the platform loader
# finds through ro.hardware.egl, and the gallium driver they link. The boot
# command line's androidboot.hardware.egl must name the same suffix.
MESA_EGL_SUFFIX = "kernarium"
MESA_GALLIUM_DRIVER = "venus_gallium_dri.so"
# Both userspace ABIs load the stack: a 64-bit application finds it in lib64,
# and an armeabi-v7a one in lib. The ABI directory decides the ELF class
# check_mesa_gles_stack requires.
MESA_ABI_DIRECTORIES = {"lib64": android_image.EM_AARCH64, "lib": android_image.EM_ARM}
# The hardware video codecs: the vendor Codec2 service of external/v4l2_codec2
# and the components it serves, two encoders and two decoders. The codec list
# names exactly those, beside the updates to the software decoders' limits. The
# properties enable those components, admit one instance per stream the host
# serves, and state the pixel rate the host sustains, which the service reports
# as its capacity and which no published performance point may exceed.
VIDEO_ENCODER_SERVICE = "android.hardware.media.c2-service-v4l2"
VIDEO_ENCODER_CODECS = {
    "c2.v4l2.avc.encoder": "video/avc",
    "c2.v4l2.hevc.encoder": "video/hevc",
}
VIDEO_ENCODER_PROPERTIES = {
    "ro.vendor.v4l2_codec2.encoder.supported.h264": "true",
    "ro.vendor.v4l2_codec2.encoder.supported.hevc": "true",
    "ro.vendor.v4l2_codec2.encode_concurrent_instances": "8",
    "ro.vendor.v4l2_codec2.video_pixel_rate": "108000000",
}
VIDEO_DECODER_CODECS = {
    "c2.v4l2.avc.decoder": "video/avc",
    "c2.v4l2.hevc.decoder": "video/hevc",
}
VIDEO_DECODER_PROPERTY_PREFIX = "ro.vendor.v4l2_codec2.decoder.supported."
VIDEO_DECODER_PROPERTIES = {
    "ro.vendor.v4l2_codec2.decoder.supported.h264": "true",
    "ro.vendor.v4l2_codec2.decoder.supported.hevc": "true",
    "ro.vendor.v4l2_codec2.decode_concurrent_instances": "4",
    "ro.vendor.v4l2_codec2.video_decode_pixel_rate": "456192000",
}
# CTS demands that the use of the largest published performance point be at least this share of
# the capacity (VideoEncoderAvailabilityTest and VideoDecoderAvailabilityTest, resource
# consumption for performance points), so a capacity cannot be far above what the codec publishes.
VIDEO_MIN_CAPACITY_SHARE = 0.6
# The smallest picture and the lowest bit rate of the encoders: the floors that the host lists
# (the platform's kVideoEncodeMinimumSide and the bit rate control's minimum), below which its
# streams fail CTS's quality bar. The codec list states exactly them.
VIDEO_ENCODER_MINIMUM_SIZE = "64x64"
VIDEO_ENCODER_MINIMUM_BITRATE = "20000"
VIDEO_ENCODER_UEVENTD = "/dev/video* 0660 system camera"
# The service reads this policy beside its default one. The poller of the V4L2
# device wakes its thread through an eventfd, which the default lacks.
VIDEO_ENCODER_SECCOMP_CALLS = ("eventfd2",)
# The crash handler of debuggerd runs in the service when it faults.
VIDEO_CRASH_HANDLER_SECCOMP_CALLS = ("getppid", "recvfrom", "setsockopt", "uname")
VIDEO_ENCODER_SECCOMP_DEFAULT = "android.hardware.media.c2-default-seccomp_policy"
VIDEO_ENCODER_SECCOMP_EXTENDED = "android.hardware.media.c2-extended-seccomp_policy"
MESA_EGL_ENTRIES = {
    f"VENDOR/{abi}/egl/lib{name}_{MESA_EGL_SUFFIX}.so"
    for abi in MESA_ABI_DIRECTORIES
    for name in ("EGL", "GLESv1_CM", "GLESv2")
}
MESA_ELF_ENTRIES = {
    *(f"VENDOR/{abi}/{MESA_GALLIUM_DRIVER}" for abi in MESA_ABI_DIRECTORIES),
    *MESA_EGL_ENTRIES,
}
# The Vulkan HAL and the buffer allocator and mapper Mesa queries through
# gralloc: same-process HALs of each ABI.
MESA_HAL_ENTRIES = {
    f"VENDOR/{abi}/hw/{module}.so"
    for abi in MESA_ABI_DIRECTORIES
    for module in ("vulkan.virtio", "gralloc.minigbm", "mapper.minigbm")
}
# A 32-bit library of the stack must be built with -D_FILE_OFFSET_BITS=64, or
# its mmap of a DRM offset above 4 GiB (the Venus renderer's, the buffer
# allocator's) passes a 32-bit off_t and fails with EINVAL. Bionic's headers
# then rename these calls to their 64 suffix. The rule covers this stack alone:
# the platform's own 32-bit libraries import plain mmap and lseek for
# offsets that never leave 32 bits.
LARGE_FILE_FUNCTIONS = ("mmap", "lseek")
# The graphics HAL selections. The product init script copies each
# androidboot.hardware.<name> into ro.hardware.<name> at early-init, so the
# boot command line is each property's one writer and must name these values;
# a vendor property would make that setprop fail.
BOOT_HAL_SELECTIONS = {
    "egl": MESA_EGL_SUFFIX,
    "vulkan": "virtio",
    "gralloc": "minigbm",
}
# The OpenGL ES claim. The product init script copies the boot command line's
# androidboot.opengles.version into ro.opengles.version at early-init, the
# version the framework reports and the CTS gates its dEQP lists on; 0x30002
# is OpenGL ES 3.2. Every ES 3.1 or later context of the driver lists
# GL_ANDROID_extension_pack_es31a, and the CTS requires the
# android.hardware.opengles.aep feature exactly when a device that reports 3.1
# or later lists it. A declared dEQP level makes the CTS run every EGL and GLES
# list dated up to it, so the level is pinned to the one whose lists passed
# whole: a platform update that moves the "latest" file fails here instead of
# raising the claim unqualified.
BOOT_OPENGLES_VERSION = 0x30002
OPENGLES_DEQP_LEVEL_FEATURE = "android.software.opengles.deqp.level"
OPENGLES_DEQP_LEVEL = 132776705  # 2026-03-01
UML_INIT_OPENGLES_COPY = "setprop ro.opengles.version ${ro.boot.opengles.version}"
# The Venus HAL makes vkAllocateMemory wait for the host's reply, so a refusal of the host's GPU
# budget reaches the application as VK_ERROR_OUT_OF_DEVICE_MEMORY instead of failing the ring and
# aborting the process. Mesa reads its VN_PERF option as this property; vendor init sets it at
# early-init, before any process that loads the driver starts, and only there. The driver loads in
# applications, SurfaceFlinger and programs run from the shell (the CTS among them), and each must
# be able to read the property, or Mesa reads no option and allocates asynchronously.
VENUS_PERF_PROPERTY = "debug.mesa.vn.perf"
VENUS_PERF_COPY = f"setprop {VENUS_PERF_PROPERTY} no_async_mem_alloc"
VENUS_DRIVER_DOMAINS = ("untrusted_app", "surfaceflinger", "shell")
# The retired AOSP external/mesa3d stack: its EGL and GLES libraries, and the
# directory it loaded its driver from.
RETIRED_EGL_ENTRIES = {
    f"VENDOR/{abi}/egl/{name}.so"
    for abi in MESA_ABI_DIRECTORIES
    for name in ("libGLES_mesa", "libEGL_mesa", "libGLESv1_CM_mesa", "libGLESv2_mesa")
}
RETIRED_DRI_DIRECTORIES = tuple(f"VENDOR/{abi}/dri/" for abi in MESA_ABI_DIRECTORIES)
REQUIRED_SDK_APEX_ENTRIES = {
    "SYSTEM/apex/com.android.bt.apex",
    "SYSTEM/apex/com.android.devicelock.apex",
    "SYSTEM/apex/com.android.nfcservices.apex",
    "SYSTEM/apex/com.android.rkpd.apex",
    "SYSTEM/apex/com.android.telephonycore.apex",
    "SYSTEM/apex/com.android.tethering.apex",
    "SYSTEM/apex/com.android.uwb.apex",
    "SYSTEM/apex/com.android.virt.apex",
    "SYSTEM/apex/com.android.wifi.apex",
}

# The product runs 64-bit applications and 32-bit armeabi-v7a or legacy
# armeabi applications. The build writes each ABI list per partition
# (ro.<partition>.product.cpu.abilist*) and the runtime derives the unprefixed
# ones from them.
ABI_PROPERTY = re.compile(
    r"ro\.(?:(?:system|vendor|odm)\.product|product)\.cpu\.(abi|abi2|abilist|abilist32|abilist64)"
)
EXPECTED_ABIS = {
    "abi": "arm64-v8a",
    "abi2": "",
    "abilist": "arm64-v8a,armeabi-v7a,armeabi",
    "abilist32": "armeabi-v7a,armeabi",
    "abilist64": "arm64-v8a",
}
# Partitions whose build properties list the ABIs, with the properties a
# multilib product must carry in them.
ABI_LIST_PROPERTY_FILES = {
    "SYSTEM/build.prop": "system",
    "VENDOR/build.prop": "vendor",
    "ODM/etc/build.prop": "odm",
}
# What only the vendor properties describe: the second zygote that starts the
# 32-bit applications, the 64-bit compiler ART Service runs for both ISAs, and
# the second architecture's variant for dex2oat and bionic.
VENDOR_MULTILIB_PROPERTIES = {
    "ro.zygote": "zygote64_32",
    "dalvik.vm.dex2oat64.enabled": "true",
    "ro.bionic.2nd_arch": "arm",
    "ro.bionic.2nd_cpu_variant": "cortex-a53",
    "dalvik.vm.isa.arm.variant": "cortex-a53",
}


REQUIRED_ENTRIES = {
    "VENDOR/bin/hw/android.hardware.sensors-service.multihal",
    "VENDOR/etc/sensors/hals.conf",
    "VENDOR/etc/vintf/manifest/android.hardware.sensors-multihal.xml",
    "VENDOR/lib64/hw/android.hardware.sensors@2.1-impl.andarium.so",
    "VENDOR/etc/init/android.hardware.sensors-service-multihal.rc",

    "BOOT/kernel",
    "INIT_BOOT/RAMDISK/init",
    "IMAGES/init_boot.img",
    "IMAGES/odm.img",
    "IMAGES/product.img",
    "IMAGES/recovery.img",
    "IMAGES/system.img",
    "IMAGES/system_ext.img",
    "IMAGES/userdata.img",
    "IMAGES/vendor.img",
    "META/init_boot_filesystem_config.txt",
    "META/kernel_configs.txt",
    "META/kernel_version.txt",
    "META/misc_info.txt",
    "META/ramdisk_node_list",
    "PRODUCT/app/Camelot/Camelot.apk",
    "PRODUCT/app/Etar/Etar.apk",
    "PRODUCT/app/ExactCalculator/ExactCalculator.apk",
    "PRODUCT/app/Glimpse/Glimpse.apk",
    "PRODUCT/app/LatinIME/LatinIME.apk",
    "PRODUCT/app/Twelve/Twelve.apk",
    "PRODUCT/app/webview/webview.apk",
    "PRODUCT/etc/build.prop",
    "SYSTEM/build.prop",
    "RECOVERY/RAMDISK/system/bin/hw/android.hardware.health-service.example_recovery",
    "RECOVERY/RAMDISK/system/bin/recovery",
    "SYSTEM/bin/e2fsck",
    "SYSTEM/bin/resize2fs",
    "SYSTEM_EXT/priv-app/Launcher3QuickStep/Launcher3QuickStep.apk",
    "SYSTEM_EXT/priv-app/LineageParts/LineageParts.apk",
    "SYSTEM_EXT/priv-app/LineageSetupWizard/LineageSetupWizard.apk",
    "SYSTEM_EXT/priv-app/LineageSettingsProvider/LineageSettingsProvider.apk",
    "SYSTEM_EXT/priv-app/Seedvault/Seedvault.apk",
    "SYSTEM_EXT/bin/kernarium_wifi_network.sh",
    "SYSTEM_EXT/etc/init/kernarium_wifi_network.rc",
    "SYSTEM_EXT/bin/kernarium_reclaim_boot_apps.sh",
    "SYSTEM_EXT/etc/init/kernarium_reclaim_boot_apps.rc",
    "SYSTEM_EXT/bin/kernarium_time_zone.sh",
    "SYSTEM_EXT/etc/init/kernarium_time_zone.rc",
    "SYSTEM_EXT/etc/selinux/system_ext_file_contexts",
    "SYSTEM_EXT/etc/selinux/system_ext_property_contexts",
    "SYSTEM_EXT/etc/selinux/system_ext_sepolicy.cil",
    "SYSTEM_EXT/priv-app/ThemePicker/ThemePicker.apk",
    "SYSTEM/etc/selinux/plat_property_contexts",
    "SYSTEM/etc/selinux/plat_sepolicy.cil",
    "VENDOR/bin/hw/android.hardware.health-service.example",
    "VENDOR/bin/kernarium_net_setup",
    "VENDOR/bin/suspend_blocker",
    "VENDOR/etc/audio_effects_config.xml",
    "VENDOR/etc/audio_policy_configuration.xml",
    "VENDOR/etc/audio_policy_volumes.xml",
    "VENDOR/etc/default_volume_tables.xml",
    "VENDOR/etc/fstab.uml",
    "VENDOR/etc/init/init.kernarium.rc",
    f"VENDOR/bin/hw/{VIDEO_ENCODER_SERVICE}",
    f"VENDOR/etc/init/{VIDEO_ENCODER_SERVICE}.rc",
    "VENDOR/etc/media_codecs_c2.xml",
    f"VENDOR/etc/seccomp_policy/{VIDEO_ENCODER_SECCOMP_DEFAULT}",
    f"VENDOR/etc/seccomp_policy/{VIDEO_ENCODER_SECCOMP_EXTENDED}",
    f"VENDOR/etc/vintf/manifest/{VIDEO_ENCODER_SERVICE}.xml",
    "VENDOR/lib64/libc2plugin_store.so",
    "VENDOR/etc/permissions/bluetooth_excluded_hardware.xml",
    "VENDOR/etc/permissions/handheld_core_hardware.xml",
    "VENDOR/etc/primary_audio_policy_configuration.xml",
    "VENDOR/etc/r_submix_audio_policy_configuration.xml",
    "VENDOR/etc/selinux/vendor_file_contexts",
    "VENDOR/etc/selinux/vendor_property_contexts",
    "VENDOR/etc/selinux/vendor_sepolicy.cil",
    "VENDOR/etc/ueventd.rc",
    "VENDOR/etc/vintf/compatibility_matrix.xml",
    "VENDOR/etc/vintf/manifest.xml",
    "VENDOR_BOOT/RAMDISK/first_stage_ramdisk/fstab.uml",
    "VENDOR/build.prop",
    # The network setup tool's netlink library (Cuttlefish's).
    "VENDOR/lib64/cuttlefish_net.so",
    # The Wi-Fi module's overlay: virt_wifi cannot randomize its MAC.
    "VENDOR/overlay/KernariumWifiOverlay.apk",
    # The overlay that releases PinnerService's home app pin.
    "VENDOR/overlay/KernariumPinnerOverlay.apk",
    *ABI_LIST_PROPERTY_FILES,
    *MESA_ELF_ENTRIES,
    *MESA_HAL_ENTRIES,
    *REQUIRED_SDK_APEX_ENTRIES,
    # vold executes sgdisk to scan and partition any disk it manages; without
    # it every such operation fails with ENOENT.
    "SYSTEM/bin/sgdisk",
    # The 32-bit runtime the product adds beside the 64-bit one: the second
    # zygote's process, the ART boot image of its ISA, and the bootstrap
    # linker that starts it.
    "SYSTEM/bin/app_process32",
    "SYSTEM/bin/bootstrap/linker",
    "SYSTEM/bin/boringssl_self_test32",
    "SYSTEM/framework/arm/boot.oat",
    "SYSTEM/framework/arm/boot-framework.oat",
    "SYSTEM/etc/init/hw/init.zygote64_32.rc",
}
REQUIRED_PREOPT_APKS = {
    "PRODUCT/app/LatinIME/LatinIME.apk",
    "SYSTEM/app/CaptivePortalLogin/CaptivePortalLogin.apk",
    "SYSTEM/app/PartnerBookmarksProvider/PartnerBookmarksProvider.apk",
    "SYSTEM/app/PrintRecommendationService/PrintRecommendationService.apk",
    "SYSTEM/priv-app/BlockedNumberProvider/BlockedNumberProvider.apk",
    "SYSTEM/priv-app/ContactsProvider/ContactsProvider.apk",
    "SYSTEM/priv-app/CredentialManager/CredentialManager.apk",
    "SYSTEM/priv-app/E2eeContactKeysProvider/E2eeContactKeysProvider.apk",
    "SYSTEM/priv-app/IntentResolver/IntentResolver.apk",
    "SYSTEM/priv-app/LiveWallpapersPicker/LiveWallpapersPicker.apk",
    "SYSTEM/priv-app/LocalTransport/LocalTransport.apk",
    "SYSTEM/priv-app/ManagedProvisioning/ManagedProvisioning.apk",
    "SYSTEM/priv-app/PackageInstaller/PackageInstaller.apk",
    "SYSTEM/priv-app/ProxyHandler/ProxyHandler.apk",
    "SYSTEM/priv-app/Shell/Shell.apk",
    "SYSTEM/priv-app/SoundPicker/SoundPicker.apk",
    "SYSTEM/priv-app/Tag/Tag.apk",
    "SYSTEM/priv-app/UserDictionaryProvider/UserDictionaryProvider.apk",
    "SYSTEM/priv-app/VpnDialogs/VpnDialogs.apk",
    "SYSTEM_EXT/priv-app/Launcher3QuickStep/Launcher3QuickStep.apk",
    "SYSTEM_EXT/priv-app/LineageParts/LineageParts.apk",
    "SYSTEM_EXT/priv-app/LineageSetupWizard/LineageSetupWizard.apk",
    "SYSTEM_EXT/priv-app/Seedvault/Seedvault.apk",
    "SYSTEM_EXT/priv-app/Settings/Settings.apk",
    "SYSTEM_EXT/priv-app/StorageManager/StorageManager.apk",
    "SYSTEM_EXT/priv-app/SystemUI/SystemUI.apk",
    "SYSTEM_EXT/priv-app/ThemePicker/ThemePicker.apk",
}
# The one application whose WebView renderer and in-process library run under
# either zygote: it carries libraries for both ABIs and declares multiArch.
REQUIRED_ARM_PREOPT_APKS = {
    "PRODUCT/app/webview/webview.apk",
}
APP_ISAS = {"arm64", "arm"}
DLKM_COMPONENTS = {"system_dlkm", "vendor_dlkm", "odm_dlkm"}
DLKM_COMPATIBILITY_ENTRIES = {
    "RECOVERY/RAMDISK/odm_dlkm",
    "RECOVERY/RAMDISK/odm_dlkm/etc",
    "RECOVERY/RAMDISK/system_dlkm",
    "RECOVERY/RAMDISK/vendor_dlkm",
    "RECOVERY/RAMDISK/vendor_dlkm/etc",
    "ROOT/odm_dlkm",
    "ROOT/odm_dlkm/etc",
    "ROOT/system_dlkm",
    "ROOT/vendor_dlkm",
    "ROOT/vendor_dlkm/etc",
    "SYSTEM/system_dlkm",
    "SYSTEM/system_dlkm/etc",
    "SYSTEM/system_dlkm/etc/build.prop",
    "VENDOR/odm_dlkm",
    "VENDOR/odm_dlkm/etc",
    "VENDOR/odm_dlkm/etc/build.prop",
    "VENDOR/vendor_dlkm",
    "VENDOR/vendor_dlkm/etc",
    "VENDOR/vendor_dlkm/etc/build.prop",
    "VENDOR_BOOT/RAMDISK/odm_dlkm",
    "VENDOR_BOOT/RAMDISK/odm_dlkm/etc",
    "VENDOR_BOOT/RAMDISK/system_dlkm",
    "VENDOR_BOOT/RAMDISK/vendor_dlkm",
    "VENDOR_BOOT/RAMDISK/vendor_dlkm/etc",
}
TARGET_PARTITIONS = {"SYSTEM", "SYSTEM_EXT", "PRODUCT", "VENDOR", "ODM"}
# The one 32-bit executable that does not carry the 32 stem suffix of a binary
# built for both ABIs: the bootstrap linker that starts the first processes.
ELF32_BIN_EXCEPTIONS = {"SYSTEM/bin/bootstrap/linker"}
FORBIDDEN_STATIC_ENTRIES = {
    "IMAGES/super.img",
    "IMAGES/super_empty.img",
    "IMAGES/system_other.img",
    "IMAGES/vbmeta.img",
    "IMAGES/vbmeta_system.img",
    "IMAGES/vbmeta_vendor.img",
    "META/ab_partitions.txt",
    "PRODUCT/app/QuickSearchBox/QuickSearchBox.apk",
    "PRODUCT/etc/init/init.rc",
    "PRODUCT/etc/permissions/privapp-permissions-org.fdroid.fdroid.privileged.xml",
    "PRODUCT/overlay/cuttlefish_overlay_uwb.apk",
    "SYSTEM_EXT/etc/permissions/privapp-permissions-com.android.threadnetwork.demoapp.xml",
    "SYSTEM_EXT/priv-app/ThreadNetworkDemoApp/ThreadNetworkDemoApp.apk",
    "SYSTEM/bin/wificond",
    "VENDOR/apex/com.android.hardware.boot.apex",
    "VENDOR/apex/com.android.hardware.contexthub.apex",
    "VENDOR/apex/com.android.hardware.neuralnetworks.apex",
    "VENDOR/apex/com.android.hardware.threadnetwork.apex",
    "VENDOR/apex/com.android.hardware.uwb.apex",
    "VENDOR/apex/com.google.cf.bt.apex",
    "VENDOR/apex/com.google.cf.health.apex",
    "VENDOR/apex/com.google.cf.light.apex",
    "VENDOR/apex/com.google.cf.nfc.apex",
    "VENDOR/apex/com.google.cf.oemlock.apex",
    "VENDOR/apex/com.google.cf.rild.apex",
    "VENDOR/apex/com.google.cf.wifi.apex",
    "VENDOR/etc/bluetooth_with_le_audio_policy_configuration_7_0.xml",
    "VENDOR/etc/init/init_graphics.vendor.rc",
    "VENDOR/etc/init/seriallogging.rc",
    "VENDOR/etc/permissions/android.hardware.ethernet.xml",
    "VENDOR/etc/permissions/android.hardware.uwb.xml",
    "VENDOR/etc/recovery-resource.dat",
    "VENDOR/lib64/hw/android.hardware.sensors@2.1-impl.cuttlefish.so",
    "VENDOR/overlay/CuttlefishTetheringOverlay.apk",
    "VENDOR/overlay/CuttlefishWifiOverlay.apk",
    "VENDOR/overlay/cuttlefish_overlay_uwb_gsi.apk",
    "VENDOR/bin/install-recovery.sh",
    "VENDOR/etc/recovery.img",
    "VENDOR/recovery-from-boot.p",
    "VENDOR/ueventd.rc",
    "SYSTEM/vendor/bin/install-recovery.sh",
    "SYSTEM/vendor/etc/recovery.img",
    "SYSTEM/vendor/etc/recovery-resource.dat",
    "SYSTEM/vendor/recovery-from-boot.p",
}
FORBIDDEN_APEX_FILENAMES = {
    "com.android.hardware.authsecret.apex",
    "com.android.hardware.graphics.composer.ranchu.apex",
    "com.android.hardware.input.processor.apex",
    "com.android.hardware.net.nlinterceptor.apex",
    "com.android.hardware.rebootescrow.apex",
    "com.android.hardware.security.authgraph.apex",
    "com.android.hardware.security.secretkeeper.apex",
    "com.android.hardware.tetheroffload.apex",
    "com.android.hardware.usb.apex",
    "com.android.vndk.v31.apex",
    "com.android.vndk.v32.apex",
    "com.android.vndk.v33.apex",
    "com.android.vndk.v34.apex",
    "com.google.cf.confirmationui.apex",
    "com.google.cf.identity.apex",
    "com.google.cf.ir.apex",
}
FORBIDDEN_EXECUTABLE_FILENAMES = {
    "fastbootd",
    "gsi_tool",
    "gsid",
    "pmgd",
    "snapshotctl",
    "snapuserd",
    "socket_vsock_proxy",
    "tombstone_transmit",
    "update_engine",
    "update_engine_client",
    "update_verifier",
}
FORBIDDEN_APP_DIRECTORIES = {
    "Aperture",
    "AudioFX",
    "BasicDreams",
    "BluetoothMidiService",
    "Calendar",
    "Camera2",
    "Canvas",
    "Contacts",
    "CuttlefishService",
    "DeskClock",
    "DeviceDiagnostics",
    "Dialer",
    "DynamicSystemInstallationService",
    "EmergencyInfo",
    "F-Droid",
    "F-DroidPrivilegedExtension",
    "Gallery2",
    "Jelly",
    # In-tree test applications (tests/integration/android/{motion,webgl,storage}):
    # `m <module>` leaves them in the shared product output, and none belongs
    # in an image, whichever partition or DATA/app path it would reach.
    "AndariumMotionProbe",
    "AndariumStorageTests",
    "AndariumWebGLProbe",
    "Profiles",
    # Setup Wizard owns provisioning; the AOSP auto-provisioner would bypass OOBE.
    "Provision",
    "QuickAccessWallet",
    "QuickSearchBox",
    "Recorder",
    "TeleService",
    "Telecom",
    "TelephonyProvider",
    "Updater",
    "messaging",
}
FORBIDDEN_FEATURE_DECLARATIONS = {
    "android.hardware.bluetooth.xml",
    "android.hardware.bluetooth_le.xml",
    "android.hardware.camera.concurrent.xml",
    "android.hardware.camera.flash-autofocus.xml",
    "android.hardware.camera.front.xml",
    "android.hardware.camera.full.xml",
    "android.hardware.camera.raw.xml",
    "android.hardware.ethernet.xml",
    "android.hardware.fingerprint.xml",
    "android.hardware.location.gps.xml",
    "android.hardware.telephony.xml",
    "android.hardware.usb.accessory.xml",
    "android.hardware.usb.host.xml",
    "android.hardware.uwb.xml",
    "android.hardware.wifi.aware.xml",
    "android.hardware.wifi.direct.xml",
    "android.hardware.wifi.passpoint.xml",
    "android.hardware.wifi.rtt.xml",
    "android.software.verified_boot.xml",
}
# The Virtio sound capture stream is the product's microphone. A build
# fingerprint cannot report it only on some boots, so the declaration is
# permanent (CDD 7.8.1 C-0-7) and the audio policy carries its input.
# system_server starts the CompanionDeviceManager service only with
# android.software.companion_device_setup, which the handheld declaration the
# product replaces used to carry; the service runs without Bluetooth.
# The kernel's virt_wifi is a station, which is android.hardware.wifi and no more:
# there is no peer-to-peer, Aware or RTT, and Passpoint waits for the first Wi-Fi
# CTS run. android.hardware.ethernet is not declared, because eth0 is the management
# NIC and the framework must not manage it.
# system_server starts MidiService only with android.software.midi, which no
# handheld declaration of the platform carries, and applications call
# MidiManager without a null check. Virtual MIDI devices (MidiDeviceService)
# need no hardware transport, so the declaration adds no USB or Bluetooth MIDI.
REQUIRED_FEATURE_NAMES = {
    "android.hardware.touchscreen",
    "android.hardware.touchscreen.multitouch",
    "android.hardware.touchscreen.multitouch.distinct",
    "android.hardware.touchscreen.multitouch.jazzhand",
    "android.hardware.sensor.accelerometer",
    "android.hardware.sensor.gyroscope",
    "android.hardware.microphone",
    "android.hardware.wifi",
    "android.software.companion_device_setup",
    "android.software.midi",
    "android.hardware.opengles.aep",
}
FORBIDDEN_FEATURE_NAMES = {
    "android.hardware.bluetooth",
    "android.hardware.bluetooth_le",
    "android.hardware.camera",
    "android.hardware.camera.any",
    "android.hardware.camera.autofocus",
    "android.hardware.camera.flash",
    "android.hardware.camera.front",
    "android.hardware.context_hub",
    "android.hardware.ethernet",
    "android.hardware.fingerprint",
    "android.hardware.location.gps",
    "android.hardware.telephony",
    "android.hardware.usb.accessory",
    "android.hardware.usb.host",
    "android.hardware.uwb",
    "android.hardware.wifi.aware",
    "android.hardware.wifi.direct",
    "android.hardware.wifi.passpoint",
    "android.hardware.wifi.rtt",
}
UML_FSTAB_ENTRIES = (
    "VENDOR/etc/fstab.uml",
    "VENDOR_BOOT/RAMDISK/first_stage_ramdisk/fstab.uml",
)
READ_ONLY_FILESYSTEMS = {
    "system_fs_type": "erofs",
    "system_ext_fs_type": "erofs",
    "product_fs_type": "erofs",
    "vendor_fs_type": "erofs",
    "odm_fs_type": "erofs",
}
READ_ONLY_PARTITIONS = ("system", "system_ext", "product", "vendor", "odm")
# The images use mkfs.erofs's defaults apart from the product's compressor and
# cluster size: no other EROFS policy may be set, for a partition or for all.
EROFS_FORBIDDEN_POLICY = (
    "erofs_blocksize",
    "erofs_enable_dedupe",
    "erofs_share_dup_blocks",
    "erofs_use_legacy_compression",
)
EROFS_PARTITION_POLICY = re.compile(
    "^(?:" + "|".join(READ_ONLY_PARTITIONS) + ")_erofs_[a-z_]+$"
)
EROFS_COMPRESS_HINTS_ENTRY = "META/erofs_default_compress_hints.txt"
VENDOR_SPL_APP_READ = re.compile(
    rb"^\(allow appdomain(?:_[0-9]+)? vendor_security_patch_level_prop(?:_[0-9]+)? "
    rb"\(file \([^)]*\bread\b[^)]*\)\)\)$",
    re.MULTILINE,
)
# The image's inode count, which the first-boot resize keeps per block group
# (BOARD_USERDATAIMAGE_EXTFS_INODE_COUNT in overlay/BoardConfig.mk).
USERDATA_EXTFS_INODE_COUNT = "8192"
# 3840x2160 at 30 frames per second, in the blocks that the codec limits count: HEVC counts
# 8x8 blocks (480 x 270 = 129,600, 3,888,000 per second), VP9 16x16 blocks (240 x 135 = 32,400).
MEDIA_C2_HEVC_BLOCKS_PER_SECOND = 129_600 * 30
MEDIA_C2_VP9_BLOCK_COUNT = 32_400
MEDIA_C2_SIZE = (3840, 2160)
# The ClearKey DRM HAL maps the shared buffers of applications, which are memfd files labelled with
# the application's domain on a kernel without ashmem (overlay/sepolicy/vendor/hal_drm_clearkey.te).
# The compiled rule names the application domains less the isolated ones through a generated
# attribute, a set expression of the form (and (appdomain) (not (isolated_app_all))).
CLEARKEY_MEMFD_RULE = re.compile(
    rb"^\(allow hal_drm_clearkey_aidl(?:_[0-9]+)? (\S+) \(memfd_file \(([^)]*)\)\)\)$",
    re.MULTILINE,
)
CLEARKEY_MEMFD_PERMISSIONS = frozenset({"getattr", "map", "read", "write"})
# The two example audio HAL processes share one domain and call each other's binder
# (overlay/sepolicy/vendor/hal_audio_default.te).
AUDIO_HAL_SELF_BINDER = re.compile(
    rb"^\(allow hal_audio_default(?:_[0-9]+)? hal_audio_default(?:_[0-9]+)? "
    rb"\(binder \([^)]*\bcall\b[^)]*\)\)\)$",
    re.MULTILINE,
)
# The Wi-Fi HAL reads the platform's persist.vendor.debug.wifi.* properties at start-up
# (overlay/sepolicy/vendor/hal_wifi_default.te).
WIFI_HAL_DEBUG_PROPERTY_READ = re.compile(
    rb"^\(allow hal_wifi_default(?:_[0-9]+)? persist_vendor_debug_wifi_prop(?:_[0-9]+)? "
    rb"\(file \([^)]*\bread\b[^)]*\)\)\)$",
    re.MULTILINE,
)
# adbd reports its host in this property and init holds a userspace wakelock while
# it is 1 (adbd's p0011 topic, overlay/config/init.vendor.rc,
# overlay/sepolicy/system_ext/private). The property is system-owned: adbd is a core
# domain, which the platform neverallow keeps from setting a vendor-owned property.
ADB_HOST_PROPERTY = "sys.kernarium.adb.host_connected"
ADB_HOST_PROPERTY_TYPE = "kernarium_adb_host_prop"
# The host application is the adb host of its guest and reaches adbd over virtio-vsock. adbd's p0011
# topics read two launch properties themselves: ro.boot.kernarium.adb_key, the public key the
# application authorizes for the boot, and ro.boot.kernarium.adb_tcp, 0 when the launch has no
# host forward, which keeps adbd off guest TCP. The launch properties are bootloader_prop, which
# every domain may read; service.adb.listen_addrs would not do, as it is a system_prop that
# vendor_init may not set. The adbd binary sits in its APEX, whose payload hints keep it
# uncompressed so that the verifier reads these names in the payload's bytes.
ADBD_APEX = "SYSTEM/apex/com.android.adbd.apex"
ADBD_BINARY = "bin/adbd"
ADBD_LAUNCH_PROPERTIES = ("ro.boot.kernarium.adb_key", "ro.boot.kernarium.adb_tcp")
# The key rule of p0011 reads the guest's debugging setting: AdbService records USB debugging in
# persist.sys.usb.config (route p0007) and wireless debugging is persist.adb.tls_server.enable. adbd
# offers any key but the host's only while one is on, and logs the refusal line otherwise; the line
# is what only the guard topic adds, as stock adbd already holds the property names.
ADBD_DEBUGGING_PROPERTIES = ("persist.sys.usb.config", "persist.adb.tls_server.enable")
ADBD_REFUSAL_MARKER = b"kernarium: key refused, debugging is off"
# A guest that boots without ADB authorization is an unattended test host, and the p0011 bypass
# topic starts LineageOS's adb_root service with rooted debugging on for it. Only a debuggable
# build can act on that: a user build has no init trigger that restarts adbd as root, so adbd would
# announce a restart that never comes and `adb root` would hang. The topic leaves the switch off on
# a build that is not debuggable and logs this line; it is what only the guarded topic adds.
ADB_ROOT_BINARY = "SYSTEM_EXT/bin/adb_root"
ADB_ROOT_GUARD_MARKER = b"kernarium: rooted debugging stays off, this build is not debuggable"
CIL_PLATFORM_VERSION_SUFFIX = re.compile(r"_[0-9]{6}$")
CIL_PROPERTY_SET_RULE = re.compile(
    rb"^\(allow (\S+) (\S+) \(property_service \(([^)]*)\)\)\)$", re.MULTILINE
)
CIL_FILE_ALLOW_RULE = re.compile(
    rb"^\(allow (\S+) (\S+) \(file \(([^)]*)\)\)\)$", re.MULTILINE
)
# An attribute's set: a list of names, or a set expression such as
# (and (domain) (not (appdomain coredomain))).
CIL_TYPE_ATTRIBUTE_SET = re.compile(rb"^\(typeattributeset (\S+) (\(.*\))\)$", re.MULTILINE)
CIL_SET_OPERATORS = frozenset({"all", "and", "not", "or", "xor"})
INIT_PROPERTY_TRIGGER = re.compile(r"property:([^=\s&]+)=")
# The property_contexts of every partition, in the order libselinux merges them.
PROPERTY_CONTEXT_FILES = (
    "SYSTEM/etc/selinux/plat_property_contexts",
    "SYSTEM_EXT/etc/selinux/system_ext_property_contexts",
    "PRODUCT/etc/selinux/product_property_contexts",
    "VENDOR/etc/selinux/vendor_property_contexts",
    "ODM/etc/selinux/odm_property_contexts",
)
CIL_DONTAUDIT_RULE = re.compile(rb"^\(dontaudit (\S+) (\S+) \((\w+) \(([^)]*)\)\)\)$", re.MULTILINE)
CIL_ALLOW_RULE = re.compile(rb"^\(allow (\S+) (\S+) \((\w+) \(([^)]*)\)\)\)$", re.MULTILINE)
CIL_PERMISSIVE_TYPE = re.compile(rb"^\(typepermissive (\S+)\)$", re.MULTILINE)
# The only domain that may be permissive, and only in the diagnostics build: the userdebug
# root shell. A user build has none.
PERMISSIVE_DOMAINS = {"user": frozenset(), "userdebug": frozenset({"su"})}
# What only a debuggable build installs (PRODUCT_PACKAGES_DEBUG, adbd's debuggable block): the
# root shell, `remount` and `procmem`. A user build must carry none of them; a userdebug build
# must carry the root shell the diagnostics need.
DEBUG_ONLY_ENTRIES = ("SYSTEM/xbin/su", "SYSTEM/bin/remount", "SYSTEM/bin/procmem")
DEBUG_REQUIRED_ENTRIES = ("SYSTEM/xbin/su",)
SUSPEND_BLOCKER_SERVICE = ("suspend_blocker", "/vendor/bin/suspend_blocker")
# eth0 is the management NIC adb is reached on and the framework sees only the
# virtual Wi-Fi built on it. The tool that sets this up runs at post-fs-data, after
# init has limited every interface to answering ARP for its own addresses
# (overlay/config/init.vendor.rc, tools/kernarium_net_setup.cpp).
NET_SETUP_SERVICE = ("kernarium_net_setup", "/vendor/bin/kernarium_net_setup")
NET_SETUP_TRIGGER = "post-fs-data"
NET_SETUP_SYSCTLS = (
    "write /proc/sys/net/ipv4/conf/all/arp_ignore 1",
    "write /proc/sys/net/ipv4/conf/all/arp_announce 2",
)
# The Wi-Fi HAL and the supplicant keep their sockets in these directories, which Cuttlefish's
# own init script creates and this product's replaces.
WIFI_DATA_DIRECTORIES = (
    "mkdir /data/vendor/wifi 0770 wifi wifi",
    "mkdir /data/vendor/wifi/wpa 0770 wifi wifi",
    "mkdir /data/vendor/wifi/wpa/sockets 0770 wifi wifi",
)
NET_SETUP_TRANSITION = re.compile(
    rb"^\(typetransition init(?:_[0-9]+)? kernarium_net_setup_exec process kernarium_net_setup\)$",
    re.MULTILINE,
)
# After boot completes a script saves the virtual Wi-Fi network (VirtWifi, open) the
# first time only, in a core domain of its own on system_ext
# (overlay/tools/kernarium_wifi_network.sh, overlay/sepolicy/system_ext/private).
# It never turns Wi-Fi on or connects: the user's switch decides whether the saved
# network is joined, and a network the user forgets stays forgotten.
WIFI_NETWORK_SERVICE = ("kernarium_wifi_network", "/system_ext/bin/kernarium_wifi_network.sh")
WIFI_NETWORK_TRIGGER = "property:sys.boot_completed=1"
WIFI_NETWORK_MARKER = "persist.sys.kernarium.virtwifi_saved"
WIFI_NETWORK_MARKER_TYPE = "kernarium_wifi_network_prop"
WIFI_NETWORK_SCRIPT_REQUIRED = ("cmd wifi add-network VirtWifi open", WIFI_NETWORK_MARKER, "setprop")
WIFI_NETWORK_SCRIPT_FORBIDDEN = (
    "set-wifi-enabled",
    "connect-network",
    "settings put",
    "wifi_on",
    "svc wifi",
)
WIFI_NETWORK_TRANSITION = re.compile(
    rb"^\(typetransition init(?:_[0-9]+)? kernarium_wifi_network_exec process "
    rb"kernarium_wifi_network\)$",
    re.MULTILINE,
)
# Once boot has settled a script kills the cached processes of a reviewed list of apps the
# system starts at boot (overlay/tools/kernarium_reclaim_boot_apps.sh), in a core domain of its
# own on system_ext (overlay/sepolicy/system_ext/private). Their receivers and bindings stay;
# only `cmd activity kill` runs on them, which kills nothing the system would not have chosen,
# and never `kill-all` or `force-stop`, which cancel alarms and jobs. Each entry is a package
# and the names of its processes.
RECLAIM_SERVICE = ("kernarium_reclaim_boot_apps", "/system_ext/bin/kernarium_reclaim_boot_apps.sh")
RECLAIM_TRIGGER = "property:sys.boot_completed=1"
RECLAIM_SETTLE = "settle=120"
RECLAIM_APPS = (
    ("android.ext.services", (".adservices", "android.ext.services")),
    ("com.android.federatedcompute.services", ("com.android.federatedcompute.services",)),
    ("com.android.ondevicepersonalization.services",
     ("com.android.ondevicepersonalization.services",)),
    ("com.stevesoltys.seedvault", ("com.stevesoltys.seedvault",)),
    ("org.calyxos.backup.contacts", ("org.calyxos.backup.contacts",)),
    ("com.android.localtransport", ("com.android.localtransport",)),
    ("com.android.rkpdapp", ("com.android.rkpdapp",)),
    ("com.android.statementservice", ("com.android.statementservice",)),
    ("com.android.healthconnect.controller", ("com.android.healthconnect.controller",)),
)
RECLAIM_SCRIPT_REQUIRED = (
    "dumpsys activity lru",
    'cmd activity kill --user 0 "$pkg"',
    RECLAIM_SETTLE,
)
RECLAIM_SCRIPT_FORBIDDEN = (
    "kill-all",
    "force-stop",
    "kill -9",
    "kill -KILL",
    "kill -s",
    "pm disable",
    "pm clear",
    "pm uninstall",
    "set-inactive",
    "stop-app",
)
RECLAIM_LIST = re.compile(r"^reclaim_list='\n(.*?)^'$", re.MULTILINE | re.DOTALL)
RECLAIM_TRANSITION = re.compile(
    rb"^\(typetransition init(?:_[0-9]+)? kernarium_reclaim_boot_apps_exec process "
    rb"kernarium_reclaim_boot_apps\)$",
    re.MULTILINE,
)
# Once boot completes a script applies the host's time zone, which the host application passes
# at every start (ro.boot.kernarium.timezone), through the platform's own setter, in a core domain
# of its own on system_ext (overlay/tools/kernarium_time_zone.sh, overlay/sepolicy/system_ext/
# private). The guest follows the host only while nobody in the guest has chosen a zone: the
# user's "Set time zone automatically" (Settings.Global.auto_time_zone, which the script reads and
# never writes) is not off, and the zone is still the one the script applied last, which it
# records in a marker property of its own. The platform has no automatic time zone here, so
# Settings has no switch and the zone pickers (Settings', the setup wizard's) are the only
# choices, neither of which turns the setting off. The domain sets no time zone property:
# system_server writes persist.sys.timezone.
TIME_ZONE_SERVICE = ("kernarium_time_zone", "/system_ext/bin/kernarium_time_zone.sh")
TIME_ZONE_TRIGGER = "property:sys.boot_completed=1"
TIME_ZONE_MARKER = "persist.sys.kernarium.timezone_applied"
TIME_ZONE_MARKER_TYPE = "kernarium_time_zone_prop"
TIME_ZONE_SCRIPT_REQUIRED = (
    "ro.boot.kernarium.timezone",
    "cmd settings get global auto_time_zone",
    'cmd alarm set-timezone "$zone"',
    TIME_ZONE_MARKER,
    'setprop "$marker" "$zone"',
)
TIME_ZONE_SCRIPT_FORBIDDEN = (
    "settings put",
    "setprop persist.sys.timezone",
    "time_zone_detector",
    "set_auto_detection",
    "svc ",
)
TIME_ZONE_TRANSITION = re.compile(
    rb"^\(typetransition init(?:_[0-9]+)? kernarium_time_zone_exec process "
    rb"kernarium_time_zone\)$",
    re.MULTILINE,
)
# The partition the host grew is filled on every boot, once /data is mounted
# (init.vendor.rc says why): the kernel's online resize needs neither a fsck nor
# a recorded check. resize2fs runs as root with the group of /data's owner, so
# it opens /data (mode 0771) without a DAC capability, which the platform
# neverallows its domain.
# The fsck domain runs resize2fs. The kernel's online resize asks for CAP_SYS_RESOURCE and for
# EXT4_IOC_RESIZE_FS on the mount point (overlay/sepolicy/vendor/fsck.te).
USERDATA_GROW_RULES = (
    ("self", "capability", frozenset({"sys_resource"})),
    ("system_data_root_file", "dir", frozenset({"open", "read", "ioctl"})),
)
USERDATA_GROW_IOCTL = "0x6610"
CIL_ALLOWX_RULE = re.compile(
    rb"^\(allowx (\S+) (\S+) \(ioctl (\w+) \(([^()]*)\)\)\)$", re.MULTILINE
)
USERDATA_GROW_COMMAND = "exec - root system -- /system/bin/resize2fs /dev/block/by-name/userdata"
USERDATA_GROW_TRIGGER = "post-fs-data"
USERDATA_DEVICE = "/dev/block/by-name/userdata"
# adbd keeps running for the host whatever the debugging setting: init starts it again after
# any stop.
ADBD_KEEP_ALIVE_TRIGGER = "property:init.svc.adbd=stopped"
ADBD_KEEP_ALIVE_COMMAND = "start adbd"
UML_INIT_REQUIRED = (
    "mount_all --early",
    "mount_all --late",
    "ro.hardware.audio.primary",
    "setprop service.adb.tcp.port 5555",
    "start adbd",
)
# Only Kernarium passes a display density, so the product init script copies
# it inside an action gated on its presence; an unconditional copy fails on
# every standalone macOS boot.
UML_INIT_DENSITY_TRIGGER = "early-init && property:ro.boot.lcd_density=*"
UML_INIT_DENSITY_COPY = "setprop ro.sf.lcd_density ${ro.boot.lcd_density}"
# The framework scales its ANR, watchdog and input timeouts by the hardware timeout
# multiplier, which every launcher passes from the guest's vCPU count; the tracked
# command line does not set it. The copy defaults to 5 for a launch that passes none,
# because an unset property leaves the framework's scale at 1.
UML_INIT_TIMEOUT_COPY = "setprop ro.hw_timeout_multiplier ${ro.boot.hw_timeout_multiplier:-5}"
UML_INIT_TIMEOUT_PROPERTY = "setprop ro.hw_timeout_multiplier "
# The boot command line expedites RCU grace periods until boot completes; the
# product init script then restores normal ones.
UML_INIT_RCU_TRIGGER = "property:sys.boot_completed=1"
UML_INIT_RCU_RESTORE = "write /sys/kernel/rcu_expedited 0"
# Compressed swap: one zram entry in the fstab, turned on once boot completes so
# that boot itself compresses nothing.
UML_ZRAM_DEVICE = "/dev/block/zram0"
UML_SWAPON_COMMAND = "swapon_all /vendor/etc/fstab.uml"
# The inherited Cuttlefish graphics script also copied the goldfish composer
# mode and the ANGLE feature overrides; this product has neither, and their
# androidboot sources never exist.
UML_INIT_FORBIDDEN = (
    "ro.vendor.hwcomposer.mode",
    "debug.angle.",
    "metrics_helper",
    "rename_eth0",
    "setup_wifi",
    "socket_vsock_proxy",
    "/dev/hvc",
    "/dev/block/by-name/custom",
)


def error(message: str) -> SystemExit:
    return SystemExit(f"Android target-files verifier: {message}")


def fail(message: str) -> None:
    raise error(message)


def sha256_member(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> str:
    digest = hashlib.sha256()
    with archive.open(info) as source:
        while chunk := source.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def safe_name(name: str) -> PurePosixPath:
    if not name or "\\" in name or "\0" in name:
        fail(f"unsafe archive path: {name!r}")
    path = PurePosixPath(name)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        fail(f"unsafe archive path: {name!r}")
    if str(path) != name:
        fail(f"non-normalized archive path: {name!r}")
    return path


def reject_module_payload(path: PurePosixPath) -> None:
    lowered = tuple(part.lower() for part in path.parts)
    if (any(part in DLKM_COMPONENTS for part in lowered)
            and str(path) not in DLKM_COMPATIBILITY_ENTRIES):
        fail(f"DLKM payload is forbidden: {path}")
    basename = lowered[-1]
    if basename == "dlkm_loader":
        fail(f"kernel module loader is forbidden: {path}")
    if android_image.is_module_payload(basename):
        fail(f"kernel module or module metadata is forbidden: {path}")
    if any(
        lowered[index : index + 2] == ("lib", "modules")
        for index in range(len(lowered) - 1)
    ):
        fail(f"kernel module directory is forbidden: {path}")


def reject_nonstatic_payload(path: PurePosixPath) -> None:
    canonical = str(path)
    if path.parts[0] == "SYSTEM_OTHER":
        fail(f"system_other payload is forbidden: {path}")
    if canonical in FORBIDDEN_STATIC_ENTRIES:
        fail(f"nonstatic image or metadata is forbidden: {path}")
    if path.name in FORBIDDEN_APEX_FILENAMES and "apex" in path.parts:
        fail(f"unneeded APEX payload is forbidden: {path}")
    if path.name in FORBIDDEN_EXECUTABLE_FILENAMES:
        fail(f"unneeded static-layout executable is forbidden: {path}")
    if path.parts[:2] == ("OTA", "bin"):
        fail(f"OTA tool payload is forbidden: {path}")
    if path.parts[0] == "IMAGES" and path.name.endswith("_dlkm.img"):
        fail(f"DLKM image is forbidden: {path}")
    if path.name.startswith("fstab.cf.") and path.parts[0] in {"VENDOR", "VENDOR_BOOT"}:
        fail(f"Cuttlefish fstab is forbidden: {path}")
    if any(part in FORBIDDEN_APP_DIRECTORIES for part in path.parts):
        fail(f"unneeded built-in application payload is forbidden: {path}")
    if path.name in FORBIDDEN_FEATURE_DECLARATIONS and "permissions" in path.parts:
        fail(f"absent hardware feature declaration is forbidden: {path}")


# The platform's compiled policy (plat_sepolicy.cil) is 3.4 MB; the bound leaves
# room for it to grow about fivefold. The vendor and system_ext policies are
# a few hundred kilobytes and keep the default bound.
PLATFORM_POLICY_LIMIT = 16 << 20


def read_small(
    archive: zipfile.ZipFile, info: zipfile.ZipInfo, limit: int = 1 << 20
) -> bytes:
    if info.is_dir() or info.file_size > limit:
        fail(f"policy file is missing or unexpectedly large: {info.filename}")
    return archive.read(info)


def check_apex_payload_filesystem(
    archive: zipfile.ZipFile, info: zipfile.ZipInfo
) -> None:
    """An APEX payload is EROFS compressed as the partitions are, or the prebuilt shim's ext4."""
    try:
        with zipfile.ZipFile(io.BytesIO(archive.read(info))) as apex:
            payload = apex.getinfo("apex_payload.img")
            with apex.open(payload) as source:
                prefix = source.read(APEX_PAYLOAD_MAGIC_SIZE)
    except (KeyError, OSError, zipfile.BadZipFile) as error:
        fail(f"cannot inspect APEX payload {info.filename}: {error}")
    if prefix[EXT4_MAGIC_OFFSET : EXT4_MAGIC_OFFSET + len(EXT4_MAGIC)] == EXT4_MAGIC:
        if info.filename in APEX_EXT4_PAYLOADS:
            return
        fail(f"APEX payload must use compressed erofs, not ext4: {info.filename}")
    if prefix[android_image.EROFS_SUPERBLOCK_OFFSET :][:4] != android_image.EROFS_MAGIC:
        fail(f"APEX payload must use compressed erofs: {info.filename}")
    check_erofs_superblock(f"APEX payload {info.filename}", prefix)


def init_services(payload: str) -> dict[str, tuple[str, list[str]]]:
    """Map each service of an init script to its executable and its option lines."""
    services: dict[str, tuple[str, list[str]]] = {}
    options: list[str] | None = None
    for raw in payload.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if fields[0] == "service" and len(fields) >= 3:
            options = []
            services[fields[1]] = (fields[2], options)
        elif fields[0] in ("on", "import"):
            options = None
        elif options is not None and raw[:1].isspace():
            options.append(" ".join(fields))
    return services


def check_suspend_blocker(init_payload: str, actions: dict[str, list[str]]) -> None:
    """The wakelock that keeps the guest awake runs exactly while an adb host is connected."""
    name, executable = SUSPEND_BLOCKER_SERVICE
    service = init_services(init_payload).get(name)
    if service is None or service[0] != executable:
        fail(f"UML init policy does not declare the {name} service at {executable}")
    if "disabled" not in service[1]:
        fail(f"UML init policy starts {name} with its class instead of on an adb host")
    for trigger, commands in actions.items():
        if f"start {name}" in commands and trigger != f"property:{ADB_HOST_PROPERTY}=1":
            fail(f"UML init policy starts {name} under '{trigger}'")
    if f"start {name}" not in actions.get(f"property:{ADB_HOST_PROPERTY}=1", []):
        fail(f"UML init policy does not start {name} while an adb host is connected")
    if f"stop {name}" not in actions.get(f"property:{ADB_HOST_PROPERTY}=0", []):
        fail(f"UML init policy does not stop {name} when the last adb host leaves")


def check_zram_swap(actions: dict[str, list[str]]) -> None:
    """Swap goes on when boot completes and at no other point."""
    if actions.get(UML_INIT_RCU_TRIGGER, []).count(UML_SWAPON_COMMAND) != 1:
        fail(f"UML init policy does not run '{UML_SWAPON_COMMAND}' once at {UML_INIT_RCU_TRIGGER}")
    for trigger, commands in actions.items():
        for command in commands:
            if command.startswith("swapon_all") and (
                trigger != UML_INIT_RCU_TRIGGER or command != UML_SWAPON_COMMAND
            ):
                fail(f"UML init policy turns swap on under '{trigger}': {command}")


def check_zram_fstab(fstab: bytes, label: str) -> None:
    """The fstab names one zram swap entry that sizes itself from the RAM."""
    entries = [
        fields
        for fields in (line.split() for line in fstab.decode("utf-8", errors="strict").splitlines())
        if fields and not fields[0].startswith("#") and fields[2:3] == ["swap"]
    ]
    if len(entries) != 1 or entries[0][0] != UML_ZRAM_DEVICE:
        fail(f"UML fstab must have exactly one swap entry, on {UML_ZRAM_DEVICE}: {label}")
    flags = entries[0][4].split(",") if len(entries[0]) > 4 else []
    if not any(flag.startswith("zramsize=") and flag.endswith("%") for flag in flags):
        fail(f"UML fstab swap entry has no zramsize percentage: {label}")


def check_userdata_grow(actions: dict[str, list[str]]) -> None:
    """Growing userdata works on every boot, not only the first, and happens while it is mounted."""
    if actions.get(USERDATA_GROW_TRIGGER, []).count(USERDATA_GROW_COMMAND) != 1:
        fail(f"UML init policy does not run '{USERDATA_GROW_COMMAND}' once at {USERDATA_GROW_TRIGGER}")
    for trigger, commands in actions.items():
        for command in commands:
            if USERDATA_DEVICE in command and (
                trigger != USERDATA_GROW_TRIGGER or command != USERDATA_GROW_COMMAND
            ):
                fail(f"UML init policy handles userdata differently under '{trigger}': {command}")


def check_userdata_grow_policy(vendor_policy: bytes) -> None:
    """The fsck domain may do what the online resize needs on /data, and no more than that."""
    def domain(name: str) -> str:
        return CIL_PLATFORM_VERSION_SUFFIX.sub("", name)

    granted: dict[tuple[str, str], set[str]] = {}
    for match in CIL_ALLOW_RULE.finditer(vendor_policy):
        if domain(match.group(1).decode()) == "fsck":
            key = (domain(match.group(2).decode()), match.group(3).decode())
            granted.setdefault(key, set()).update(match.group(4).decode().split())
    for target, object_class, permissions in USERDATA_GROW_RULES:
        if not permissions <= granted.get((target, object_class), set()):
            fail(
                f"vendor policy does not let the fsck domain have {sorted(permissions)} on "
                f"{target}:{object_class}: the online grow of userdata needs it"
            )
    ioctls = {
        command
        for match in CIL_ALLOWX_RULE.finditer(vendor_policy)
        if domain(match.group(1).decode()) == "fsck"
        and domain(match.group(2).decode()) == "system_data_root_file"
        and match.group(3) == b"dir"
        for command in match.group(4).decode().split()
    }
    if USERDATA_GROW_IOCTL not in ioctls:
        fail(
            "vendor policy does not let the fsck domain issue EXT4_IOC_RESIZE_FS "
            f"({USERDATA_GROW_IOCTL}) on system_data_root_file:dir"
        )
    if ioctls != {USERDATA_GROW_IOCTL}:
        fail(
            "vendor policy lets the fsck domain issue more ioctls on /data than "
            f"EXT4_IOC_RESIZE_FS: {sorted(ioctls)}"
        )


def check_network_setup(init_payload: str, actions: dict[str, list[str]]) -> None:
    """init limits ARP, then waits for the tool that addresses eth0 and builds the Wi-Fi."""
    name, executable = NET_SETUP_SERVICE
    service = init_services(init_payload).get(name)
    if service is None or service[0] != executable:
        fail(f"UML init policy does not declare the {name} service at {executable}")
    for option in ("oneshot", "disabled"):
        if option not in service[1]:
            fail(f"UML init policy does not declare {name} as {option}")
    start = f"exec_start {name}"
    for trigger, commands in actions.items():
        if trigger != NET_SETUP_TRIGGER and (start in commands or f"start {name}" in commands):
            fail(f"UML init policy starts {name} under '{trigger}'")
    commands = actions.get(NET_SETUP_TRIGGER, [])
    if start not in commands:
        fail(f"UML init policy does not wait for {name} at {NET_SETUP_TRIGGER}")
    for sysctl in NET_SETUP_SYSCTLS:
        if sysctl not in commands or commands.index(sysctl) > commands.index(start):
            fail(f"UML init policy does not run '{sysctl}' before {name}")
    boot = actions.get("boot", [])
    for directory in WIFI_DATA_DIRECTORIES:
        if directory not in boot:
            fail(f"UML init policy does not run '{directory}' at boot")


def init_actions(payload: str) -> dict[str, list[str]]:
    """Map each action trigger of an init script to its commands."""
    actions: dict[str, list[str]] = {}
    commands: list[str] | None = None
    for raw in payload.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        keyword, _, rest = line.partition(" ")
        if keyword == "on":
            commands = actions.setdefault(" ".join(rest.split()), [])
        elif keyword in ("service", "import"):
            commands = None
        elif commands is not None and raw[:1].isspace():
            commands.append(" ".join(line.split()))
    return actions


# The Android product's identity: what the guest reports as its brand, maker, device,
# product name and model, and so the prefix of its fingerprint. The product is Andarium's
# virtual device, not a Cuttlefish or "Darwin" one; the host platform keeps the name darwin.
PRODUCT_IDENTITY = {
    "brand": "Andarium",
    "manufacturer": "Kernarium",
    "device": "andarium",
    "name": "lineage_andarium",
    "model": "Andarium Virtual Device",
}


# The board and the SoC the guest reports as Build.BOARD, Build.SOC_MANUFACTURER and
# Build.SOC_MODEL. The board is the device's name, not Cuttlefish's. The SoC is the virtual CPU,
# split as a chip vendor writes it, the maker and the chip (Qualcomm and SM8550): joined with a
# space, the pair is the name the host gives the CPU (darwin/cpu/guest_identity.hpp), which a
# tooling test requires. The ODM partition states it (overlay/config/odm.prop); the vendor
# partition carries no SoC property, since the Cuttlefish values it would inherit are dropped.
BOARD_NAME = "andarium"
SOC_IDENTITY = {"ro.soc.manufacturer": "Kernarium", "ro.soc.model": "Virtual CPU"}
# What CTS accepts (android.os.cts.BuildTest#testBuildConstants): each value is trimmed, matches
# its pattern, and is not "unknown" (the test compares exactly; the check here ignores case).
BOARD_PATTERN = re.compile(r"^[0-9A-Za-z._-]+$")
SOC_PATTERNS = {
    "ro.soc.manufacturer": re.compile(r"^[0-9A-Za-z ]+$"),
    "ro.soc.model": re.compile(r"^[0-9A-Za-z ._/+-]+$"),
}


# brand/name/device:release/id/incremental:type/tags, the form of ro.build.fingerprint.
BUILD_FINGERPRINT = re.compile(
    r"^[^/]+/[^/]+/[^:/]+:[^/]+/(?P<id>[^/]+)/(?P<incremental>[^:/]+):(?P<type>[^/]+)/(?P<tags>.+)$"
)


def check_product_identity(
    system_properties: dict[str, str],
    product_properties: dict[str, str],
    vendor_properties: dict[str, str],
    build_variant: str,
) -> None:
    """The identity lives in the product and vendor partitions and in the public fingerprint.

    The system partition is AOSP's generic system (generic_system.mk): its ro.product.system.*
    and ro.system.build.fingerprint say Android / generic / Generic System, and the product
    leaves them so. What the guest reports as Build.BRAND, MODEL and the rest is the product
    partition's (and the vendor's) ro.product.<partition>.* through
    ro.product.property_source_order, and Build.FINGERPRINT is ro.build.fingerprint.
    """
    for partition, properties in (("product", product_properties), ("vendor", vendor_properties)):
        for key, expected in PRODUCT_IDENTITY.items():
            name = f"ro.product.{partition}.{key}"
            if properties.get(name) != expected:
                fail(f"unexpected product identity {name}: {properties.get(name)!r}")
    fingerprint = system_properties.get("ro.build.fingerprint", "")
    prefix = (
        f"{PRODUCT_IDENTITY['brand']}/{PRODUCT_IDENTITY['name']}/"
        f"{PRODUCT_IDENTITY['device']}:"
    )
    if not fingerprint.startswith(prefix) or f":{build_variant}/" not in fingerprint:
        fail(f"unexpected build fingerprint for a {build_variant} build: {fingerprint!r}")
    # The vendor partition states its own build identity, which the guest reports as
    # ro.vendor.build.*. It must be the build's, not the identity of an earlier or smaller build
    # (a canary build leaves "eng.build" and a userdebug type there) that the partition kept.
    parts = BUILD_FINGERPRINT.match(fingerprint)
    if parts is None:
        fail(f"the build fingerprint has no build identity: {fingerprint!r}")
    for name, expected in (
        ("ro.vendor.build.fingerprint", fingerprint),
        ("ro.vendor.build.type", parts["type"]),
        ("ro.vendor.build.tags", parts["tags"]),
        ("ro.vendor.build.id", parts["id"]),
        ("ro.vendor.build.version.incremental", parts["incremental"]),
    ):
        if vendor_properties.get(name) != expected:
            fail(
                f"the vendor partition does not carry the build's identity: {name} is "
                f"{vendor_properties.get(name)!r}, expected {expected!r} from {fingerprint!r}"
            )


def cts_build_value_problems(name: str, value: str, pattern: re.Pattern[str]) -> list[str]:
    """Why CTS' BuildTest would reject a Build constant of this value, if it would."""
    problems = []
    if value != value.strip():
        problems.append("has surrounding whitespace")
    if pattern.fullmatch(value) is None:
        problems.append(f"does not match {pattern.pattern}")
    if value.strip().lower() == "unknown":
        problems.append("is unknown")
    return [f"{name} {value!r} {problem}" for problem in problems]


def check_hardware_identity(
    odm_properties: dict[str, str], vendor_properties: dict[str, str]
) -> None:
    """The board is the device's and the SoC is the virtual CPU, each stated once.

    ro.product.board is the vendor partition's, from the board's bootloader name. The SoC
    identity is the ODM partition's: the vendor partition carries no ro.soc.* property, because
    the inherited Cuttlefish product states Google and its device name there and the build drops
    them. The expected values themselves must pass CTS, so that a change to them fails here and
    not in BuildTest."""
    board = vendor_properties.get("ro.product.board")
    if board != BOARD_NAME:
        fail(
            f"unexpected board name ro.product.board in VENDOR/build.prop: {board!r}, "
            f"not {BOARD_NAME!r}"
        )
    problems = cts_build_value_problems("ro.product.board", BOARD_NAME, BOARD_PATTERN)
    for name, expected in SOC_IDENTITY.items():
        problems += cts_build_value_problems(name, expected, SOC_PATTERNS[name])
        value = odm_properties.get(name)
        if value != expected:
            fail(
                f"unexpected SoC identity {name} in ODM/etc/build.prop: {value!r}, "
                f"not {expected!r}"
            )
    if problems:
        fail(f"the expected hardware identity fails CTS' BuildTest: {'; '.join(problems)}")
    for name, value in vendor_properties.items():
        if name.startswith("ro.soc."):
            fail(
                f"VENDOR/build.prop sets {name}={value}: the SoC identity is the ODM partition's, "
                "and PRODUCT_VENDOR_PROPERTY_BLACKLIST drops the inherited Cuttlefish values"
            )


def check_debuggable_payload(infos: dict[str, zipfile.ZipInfo], build_variant: str) -> None:
    """A user build carries nothing a debuggable build adds; a userdebug build carries su."""
    if build_variant == "user":
        for entry in DEBUG_ONLY_ENTRIES:
            if entry in infos:
                fail(f"a user build must not carry {entry}")
    else:
        for entry in DEBUG_REQUIRED_ENTRIES:
            if entry not in infos:
                fail(f"a {build_variant} build lacks {entry}")


def check_boot_policy(
    archive: zipfile.ZipFile,
    infos: dict[str, zipfile.ZipInfo],
    build_variant: str,
) -> None:
    system_properties = read_small(
        archive, infos["SYSTEM/build.prop"]
    ).decode("utf-8", errors="strict").splitlines()
    system_property_map = {
        line.partition("=")[0]: line.partition("=")[2].strip()
        for line in system_properties
        if line.partition("=")[1]
    }
    for property_name, expected in BUILD_VARIANTS[build_variant].items():
        if system_property_map.get(property_name) != expected:
            fail(
                f"unexpected {build_variant}-build property {property_name}: "
                f"{system_property_map.get(property_name)!r}"
            )
    check_product_identity(
        system_property_map,
        parse_properties(
            read_small(archive, infos["PRODUCT/etc/build.prop"]).decode("utf-8", errors="strict")
        ),
        parse_properties(
            read_small(archive, infos["VENDOR/build.prop"]).decode("utf-8", errors="strict")
        ),
        build_variant,
    )
    check_hardware_identity(
        parse_properties(
            read_small(archive, infos["ODM/etc/build.prop"]).decode("utf-8", errors="strict")
        ),
        parse_properties(
            read_small(archive, infos["VENDOR/build.prop"]).decode("utf-8", errors="strict")
        ),
    )
    check_debuggable_payload(infos, build_variant)

    fstab = android_image.tracked_fstab(error)
    for entry in UML_FSTAB_ENTRIES:
        check_userdata_quota(read_small(archive, infos[entry]), entry)
        check_zram_fstab(read_small(archive, infos[entry]), entry)
        if read_small(archive, infos[entry]) != fstab:
            fail(f"UML fstab differs from the fixed UBD contract: {entry}")

    misc_info: dict[str, str] = {}
    for line in read_small(archive, infos["META/misc_info.txt"]).decode(
        "utf-8", errors="strict"
    ).splitlines():
        key, separator, value = line.partition("=")
        if separator:
            misc_info[key] = value
    for key, expected in READ_ONLY_FILESYSTEMS.items():
        if misc_info.get(key) != expected:
            fail(f"{key} must select read-only erofs images")
    check_erofs_policy(archive, infos, misc_info)
    if misc_info.get("userdata_extfs_inode_count") != USERDATA_EXTFS_INODE_COUNT:
        fail(
            "userdata_extfs_inode_count must be "
            f"{USERDATA_EXTFS_INODE_COUNT}: {misc_info.get('userdata_extfs_inode_count')!r}"
        )

    init_payload = read_small(
        archive, infos["VENDOR/etc/init/init.kernarium.rc"]
    ).decode("utf-8", errors="strict")
    for token in UML_INIT_REQUIRED:
        if token not in init_payload:
            fail(f"UML init policy is missing required action: {token}")
    for token in UML_INIT_FORBIDDEN:
        if token in init_payload:
            fail(f"UML init policy retains unsupported Cuttlefish action: {token}")
    actions = init_actions(init_payload)
    early_init = actions.get("early-init", [])
    for name in sorted(BOOT_HAL_SELECTIONS):
        copy = f"setprop ro.hardware.{name} ${{ro.boot.hardware.{name}}}"
        if copy not in early_init:
            fail(f"UML init policy does not copy the {name} HAL selection at early-init")
    if UML_INIT_OPENGLES_COPY not in early_init:
        fail("UML init policy does not copy the OpenGL ES version at early-init")
    for trigger, commands in actions.items():
        for command in commands:
            if command.startswith(UML_INIT_TIMEOUT_PROPERTY) and (
                trigger != "early-init" or command != UML_INIT_TIMEOUT_COPY
            ):
                fail(
                    f"UML init policy runs '{command}' at '{trigger}', not "
                    f"'{UML_INIT_TIMEOUT_COPY}' at early-init"
                )
    if UML_INIT_TIMEOUT_COPY not in early_init:
        fail("UML init policy does not copy the hardware timeout multiplier at early-init")
    for trigger, commands in actions.items():
        for command in commands:
            if command.startswith(f"setprop {VENUS_PERF_PROPERTY} ") and (
                trigger != "early-init" or command != VENUS_PERF_COPY
            ):
                fail(
                    f"UML init policy runs '{command}' at '{trigger}', not "
                    f"'{VENUS_PERF_COPY}' at early-init"
                )
    if VENUS_PERF_COPY not in early_init:
        fail(
            f"UML init policy does not run '{VENUS_PERF_COPY}' at early-init: a host GPU "
            "budget refusal would abort a Vulkan application"
        )
    for trigger, commands in actions.items():
        if UML_INIT_DENSITY_COPY in commands and trigger != UML_INIT_DENSITY_TRIGGER:
            fail(f"UML init policy copies the display density under '{trigger}'")
    if UML_INIT_DENSITY_COPY not in actions.get(UML_INIT_DENSITY_TRIGGER, []):
        fail("UML init policy does not copy a host-supplied display density")
    if UML_INIT_RCU_RESTORE not in actions.get(UML_INIT_RCU_TRIGGER, []):
        fail("UML init policy does not restore normal RCU grace periods after boot")
    if ADBD_KEEP_ALIVE_COMMAND not in actions.get(ADBD_KEEP_ALIVE_TRIGGER, []):
        fail(
            f"UML init policy does not run '{ADBD_KEEP_ALIVE_COMMAND}' at "
            f"{ADBD_KEEP_ALIVE_TRIGGER}: adbd would stay down after a stop"
        )
    check_zram_swap(actions)
    check_userdata_grow(actions)
    check_suspend_blocker(init_payload, actions)
    check_network_setup(init_payload, actions)
    check_wifi_network(
        read_small(archive, infos["SYSTEM_EXT/etc/init/kernarium_wifi_network.rc"]).decode(
            "utf-8", errors="strict"
        ),
        read_small(archive, infos["SYSTEM_EXT/bin/kernarium_wifi_network.sh"]),
    )
    check_reclaim_boot_apps(
        read_small(archive, infos["SYSTEM_EXT/etc/init/kernarium_reclaim_boot_apps.rc"]).decode(
            "utf-8", errors="strict"
        ),
        read_small(archive, infos["SYSTEM_EXT/bin/kernarium_reclaim_boot_apps.sh"]),
    )
    check_time_zone(
        read_small(archive, infos["SYSTEM_EXT/etc/init/kernarium_time_zone.rc"]).decode(
            "utf-8", errors="strict"
        ),
        read_small(archive, infos["SYSTEM_EXT/bin/kernarium_time_zone.sh"]),
    )
    check_media_codecs_c2(read_small(archive, infos["VENDOR/etc/media_codecs_c2.xml"]))

    ueventd_payload = read_small(
        archive, infos["VENDOR/etc/ueventd.rc"]
    ).decode("utf-8", errors="strict")
    ueventd_entries = {
        line.strip()
        for line in ueventd_payload.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }
    for entry in (
        "/dev/ttyS0 0660 root shell",
        "/dev/hvc0 0660 root shell",
    ):
        if entry not in ueventd_entries:
            fail(f"UML ueventd policy is missing console entry: {entry}")
    for entry in ueventd_entries:
        device = entry.split(maxsplit=1)[0]
        if device.startswith("/dev/hvc") and device != "/dev/hvc0":
            fail(f"UML ueventd policy retains unsupported HVC device: {device}")
        for prefix in ("/dev/ion", "/dev/pmem", "/dev/vhci"):
            if device.startswith(prefix):
                fail(f"UML ueventd policy retains unsupported device: {device}")


# The runtime resource overlay Soong generates from the product's SimpleDeviceConfig
# overlays, and the DeviceConfig key whose value lists the app ops recorded into the discrete
# history. The default ops are DiscreteOpsRegistry's LEGACY_OPS and IMPORTANT_OPS_FOR_SECURITY:
# LineageOS's list predates most of them, and an op that is not listed never reaches the history.
DEVICE_CONFIG_OVERLAY = (
    "PRODUCT/overlay/SimpleDeviceConfig__lineage_andarium__auto_generated_rro_product.apk"
)
DISCRETE_OPS_KEY = b"privacy/discrete_history_ops_cslist="
DISCRETE_OPS_DEFAULT = frozenset({
    0, 1, 2, 25, 26, 27, 41, 42, 63, 73, 88, 89, 100, 101,
    120, 136, 141, 147, 149, 150, 152, 170, 171, 172, 173,
})


def check_discrete_ops(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The discrete app-op history list must keep AOSP's default ops."""
    info = infos.get(DEVICE_CONFIG_OVERLAY)
    if info is None:
        fail(f"{DEVICE_CONFIG_OVERLAY} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info))) as overlay:
            table = overlay.read("resources.arsc")
    except (KeyError, OSError, zipfile.BadZipFile) as error:
        fail(f"{DEVICE_CONFIG_OVERLAY} has no readable resource table: {error}")
    lists = [
        {int(op) for op in match.group(1).split(b",") if op}
        for match in re.finditer(re.escape(DISCRETE_OPS_KEY) + rb"([0-9,]+)", table)
    ]
    if not lists:
        fail(f"{DEVICE_CONFIG_OVERLAY} does not set {DISCRETE_OPS_KEY.decode()}")
    missing = min((sorted(DISCRETE_OPS_DEFAULT - ops) for ops in lists), key=len)
    if missing:
        fail(
            f"no discrete app-op list in {DEVICE_CONFIG_OVERLAY} keeps AOSP's default ops; "
            f"the closest lacks {', '.join(map(str, missing))}"
        )


# LineageOS's anonymous statistics uploader posts the device name, build, country, carrier and a
# hash of the Android ID to stats.lineageos.org once its setting is on, and the Setup Wizard asks
# for that setting. The product removes both: the two applications carry no statistics
# components, no endpoint and no switch for the setting, so nothing can post whatever the setting
# says. Names are searched in the compiled manifest, resource table, layouts and every dex, as
# UTF-8 and as UTF-16 (the two string-pool encodings).
STATISTICS_APKS = (
    "SYSTEM_EXT/priv-app/LineageParts/LineageParts.apk",
    "SYSTEM_EXT/priv-app/LineageSetupWizard/LineageSetupWizard.apk",
)
STATISTICS_MARKERS = (
    b"lineagestats",
    b"stats.lineageos.org",
    b"lineageos.org",
    b"stats_collection",
    b"send_metrics",
    b"services_metrics",
    b"anonymous_statistics",
    b"StatsUploadJobService",
    b"ReportingService",
)
STATISTICS_APK_LIMIT = 64 << 20


def check_no_statistics_uploader(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """No application can post statistics to LineageOS, whatever the setting says."""
    for entry in STATISTICS_APKS:
        info = infos.get(entry)
        if info is None:
            fail(f"{entry} is missing")
        try:
            with zipfile.ZipFile(io.BytesIO(read_small(archive, info, STATISTICS_APK_LIMIT))) as apk:
                for name in apk.namelist():
                    if not (
                        name in ("AndroidManifest.xml", "resources.arsc")
                        or (name.startswith("classes") and name.endswith(".dex"))
                        or (name.startswith("res/") and name.endswith(".xml"))
                    ):
                        continue
                    payload = apk.read(name)
                    for marker in STATISTICS_MARKERS:
                        if marker in payload or marker.decode().encode("utf-16-le") in payload:
                            fail(
                                f"{entry} still carries LineageOS's statistics uploader: "
                                f"{name} contains {marker.decode()}"
                            )
        except (OSError, zipfile.BadZipFile) as error:
            fail(f"{entry} is not a readable application archive: {error}")


# The Setup Wizard names the system Andarium. Its strings come from a runtime resource overlay that
# the product's overlay directory contributes to Soong's one overlay for the application; the
# resource table must hold the product's name and recovery strings and none of LineageOS's. The
# table is searched as UTF-8 and as UTF-16, the two string-pool encodings.
SETUP_WIZARD_OVERLAY = (
    "PRODUCT/overlay/LineageSetupWizard__lineage_andarium__auto_generated_rro_product.apk"
)
SETUP_WIZARD_NAMES = (
    b"Andarium features",
    b"Update Andarium Recovery",
    b"Updates Andarium Recovery on first boot subsequent to every update.",
)


# The Setup Wizard's two logos are the product's: the overlay replaces the vector drawable of the
# Finish screen and the services page header, and the Lottie animation of the Welcome screen. The
# animation is a raw file, which the overlay carries unchanged; the compiled vector keeps its path
# data as strings, and a vector that holds every path of the tracked one is that vector.
SETUP_WIZARD_ART = (
    Path(__file__).resolve().parents[1]
    / "overlay/overlay/packages/apps/SetupWizard/app/src/main/res"
)
SETUP_WIZARD_WELCOME = "res/raw/lottie_welcome.json"


def check_setup_wizard_art(overlay: zipfile.ZipFile) -> None:
    paths = re.findall(
        r'android:pathData="([^"]+)"', (SETUP_WIZARD_ART / "drawable/logo.xml").read_text(encoding="utf-8")
    )
    if len(paths) < 2:
        fail(f"{SETUP_WIZARD_ART}/drawable/logo.xml has fewer than two paths: the tracked logo is not the product's")
    logos = [name for name in overlay.namelist() if re.fullmatch(r"res/drawable[^/]*/logo\.xml", name)]
    if not any(
        all(path.encode() in table or path.encode("utf-16-le") in table for path in paths)
        for table in (overlay.read(name) for name in logos)
    ):
        fail(f"{SETUP_WIZARD_OVERLAY} has no res/drawable/logo.xml that holds the product's logo")
    try:
        welcome = overlay.read(SETUP_WIZARD_WELCOME)
    except KeyError:
        fail(f"{SETUP_WIZARD_OVERLAY} has no {SETUP_WIZARD_WELCOME}")
    if welcome != (SETUP_WIZARD_ART / "raw/lottie_welcome.json").read_bytes():
        fail(f"{SETUP_WIZARD_OVERLAY}'s {SETUP_WIZARD_WELCOME} is not the product's animation")
    check_setup_wizard_flow(overlay)


# The Setup Wizard shows the pages the product has reviewed and no others. LineageOS's flow lists
# eleven more: each is either decided against or can never apply to this product (the README of the
# release names the reason for every one). The overlay carries two scripts, which are raw files and
# so reach the overlay unchanged: the owner's flow, and the one a secondary user runs. A script is
# reviewed as the flow its first action reaches, in order, and it may define nothing else.
SETUP_WIZARD_NAMESPACE = "http://schemas.android.com/apk/res/com.google.android.setupwizard"
SETUP_WIZARD_FLOWS = {
    "lineage_wizard_script.xml": ("welcome", "locale", "datetime", "theme_settings"),
    "lineage_wizard_script_user.xml": ("welcome", "theme_settings"),
}
SETUP_WIZARD_HIDDEN = (
    "bluetooth_setup",
    "network_setup",
    "sim_missing",
    "restore",
    "location_settings",
    "device_specific",
    "recovery_update",
    "lineage_settings",
    "biometric_settings",
    "lockscreen_settings",
    "navigation_settings",
)


def check_setup_wizard_script(source: str, text: str, reviewed: tuple[str, ...]) -> None:
    """The script's flow is the reviewed pages in order, and no hidden page is defined or reached."""
    try:
        root = ET.fromstring(text)
    except ET.ParseError as error:
        fail(f"{source} is not well-formed XML: {error}")
    if root.tag != "WizardScript":
        fail(f"{source} is not a WizardScript")
    first = root.get(f"{{{SETUP_WIZARD_NAMESPACE}}}firstAction")
    if first is None:
        fail(f"{source} names no firstAction")
    actions: dict[str, list[str]] = {}
    for action in root.findall("WizardAction"):
        identifier = action.get("id")
        if identifier is None or identifier in actions:
            fail(f"{source} has a WizardAction without a unique id: {identifier!r}")
        actions[identifier] = [
            target
            for result in action.findall("result")
            if (target := result.get(f"{{{SETUP_WIZARD_NAMESPACE}}}action")) is not None
        ]
    reached: list[str] = []
    pending = [first]
    while pending:
        identifier = pending.pop(0)
        if identifier in reached:
            continue
        if identifier not in actions:
            fail(f"{source}: the flow reaches {identifier!r}, which the script does not define")
        reached.append(identifier)
        pending.extend(actions[identifier])
    hidden = [page for page in SETUP_WIZARD_HIDDEN if page in actions]
    if hidden:
        fail(f"{source} defines pages the product hides: {', '.join(hidden)}")
    if tuple(reached) != reviewed:
        fail(f"{source} reaches {' > '.join(reached)}, not the reviewed {' > '.join(reviewed)}")
    unreached = sorted(set(actions) - set(reached))
    if unreached:
        fail(f"{source} defines pages its flow never reaches: {', '.join(unreached)}")


def check_setup_wizard_flow(overlay: zipfile.ZipFile) -> None:
    """The overlay's scripts are the tracked ones, and the tracked ones are the reviewed flows."""
    for name, reviewed in SETUP_WIZARD_FLOWS.items():
        tracked = SETUP_WIZARD_ART / "raw" / name
        check_setup_wizard_script(str(tracked), tracked.read_text(encoding="utf-8"), reviewed)
        member = f"res/raw/{name}"
        try:
            shipped = overlay.read(member)
        except KeyError:
            fail(f"{SETUP_WIZARD_OVERLAY} has no {member}")
        if shipped != tracked.read_bytes():
            fail(f"{SETUP_WIZARD_OVERLAY}'s {member} is not the product's script")


def check_setup_wizard_branding(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The Setup Wizard's strings say Andarium, in the default locale, and carry no LineageOS name."""
    info = infos.get(SETUP_WIZARD_OVERLAY)
    if info is None:
        fail(f"{SETUP_WIZARD_OVERLAY} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, 16 << 20))) as overlay:
            table = overlay.read("resources.arsc")
            check_setup_wizard_art(overlay)
    except (KeyError, OSError, zipfile.BadZipFile) as error:
        fail(f"{SETUP_WIZARD_OVERLAY} has no readable resource table: {error}")

    def holds(text: bytes) -> bool:
        return text in table or text.decode().encode("utf-16-le") in table

    for text in SETUP_WIZARD_NAMES:
        if not holds(text):
            fail(f"{SETUP_WIZARD_OVERLAY} does not set the string {text.decode()!r}")
    if holds(b"Lineage"):
        fail(f"{SETUP_WIZARD_OVERLAY} still names LineageOS or Lineage")


# The navigation mode is the platform's. LineageOS's Setup Wizard switched a device without an
# app-switch key to gestural navigation when it started, before its first page; the product's
# wizard topic (route p0026) removes that call, so a fresh guest runs the framework's default,
# three buttons, and Settings can switch it either way: both mode overlays stay in the image. The
# wizard's dex no longer names the function (R8 keeps method names), no overlay configuration
# enables a mode overlay, and no property enables one at boot, which would override the default.
SETUP_WIZARD_APK = "SYSTEM_EXT/priv-app/LineageSetupWizard/LineageSetupWizard.apk"
GESTURAL_FORCING = b"enableGesturalNavigation"
NAVIGATION_OVERLAY_DIRECTORIES = (
    "PRODUCT/overlay/NavigationBarMode3Button/",
    "PRODUCT/overlay/NavigationBarModeGestural/",
)
NAVIGATION_OVERLAY_PACKAGES = "com.android.internal.systemui.navbar."
OVERLAY_CONFIGURATION = re.compile(r"[A-Z_]+/overlay/config/[^/]+\.xml")
OVERLAY_THEME_PROPERTY = "ro.boot.vendor.overlay.theme"


# Settings' "Android version" row opens the platform logo screen, and that screen starts the easter
# egg by its PLATLOGO category, so the image carries the egg's package. The framework's product list
# decides where it lands, hence any of the three application directories.
EASTER_EGG_APK = re.compile(r"(?:SYSTEM|SYSTEM_EXT|PRODUCT)/(?:priv-)?app/EasterEgg/EasterEgg\.apk")


def check_easter_egg(infos: dict[str, zipfile.ZipInfo]) -> None:
    """The easter egg that the platform logo screen opens is in the image."""
    if not any(EASTER_EGG_APK.fullmatch(name) for name in infos):
        fail(
            "EasterEgg.apk is missing from the image: the Android version row opens the logo screen, "
            "which starts the egg (com.android.egg)"
        )


def check_navigation_mode(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The wizard leaves the navigation mode alone, and both modes stay available."""
    info = infos.get(SETUP_WIZARD_APK)
    if info is None:
        fail(f"{SETUP_WIZARD_APK} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, STATISTICS_APK_LIMIT))) as apk:
            for name in apk.namelist():
                if name.startswith("classes") and name.endswith(".dex") and GESTURAL_FORCING in apk.read(name):
                    fail(
                        f"{SETUP_WIZARD_APK} still forces gestural navigation: "
                        f"{name} contains {GESTURAL_FORCING.decode()}"
                    )
    except (OSError, zipfile.BadZipFile) as error:
        fail(f"{SETUP_WIZARD_APK} is not a readable application archive: {error}")
    for directory in NAVIGATION_OVERLAY_DIRECTORIES:
        if not any(name.startswith(directory) and name.endswith(".apk") for name in infos):
            fail(f"the navigation mode overlay under {directory} is missing: Settings could not switch modes")
    for name in sorted(infos):
        if OVERLAY_CONFIGURATION.fullmatch(name) is None:
            continue
        try:
            configuration = ET.fromstring(read_small(archive, infos[name], 1 << 20))
        except ET.ParseError as error:
            fail(f"{name} is not well-formed XML: {error}")
        for element in configuration.iter():
            package = element.get("package", "")
            if package.startswith(NAVIGATION_OVERLAY_PACKAGES) and element.get("enabled") != "false":
                fail(f"{name} enables the navigation mode overlay {package}: the mode is the platform's")


# Trebuchet's stock default workspace names Google applications and widgets that the image does not
# ship, so a fresh guest lost most of its items. The product's runtime resource overlay for
# Launcher3QuickStep carries one layout per grid the launcher can pick, and each holds the dock
# (hotseat) of Files and Settings and nothing else: no item on the workspace and no widget. Calendar,
# Gallery, Music and Calculator stay installed and reach the person through the app drawer. The tracked
# layouts are checked as a table of reviewed components (each one's APK must be in the image) and the
# overlay must hold every layout. The overlay compiles the layouts, so the check reads their strings
# in the overlay's string pools, as the Setup Wizard check does, and a layout of the stock launcher
# fails on its Google names.
LAUNCHER_OVERLAY = (
    "PRODUCT/overlay/Launcher3QuickStep__lineage_andarium__auto_generated_rro_product.apk"
)
LAUNCHER_LAYOUTS = (
    Path(__file__).resolve().parents[1] / "overlay/overlay/packages/apps/Launcher3/res/xml"
)
LAUNCHER_NAMESPACE = "http://schemas.android.com/apk/res-auto/com.android.launcher3"
LAUNCHER_GRIDS = ("4x5", "5x5", "5x6", "5x7", "6x5", "6x6")
LAUNCHER_FILES = ("com.android.documentsui", "com.android.documentsui.LauncherActivity")
LAUNCHER_SETTINGS = ("com.android.settings", "com.android.settings.Settings")
# (package, class) of each application with a launcher activity, and where its APK is. All six stay in
# the image; the app drawer lists them.
LAUNCHER_APPS = {
    LAUNCHER_SETTINGS: "SYSTEM_EXT/priv-app/Settings/Settings.apk",
    ("org.lineageos.etar", "com.android.calendar.AllInOneActivity"): "PRODUCT/app/Etar/Etar.apk",
    ("org.lineageos.glimpse", "org.lineageos.glimpse.MainActivity"): "PRODUCT/app/Glimpse/Glimpse.apk",
    ("org.lineageos.twelve", "org.lineageos.twelve.MainActivity"): "PRODUCT/app/Twelve/Twelve.apk",
    ("com.android.calculator2", "com.android.calculator2.Calculator"):
        "PRODUCT/app/ExactCalculator/ExactCalculator.apk",
    LAUNCHER_FILES: "SYSTEM/priv-app/DocumentsUI/DocumentsUI.apk",
}
# What the home screen holds: the dock, in the order Files then Settings, and nothing on the workspace.
LAUNCHER_DOCK = (LAUNCHER_FILES, LAUNCHER_SETTINGS)
LAUNCHER_HOTSEAT_CONTAINER = "-101"
# Each grid's dock slots and the slots of Files and Settings: the pair is centred in the dock.
LAUNCHER_DOCK_SLOTS = {"4x5": 4, "5x5": 5, "5x6": 5, "5x7": 5, "6x5": 6, "6x6": 6}
LAUNCHER_DOCK_RANKS = {"4x5": (1, 2), "5x5": (1, 3), "5x6": (1, 3), "5x7": (1, 3), "6x5": (2, 3), "6x6": (2, 3)}
LAUNCHER_FORBIDDEN_NAMES = ("google", "vending", "deskclock", "alarmclock", "@string/", "@xml/")
LAUNCHER_ATTRIBUTES = ("packageName", "className", "container", "screen", "x", "y")


def check_launcher_layout(grid: str, source: str, text: str) -> None:
    """One tracked layout: Files and Settings in the dock of a grid, and nothing on the workspace."""
    try:
        root = ET.fromstring(text)
    except ET.ParseError as error:
        fail(f"{source} is not well-formed XML: {error}")
    if root.tag != "favorites":
        fail(f"{source} is not a favorites layout")
    # Comments are not in the parsed tree: the names that matter are the ones the launcher reads.
    for element in root.iter():
        for value in [element.tag, element.text or "", *element.attrib.values()]:
            for name in LAUNCHER_FORBIDDEN_NAMES:
                if name in value.lower():
                    fail(f"{source} names {name!r} in {value!r}: the image ships no such application")
    slots = LAUNCHER_DOCK_SLOTS[grid]
    seen: dict[tuple[str, str], int] = {}
    ranks: set[int] = set()
    for item in root:
        if item.tag != "favorite":
            fail(f"{source} holds a <{item.tag}>: the layout may hold only applications")
        attributes = {
            (name[len(LAUNCHER_NAMESPACE) + 2:] if name.startswith("{") else name): value
            for name, value in item.attrib.items()
        }
        if sorted(attributes) != sorted(LAUNCHER_ATTRIBUTES):
            fail(
                f"{source} has a favorite with attributes {sorted(attributes)}, "
                f"not {sorted(LAUNCHER_ATTRIBUTES)} (a folder, uri or title item is not allowed)"
            )
        if any(not name.startswith("{" + LAUNCHER_NAMESPACE + "}") for name in item.attrib):
            fail(f"{source} has a favorite attribute outside the launcher namespace")
        component = (attributes["packageName"], attributes["className"])
        if component not in LAUNCHER_APPS:
            fail(f"{source} names {component[0]}/{component[1]}, which is not in the reviewed table")
        if attributes["container"] != LAUNCHER_HOTSEAT_CONTAINER:
            fail(
                f"{source} puts {component[1]} in container {attributes['container']}: the workspace is "
                "empty, and only the dock holds items"
            )
        if component not in LAUNCHER_DOCK:
            fail(f"{source} puts {component[1]} in the dock: only Files and Settings are there, the app drawer holds the rest")
        if component in seen:
            fail(f"{source} names {component[0]}/{component[1]} twice")
        try:
            screen, x, y = (int(attributes[name]) for name in ("screen", "x", "y"))
        except ValueError:
            fail(f"{source} has a favorite whose screen, x or y is not a number")
        if not 0 <= screen < slots:
            fail(f"{source} puts {component[1]} in dock slot {screen}, outside the {slots} slots of the {grid} dock")
        if x != screen or y != 0:
            fail(f"{source} puts {component[1]} at ({x}, {y}) in slot {screen}: a dock item's column is its slot and its row 0")
        if screen in ranks:
            fail(f"{source} puts two items in dock slot {screen}")
        ranks.add(screen)
        seen[component] = screen
    if set(seen) != set(LAUNCHER_DOCK):
        missing = sorted(app[1] for app in set(LAUNCHER_DOCK) - set(seen))
        fail(f"{source} lacks {', '.join(missing)}: the dock holds exactly Files and Settings")
    if tuple(seen[component] for component in LAUNCHER_DOCK) != LAUNCHER_DOCK_RANKS[grid]:
        fail(
            f"{source} puts Files and Settings in slots {seen[LAUNCHER_FILES]} and {seen[LAUNCHER_SETTINGS]}, "
            f"not the centred {LAUNCHER_DOCK_RANKS[grid][0]} and {LAUNCHER_DOCK_RANKS[grid][1]} of the {slots} slots"
        )


def check_launcher_workspace(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The overlay carries the dock layouts, and every application of the image is installed."""
    for apk in LAUNCHER_APPS.values():
        if apk not in infos:
            fail(f"{apk} is missing: the launcher's default workspace names its application")
    info = infos.get(LAUNCHER_OVERLAY)
    if info is None:
        fail(f"{LAUNCHER_OVERLAY} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, 16 << 20))) as overlay:
            members = {name: overlay.read(name) for name in overlay.namelist() if name.startswith("res/xml/")}
    except (OSError, zipfile.BadZipFile) as error:
        fail(f"{LAUNCHER_OVERLAY} is not a readable overlay: {error}")
    for grid in LAUNCHER_GRIDS:
        name = f"default_workspace_{grid}.xml"
        tracked = LAUNCHER_LAYOUTS / name
        check_launcher_layout(grid, str(tracked), tracked.read_text(encoding="utf-8"))
        payload = members.get(f"res/xml/{name}")
        if payload is None:
            fail(f"{LAUNCHER_OVERLAY} has no res/xml/{name}: a {grid} guest would show the stock layout")

        def holds(text: str) -> bool:
            return text.encode() in payload or text.encode("utf-16-le") in payload

        for _package, component in LAUNCHER_DOCK:
            if not holds(component):
                fail(f"{LAUNCHER_OVERLAY}'s res/xml/{name} does not name {component}")
        for _package, component in sorted(set(LAUNCHER_APPS) - set(LAUNCHER_DOCK)):
            if holds(component):
                fail(f"{LAUNCHER_OVERLAY}'s res/xml/{name} names {component}: the home screen holds only the dock")
        for stock in LAUNCHER_FORBIDDEN_NAMES[:4]:
            if holds(stock):
                fail(f"{LAUNCHER_OVERLAY}'s res/xml/{name} is a stock layout: it names {stock}")


# The framework's runtime resource overlay holds the product's replacement for LineageOS's
# default wallpaper. Its two tablet variants are what every display of 600dp and more draws, and
# SystemUI sizes the wallpaper surface from the bitmap, so a variant of the default 1920 pixels
# keeps a 14 MiB buffer alive for a window that is the display's long side wide.
FRAMEWORK_OVERLAY = (
    "PRODUCT/overlay/framework-res__lineage_andarium__auto_generated_rro_product.apk"
)
FRAMEWORK_OVERLAY_LIMIT = 16 << 20
DEFAULT_WALLPAPER_ENTRIES = tuple(
    f"res/{directory}-v13/{default_wallpaper.FILE_NAME}"
    for directory in default_wallpaper.VARIANT_DIRECTORIES
)


def check_default_wallpaper(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The tablet default wallpapers are the overlay's bounded square, not LineageOS's 1920."""
    info = infos.get(FRAMEWORK_OVERLAY)
    if info is None:
        fail(f"{FRAMEWORK_OVERLAY} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, FRAMEWORK_OVERLAY_LIMIT))) as overlay:
            for entry in DEFAULT_WALLPAPER_ENTRIES:
                try:
                    header = default_wallpaper.png_size(overlay.read(entry))
                except KeyError:
                    fail(f"{FRAMEWORK_OVERLAY} has no {entry}")
                except default_wallpaper.WallpaperError as error:
                    fail(f"{entry} in {FRAMEWORK_OVERLAY} is {error}")
                width, height = header[:2]
                if width != height or width > default_wallpaper.MAX_SIDE:
                    fail(
                        f"{entry} in {FRAMEWORK_OVERLAY} is {width}x{height}: the tablet default "
                        f"wallpaper must be a square of at most {default_wallpaper.MAX_SIDE} pixels "
                        "(LineageOS's overlay wins the resource when the product lists "
                        f"{default_wallpaper.PRIORITY_OVERLAY} after its inherit-product calls)"
                    )
    except (OSError, zipfile.BadZipFile) as error:
        fail(f"{FRAMEWORK_OVERLAY} is not a readable archive: {error}")


# The guest's clock is the host's: the runtime hands the guest the host's wall clock at boot and at
# resume, so the platform gets no network time. NetworkTimeUpdateService polls NTP whenever a default
# network exists, whatever the time detector's origin priorities say, and NtpTrustedTime takes its
# servers from config_ntpServers. The one value that leaves it without a server is a list whose
# entries are not ntp:// URIs: the client rejects the list and refreshes nothing before any lookup,
# whereas an empty list makes NtpConfig throw in system_server. The origin priorities then leave
# the network out as well. An empty priority list means the platform default, which holds the
# network, and a list of telephony alone, on a device without telephony, would make the platform
# drop "Set time automatically"; "external" keeps automatic detection supported.
NETWORK_TIME_SERVERS = "config_ntpServers"
NETWORK_TIME_ORIGINS = "config_autoTimeSourcesPriority"


def resource_string_pool(table: bytes, offset: int) -> list[str]:
    """The strings of the ResStringPool chunk at offset."""
    count, _styles, flags, strings_start = struct.unpack_from("<IIII", table, offset + 8)
    offsets = struct.unpack_from(f"<{count}I", table, offset + 28)
    strings = []
    for start in offsets:
        position = offset + strings_start + start
        if flags & 0x100:
            # UTF-8: the length in characters, then the length in bytes, each one or two bytes.
            for _ in range(2):
                length = table[position]
                position += 1
                if length & 0x80:
                    length = ((length & 0x7F) << 8) | table[position]
                    position += 1
            strings.append(table[position:position + length].decode("utf-8", "replace"))
        else:
            length = struct.unpack_from("<H", table, position)[0]
            position += 2
            if length & 0x8000:
                length = ((length & 0x7FFF) << 16) | struct.unpack_from("<H", table, position)[0]
                position += 2
            strings.append(table[position:position + 2 * length].decode("utf-16-le", "replace"))
    return strings


def resource_entries(table: bytes, type_name: str):
    """The (name, position, flags) of every entry of one resource type in a compiled resource
    table, in each of its configurations. The position is that of the ResTable_entry, or of the
    compact entry whose key sits where a full entry keeps its size."""
    position = struct.unpack_from("<H", table, 2)[0]
    chunk, _, size = struct.unpack_from("<HHI", table, position)
    if chunk != 0x0001:
        raise ValueError("the table does not start with a string pool")
    position += size
    while position + 8 <= len(table):
        chunk, header_size, size = struct.unpack_from("<HHI", table, position)
        if size < 8:
            break
        if chunk == 0x0200:
            type_pool, _, key_pool = struct.unpack_from("<III", table, position + 268)
            types = resource_string_pool(table, position + type_pool)
            keys = resource_string_pool(table, position + key_pool)
            inner, end = position + header_size, position + size
            while inner + 8 <= end:
                inner_chunk, inner_header, inner_size = struct.unpack_from("<HHI", table, inner)
                if inner_size < 8:
                    break
                if inner_chunk == 0x0201 and types[table[inner + 8] - 1] == type_name:
                    count, entries_start = struct.unpack_from("<II", table, inner + 12)
                    kind = table[inner + 9]
                    if kind & 0x01:  # sparse: (index, offset / 4) pairs of 16 bits
                        offsets = [4 * pair[1] for pair in struct.iter_unpack("<HH", table[inner + inner_header:inner + inner_header + 4 * count])]
                    elif kind & 0x02:  # 16-bit offsets in units of 4 bytes
                        offsets = [0xFFFFFFFF if value == 0xFFFF else 4 * value
                                   for value in struct.unpack_from(f"<{count}H", table, inner + inner_header)]
                    else:
                        offsets = struct.unpack_from(f"<{count}I", table, inner + inner_header)
                    for entry_offset in offsets:
                        if entry_offset == 0xFFFFFFFF:
                            continue
                        entry = inner + entries_start + entry_offset
                        key, flags = struct.unpack_from("<HH", table, entry)
                        if not flags & 0x08:  # a full entry: size, flags, then a 32-bit key
                            key = struct.unpack_from("<I", table, entry + 4)[0]
                        yield keys[key], entry, flags
                inner += inner_size
        position += size


def resource_arrays(table: bytes) -> dict[str, list[list[str]]]:
    """Every array resource of a compiled resource table, by name: one list of its items'
    strings for each definition (an item that is not a string reads as "<type:data>")."""
    arrays: dict[str, list[list[str]]] = {}
    values = resource_string_pool(table, struct.unpack_from("<H", table, 2)[0])
    for name, entry, flags in resource_entries(table, "array"):
        if not flags & 1:
            continue
        item_count = struct.unpack_from("<I", table, entry + 12)[0]
        items = []
        for index in range(item_count):
            _, _, data_type, data = struct.unpack_from("<HBBI", table, entry + 16 + 12 * index + 4)
            items.append(values[data] if data_type == 3 else f"<{data_type}:{data}>")
        arrays.setdefault(name, []).append(items)
    return arrays


def resource_booleans(table: bytes) -> dict[str, list[bool]]:
    """Every bool resource of a compiled resource table, by name: its value in each definition."""
    booleans: dict[str, list[bool]] = {}
    for name, entry, flags in resource_entries(table, "bool"):
        if flags & 0x08:  # compact: the value type is the flags' high byte, the data follows
            data_type, data = flags >> 8, struct.unpack_from("<I", table, entry + 4)[0]
        elif flags & 0x01:
            continue
        else:
            size = struct.unpack_from("<H", table, entry)[0]
            _, _, data_type, data = struct.unpack_from("<HBBI", table, entry + size)
        if data_type == 0x12:  # TYPE_INT_BOOLEAN
            booleans.setdefault(name, []).append(data != 0)
    return booleans


def check_network_time(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The platform queries no NTP server and takes no time from the network."""
    info = infos.get(FRAMEWORK_OVERLAY)
    if info is None:
        fail(f"{FRAMEWORK_OVERLAY} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, FRAMEWORK_OVERLAY_LIMIT))) as overlay:
            arrays = resource_arrays(overlay.read("resources.arsc"))
    except (KeyError, OSError, ValueError, struct.error, IndexError, zipfile.BadZipFile) as error:
        fail(f"{FRAMEWORK_OVERLAY} has no readable resource table: {error}")
    for name in (NETWORK_TIME_SERVERS, NETWORK_TIME_ORIGINS):
        if name not in arrays:
            fail(f"{FRAMEWORK_OVERLAY} does not set {name}: the platform would query time.android.com")
    for items in arrays[NETWORK_TIME_SERVERS]:
        if not items:
            fail(f"{NETWORK_TIME_SERVERS} is empty in {FRAMEWORK_OVERLAY}: NtpConfig throws on an empty list in system_server")
        for item in items:
            if re.match(r"(?i)ntp:", item):
                fail(f"{NETWORK_TIME_SERVERS} names the NTP server {item} in {FRAMEWORK_OVERLAY}: no NTP query may be left")
    for items in arrays[NETWORK_TIME_ORIGINS]:
        if not items:
            fail(f"{NETWORK_TIME_ORIGINS} is empty in {FRAMEWORK_OVERLAY}: the platform default holds the network")
        if "network" in items:
            fail(f"{NETWORK_TIME_ORIGINS} lists the network in {FRAMEWORK_OVERLAY}: network time is not a time source")
        if "external" not in items:
            fail(f"{NETWORK_TIME_ORIGINS} in {FRAMEWORK_OVERLAY} lacks external: without it a device without telephony loses \"Set time automatically\"")


# The power manager's FaceDownDetector turns the screen off when the device lies face down. While
# it is enabled it holds an accelerometer connection (every 200 ms, batched over 2 s) for as long
# as the screen is on, so the host's motion sensor stays running although no application asked for
# a reading, and a virtual device that is never "face down" gains nothing from it. The detector is
# enabled only when the framework resource is true (and the attention_manager_service flag the
# platform leaves on), so the product's framework overlay turns it off for every boot.
FLIP_TO_SCREEN_OFF = "config_flipToScreenOffEnabled"


def check_flip_to_screen_off(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The framework overlay disables flip-to-screen-off, which would hold the accelerometer."""
    info = infos.get(FRAMEWORK_OVERLAY)
    if info is None:
        fail(f"{FRAMEWORK_OVERLAY} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, FRAMEWORK_OVERLAY_LIMIT))) as overlay:
            booleans = resource_booleans(overlay.read("resources.arsc"))
    except (KeyError, OSError, ValueError, struct.error, IndexError, zipfile.BadZipFile) as error:
        fail(f"{FRAMEWORK_OVERLAY} has no readable resource table: {error}")
    if FLIP_TO_SCREEN_OFF not in booleans:
        fail(
            f"{FRAMEWORK_OVERLAY} does not set {FLIP_TO_SCREEN_OFF}: the power manager's "
            "FaceDownDetector would hold an accelerometer connection while the screen is on"
        )
    if any(booleans[FLIP_TO_SCREEN_OFF]):
        fail(
            f"{FLIP_TO_SCREEN_OFF} is true in {FRAMEWORK_OVERLAY}: the power manager's "
            "FaceDownDetector would hold an accelerometer connection while the screen is on"
        )


# PinnerService's pin sizes are framework resources that the platform does not declare
# overlayable. A runtime resource overlay may replace a resource its target has not declared
# overlayable only when the overlay targets SDK 28 or lower; from Android 10 on the target's
# overlayable policies bind it, and the overlay then applies to nothing without a message. The
# overlay releases the home app's pin, about 54 MiB locked at idle.
PINNER_OVERLAY = "VENDOR/overlay/KernariumPinnerOverlay.apk"
PINNER_OVERLAY_RESOURCE = b"config_pinnerHomePinBytes"
PINNER_OVERLAY_MAX_TARGET_SDK = 28
# The framework attributes android:targetSdkVersion and android:debuggable, and the types a
# compiled manifest gives them: a number (TYPE_INT_DEC and TYPE_INT_HEX) and a literal
# boolean (TYPE_INT_BOOLEAN, all ones for true).
ATTR_TARGET_SDK_VERSION = 0x01010270
ATTR_DEBUGGABLE = 0x0101000F
ATTR_NAME = 0x01010003
AXML_STRING_POOL = 0x0001
AXML_START_ELEMENT = 0x0102
AXML_RESOURCE_MAP = 0x0180
AXML_STRING_TYPE = 0x03
AXML_UTF8_POOL = 0x100
AXML_INTEGER_TYPES = (0x10, 0x11)
AXML_BOOLEAN_TYPE = 0x12


def manifest_attribute(manifest: bytes, attribute: int) -> tuple[int, int] | None:
    """The (type, data) of the first element attribute with this framework ID in a compiled manifest, or None."""
    if len(manifest) < 8 or struct.unpack_from("<HH", manifest, 0)[0] != 0x0003:
        raise ValueError("not a compiled XML file")
    resource_ids: list[int] = []
    offset = struct.unpack_from("<H", manifest, 2)[0]
    while offset + 8 <= len(manifest):
        kind, header, size = struct.unpack_from("<HHI", manifest, offset)
        if size < 8 or offset + size > len(manifest):
            raise ValueError("truncated compiled XML chunk")
        if kind == AXML_RESOURCE_MAP:
            resource_ids = list(struct.unpack_from(f"<{(size - header) // 4}I", manifest, offset + header))
        elif kind == AXML_START_ELEMENT:
            body = offset + header
            attribute_start, attribute_size, count = struct.unpack_from("<HHH", manifest, body + 8)
            for index in range(count):
                position = body + attribute_start + index * attribute_size
                name, _, _, data_type, data = struct.unpack_from("<IIHxBI", manifest, position + 4)
                if name < len(resource_ids) and resource_ids[name] == attribute:
                    return data_type, data
        offset += size
    return None


def axml_strings(manifest: bytes, chunk: int) -> list[str]:
    """The strings of the compiled XML string pool chunk at this offset."""
    _, header, size, count, _, flags, strings_start, _ = struct.unpack_from("<HHIIIIII", manifest, chunk)

    def length8(position: int) -> tuple[int, int]:
        # One byte, or two when the high bit of the first is set.
        (first,) = struct.unpack_from("B", manifest, position)
        if first & 0x80:
            (second,) = struct.unpack_from("B", manifest, position + 1)
            return (first & 0x7F) << 8 | second, position + 2
        return first, position + 1

    strings = []
    for index in range(count):
        position = chunk + strings_start + struct.unpack_from("<I", manifest, chunk + header + 4 * index)[0]
        if flags & AXML_UTF8_POOL:
            _, position = length8(position)  # the character count, then the byte count
            length, position = length8(position)
            encoding = "utf-8"
        else:
            (length,) = struct.unpack_from("<H", manifest, position)
            position += 2
            if length & 0x8000:
                length = (length & 0x7FFF) << 16 | struct.unpack_from("<H", manifest, position)[0]
                position += 2
            length *= 2
            encoding = "utf-16-le"
        text = manifest[position:position + length]
        if position + length > chunk + size or len(text) != length:
            raise ValueError("truncated compiled XML string")
        strings.append(text.decode(encoding))
    return strings


# The manifest elements that request a permission.
PERMISSION_REQUEST_ELEMENTS = {"uses-permission", "uses-permission-sdk-23", "uses-permission-sdk-m"}


def manifest_requested_permissions(manifest: bytes) -> set[str]:
    """The permissions a compiled manifest requests with <uses-permission>.

    A manifest that declares a permission (framework-res.apk declares every platform permission)
    or guards a component with it does not request it, and is not listed."""
    if len(manifest) < 8 or struct.unpack_from("<HH", manifest, 0)[0] != 0x0003:
        raise ValueError("not a compiled XML file")
    strings: list[str] = []
    resource_ids: list[int] = []
    requested: set[str] = set()
    offset = struct.unpack_from("<H", manifest, 2)[0]
    while offset + 8 <= len(manifest):
        kind, header, size = struct.unpack_from("<HHI", manifest, offset)
        if size < 8 or offset + size > len(manifest):
            raise ValueError("truncated compiled XML chunk")
        if kind == AXML_STRING_POOL:
            strings = axml_strings(manifest, offset)
        elif kind == AXML_RESOURCE_MAP:
            resource_ids = list(struct.unpack_from(f"<{(size - header) // 4}I", manifest, offset + header))
        elif kind == AXML_START_ELEMENT:
            body = offset + header
            element = struct.unpack_from("<I", manifest, body + 4)[0]
            if element < len(strings) and strings[element] in PERMISSION_REQUEST_ELEMENTS:
                attribute_start, attribute_size, count = struct.unpack_from("<HHH", manifest, body + 8)
                for index in range(count):
                    position = body + attribute_start + index * attribute_size
                    name, raw, _, data_type, data = struct.unpack_from("<IIHxBI", manifest, position + 4)
                    if name < len(resource_ids) and resource_ids[name] == ATTR_NAME and data_type == AXML_STRING_TYPE:
                        if data >= len(strings):
                            raise ValueError("a permission name outside the string pool")
                        requested.add(strings[data])
        offset += size
    return requested


def manifest_target_sdk(manifest: bytes) -> int | None:
    """The targetSdkVersion of a compiled manifest, or None when it has none or it is not a number."""
    value = manifest_attribute(manifest, ATTR_TARGET_SDK_VERSION)
    if value is None:
        return None
    data_type, data = value
    return data if data_type in AXML_INTEGER_TYPES else None


def manifest_debuggable(manifest: bytes) -> bool:
    """Whether a compiled manifest sets android:debuggable to true; a value that is not a literal boolean is an error."""
    value = manifest_attribute(manifest, ATTR_DEBUGGABLE)
    if value is None:
        return False
    data_type, data = value
    if data_type != AXML_BOOLEAN_TYPE:
        raise ValueError("android:debuggable is not a literal boolean")
    return data != 0


def check_pinner_overlay(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The home pin overlay is built for SDK 28 or lower and carries the pin size it sets."""
    info = infos.get(PINNER_OVERLAY)
    if info is None:
        fail(f"{PINNER_OVERLAY} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info))) as overlay:
            manifest = overlay.read("AndroidManifest.xml")
            table = overlay.read("resources.arsc")
    except (KeyError, OSError, zipfile.BadZipFile) as error:
        fail(f"{PINNER_OVERLAY} has no readable manifest and resource table: {error}")
    try:
        target = manifest_target_sdk(manifest)
    except (ValueError, struct.error) as error:
        fail(f"{PINNER_OVERLAY} has an unreadable manifest: {error}")
    if target is None or target > PINNER_OVERLAY_MAX_TARGET_SDK:
        fail(
            f"{PINNER_OVERLAY} targets SDK {target}: PinnerService's pin sizes are not overlayable, so "
            f"the overlay applies only when it targets {PINNER_OVERLAY_MAX_TARGET_SDK} or lower "
            "(its Android.bp builds it with sdk_version \"28\")"
        )
    # Resource names sit in the table's key string pool as UTF-8 or UTF-16.
    if PINNER_OVERLAY_RESOURCE not in table and PINNER_OVERLAY_RESOURCE.decode().encode("utf-16-le") not in table:
        fail(f"{PINNER_OVERLAY} does not set {PINNER_OVERLAY_RESOURCE.decode()}")


# The permission that lets an application start Android's trade-in mode (TradeInModeService in
# system_server). An unprovisioned device enters it a few tens of seconds after boot when an
# application asks, which turns USB debugging on and restarts adbd twice. A virtual device has no
# use for it, and the service has no configuration switch, so the product carries no application
# that asks at boot: AOSP's DeviceDiagnostics, whose boot receiver does, is excluded from the
# product. The shell package holds the permission too, for the commands a person types over adb;
# nothing in it starts the mode by itself. Any other APK of the image partitions that requests the
# permission is refused.
TRADE_IN_MODE_PERMISSION = "android.permission.ENTER_TRADE_IN_MODE"
TRADE_IN_MODE_REQUESTERS = {"SYSTEM/priv-app/Shell/Shell.apk"}


def check_application_manifests(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo], build_variant: str
) -> None:
    """No application opts in to debugging (a user build) or can start trade-in mode (every build).

    A debuggable application is open to `run-as` and to a debugger whatever ro.debuggable says,
    and release signing would not change that, so a test probe left in the shared product
    output (or a prebuilt built with --debug-mode) must stop the build here. Only a user build
    is checked for it: a userdebug build is the diagnostics variant. Trade-in mode can start only
    for an application that requests ENTER_TRADE_IN_MODE, and the privapp allowlist's entry for
    a package that is not installed grants nothing, so the request is what the check looks for,
    in every variant (a userdebug build never enters it, but the product list is one list), in
    every APK but the shell's (TRADE_IN_MODE_REQUESTERS). The
    check reads every APK of the image partitions; APKs inside an APEX payload are compressed
    EROFS and out of its reach.
    """
    for entry in sorted(infos):
        info = infos[entry]
        path = PurePosixPath(entry)
        if (
            info.is_dir()
            or path.suffix != ".apk"
            or path.parts[0] not in TARGET_PARTITIONS
            # A symbolic link holds its target's path; the target is checked as its own entry.
            or stat.S_ISLNK(info.external_attr >> 16)
        ):
            continue
        try:
            with zipfile.ZipFile(io.BytesIO(archive.read(info))) as apk:
                manifest = apk.read("AndroidManifest.xml")
            debuggable = build_variant == "user" and manifest_debuggable(manifest)
            requested = manifest_requested_permissions(manifest)
        except (KeyError, OSError, zipfile.BadZipFile, ValueError, struct.error) as error:
            fail(f"{entry} has no readable manifest: {error}")
        if debuggable:
            fail(f"a user build must not carry a debuggable application: {entry}")
        if TRADE_IN_MODE_PERMISSION in requested and entry not in TRADE_IN_MODE_REQUESTERS:
            fail(
                f"{entry} requests {TRADE_IN_MODE_PERMISSION}: trade-in mode would start on an "
                "unprovisioned first boot and restart adbd twice; the product must not carry "
                "an application that can start it (DeviceDiagnostics is excluded)"
            )


# The AOSP example HALs the product keeps, each packaged as the vendor APEX that Cuttlefish
# installs. The APEX payload holds the service binary, the VINTF fragment that declares the AIDL
# interface, and the init script that starts the service. The payload is compressed EROFS: the
# fragment and the script stay readable in its bytes because the payload's compress hints
# (overlay/config/erofs_apex_payload_hints.txt) store every .rc and .xml file uncompressed, and
# the binary is looked up in the payload's directories, since the init script names it too.
APEX_PAYLOAD_READ_LIMIT = 64 << 20
# system_server's thermal manager and CTS NativeThermalTest need a thermal HAL.
THERMAL_HAL = {
    "apex": "VENDOR/apex/com.android.hardware.thermal.apex",
    "binary": b"android.hardware.thermal-service.example",
    "fragment": (
        b"<name>android.hardware.thermal</name>",
        b"<fqname>IThermal/default</fqname>",
    ),
    "interface": "IThermal/default",
    "service": "vendor.thermal-example",
    "need": "a thermal HAL",
}
# MediaCas and CTS MediaCasTest need the default CAS HAL, which loads the ClearKey CAS plugin
# (CAS system id 63192) from /vendor/lib*/mediacas.
CAS_HAL = {
    "apex": "VENDOR/apex/com.android.hardware.cas.apex",
    "binary": b"android.hardware.cas-service.example",
    "fragment": (
        b"<name>android.hardware.cas</name>",
        b"<fqname>IMediaCasService/default</fqname>",
    ),
    "interface": "IMediaCasService/default",
    "service": "vendor.cas-default",
    "need": "the default CAS HAL",
}
# The virtual Wi-Fi needs the Wi-Fi HAL and the supplicant, which Cuttlefish packages as vendor
# APEXes (the supplicant one holds hostapd too). The HAL APEX's init script names the binary
# below /apex/com.android.hardware.wifi.
WIFI_HAL = {
    "apex": "VENDOR/apex/com.android.hardware.wifi.apex",
    "binary": b"android.hardware.wifi-service",
    "fragment": (
        b"<name>android.hardware.wifi</name>",
        b"<fqname>IWifi/default</fqname>",
    ),
    "interface": "IWifi/default",
    "service": "vendor.wifi_hal_legacy",
    "need": "the Wi-Fi HAL",
}
WIFI_SUPPLICANT = {
    "apex": "VENDOR/apex/com.google.cf.wpa_supplicant.apex",
    "binary": b"wpa_supplicant",
    "fragment": (
        b"<name>android.hardware.wifi.supplicant</name>",
        b"<fqname>ISupplicant/default</fqname>",
    ),
    "interface": "ISupplicant/default",
    "service": "wpa_supplicant",
    "need": "the Wi-Fi supplicant",
}
CAS_PLUGINS = (
    "VENDOR/lib64/mediacas/libclearkeycasplugin.so",
    "VENDOR/lib/mediacas/libclearkeycasplugin.so",
)


def read_apex_payload(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo], apex_name: str, need: str
) -> bytes:
    """The EROFS payload of a required APEX."""
    info = infos.get(apex_name)
    if info is None:
        fail(f"{apex_name} is missing: the product needs {need}")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, 16 << 20))) as apex:
            payload_info = apex.getinfo("apex_payload.img")
            if payload_info.file_size > APEX_PAYLOAD_READ_LIMIT:
                fail(f"{apex_name} has an unexpectedly large payload")
            payload = apex.read(payload_info)
    except (KeyError, OSError, zipfile.BadZipFile) as problem:
        fail(f"cannot inspect {apex_name}: {problem}")
    if payload[android_image.EROFS_SUPERBLOCK_OFFSET :][:4] != android_image.EROFS_MAGIC:
        fail(f"cannot inspect {apex_name}: the payload is not EROFS")
    return payload


def check_payload_regular_file(payload: bytes, path: str, apex_name: str, what: str) -> None:
    """The payload's directories hold a regular file at this path."""
    try:
        file_type = android_image.erofs_file_type(payload, path, apex_name, error)
    except (struct.error, ValueError) as parse_error:
        fail(f"cannot read the directories of {apex_name}: {parse_error}")
    if file_type != android_image.EROFS_FILE_TYPE_REGULAR:
        fail(f"{apex_name} does not hold {what} {path}")


# The framework's AdbService carries the p0007 topics that record the USB debugging setting in
# persist.sys.usb.config and restart adbd, where stock stops it, when debugging is turned off.
# The jar stores its dex files uncompressed, so the topic's log line is in the bytes. A build
# without it would forget the user's choice at every boot and stop the host's channel.
SERVICES_JAR = "SYSTEM/framework/services.jar"
SERVICES_JAR_MARKERS = (b"kernarium: USB debugging is now ",)
SERVICES_JAR_LIMIT = 48 << 20


def check_services_jar(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    info = infos.get(SERVICES_JAR)
    if info is None:
        fail(f"{SERVICES_JAR} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, SERVICES_JAR_LIMIT))) as jar:
            dex = [
                name for name in jar.namelist() if re.fullmatch(r"classes[0-9]*\.dex", name)
            ]
            if not dex:
                fail(f"{SERVICES_JAR} holds no dex file")
            for marker in SERVICES_JAR_MARKERS:
                if not any(marker in jar.read(name) for name in dex):
                    fail(
                        f"{SERVICES_JAR} lacks the AdbService topics of route p0007 "
                        f"({marker.decode()!r} is not in its dex files)"
                    )
    except (OSError, zipfile.BadZipFile) as problem:
        fail(f"cannot inspect {SERVICES_JAR}: {problem}")


def check_ota_certificates(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo], build_variant: str, signed: bool
) -> None:
    """The system's and the recovery's otacerts.zip each hold the one member the build state names.

    An unsigned build of either variant holds the AOSP test certificate's file name. The signed
    target files of a release trust the release certificate alone: sign_target_files_apks names it
    after the path it is given for the release key, so a signed archive holds that one member and
    an archive still carrying the test certificate holds another. Only a user build is signed, with
    the one release key set; the signer checks the certificate's digest, which needs the keys.
    """
    member = ota_certificate_member(build_variant, signed)
    state = "signed" if signed else "unsigned"
    for name in OTA_CERTIFICATE_ARCHIVES:
        if name not in infos:
            fail(f"{name} is missing from the {state} target files")
        try:
            with zipfile.ZipFile(io.BytesIO(read_small(archive, infos[name]))) as certificates:
                members = sorted(certificates.namelist())
        except zipfile.BadZipFile as problem:
            fail(f"{name} is not a zip archive: {problem}")
        if members != [member]:
            fail(
                f"{name} holds the members {members}, not only {member}: the target files are "
                + ("not signed with the release keys" if signed else "not an unsigned build")
            )


def check_adbd_launch_properties(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The APEX's adbd reads the host application's key and the launch's TCP policy.

    The payload hints store bin/adbd uncompressed, so the names that the p0011 topics add are
    in the payload's bytes. A build whose adbd lacks them would ignore the key and keep
    listening on guest TCP, which no other check would notice.
    """
    payload = read_apex_payload(archive, infos, ADBD_APEX, "the adb daemon")
    check_payload_regular_file(payload, ADBD_BINARY, ADBD_APEX, "the adb daemon")
    for name in ADBD_LAUNCH_PROPERTIES + ADBD_DEBUGGING_PROPERTIES:
        if name.encode() not in payload:
            fail(
                f"{ADBD_APEX} holds an adbd that does not read {name}: the p0011 topics are "
                "missing from the build, or the payload hints no longer store bin/adbd "
                "uncompressed"
            )
    if ADBD_REFUSAL_MARKER not in payload:
        fail(
            f"{ADBD_APEX} holds an adbd without the key rule of route p0011 "
            f"({ADBD_REFUSAL_MARKER.decode()!r} is not in bin/adbd): it would offer every "
            "key a prompt while debugging is off"
        )


def check_adb_root_service(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The adb_root service must not turn rooted debugging on for a build that is not debuggable.

    Stock adb_root has no such rule and the bypass topic of route p0011 would otherwise enable it on
    every build whose boot policy skips authorization, which makes `adb root` on a user build print
    "restarting adbd as root" and wait for a restart that no init trigger performs. The service binary
    is in the target files as a file, so the guarded topic's log line is in its bytes.
    """
    info = infos.get(ADB_ROOT_BINARY)
    if info is None or info.is_dir():
        fail(f"{ADB_ROOT_BINARY} is missing: the product needs LineageOS's adb_root service")
    if ADB_ROOT_GUARD_MARKER not in read_small(archive, info, 64 << 20):
        fail(
            f"{ADB_ROOT_BINARY} does not hold the debuggable-build rule of route p0011 "
            f"({ADB_ROOT_GUARD_MARKER.decode()!r} is not in it): it would enable rooted debugging on "
            "a user build, whose `adb root` then waits for an adbd restart that never comes"
        )


def check_example_hal(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo], hal: dict
) -> None:
    """The vendor APEX must hold the example service, its VINTF fragment and its init script."""
    apex_name = hal["apex"]
    binary = hal["binary"]
    payload = read_apex_payload(archive, infos, apex_name, hal["need"])
    check_payload_regular_file(payload, f"bin/hw/{binary.decode()}", apex_name, "the service binary")
    if not all(marker in payload for marker in hal["fragment"]):
        fail(f"{apex_name} does not declare {hal['interface']} in a VINTF fragment")
    apex_dir = PurePosixPath(apex_name).stem
    service = f"service {hal['service']} /apex/{apex_dir}/bin/hw/".encode() + binary
    if service not in payload:
        fail(f"{apex_name} does not start the {hal['service']} service")


def check_thermal_hal(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    check_example_hal(archive, infos, THERMAL_HAL)


def check_wifi_hals(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    check_example_hal(archive, infos, WIFI_HAL)
    check_example_hal(archive, infos, WIFI_SUPPLICANT)


def check_cas_hal(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    check_example_hal(archive, infos, CAS_HAL)
    for plugin in CAS_PLUGINS:
        if plugin not in infos:
            fail(f"the ClearKey CAS plugin is missing: {plugin}")


def check_motion_sensors(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    configuration = read_small(archive, infos["VENDOR/etc/sensors/hals.conf"]).decode().strip()
    if configuration != "/vendor/lib64/hw/android.hardware.sensors@2.1-impl.andarium.so":
        fail("motion multi-HAL must load only the Andarium backend")
    fragment = ET.fromstring(read_small(
        archive, infos["VENDOR/etc/vintf/manifest/android.hardware.sensors-multihal.xml"]
    ))
    if not any(hal.get("format") == "aidl"
               and hal.findtext("name") == "android.hardware.sensors"
               and hal.findtext("fqname") == "ISensors/default" for hal in fragment.findall("hal")):
        fail("motion sensor HAL is not declared as AIDL ISensors/default")
    script = read_small(archive, infos[
        "VENDOR/etc/init/android.hardware.sensors-service-multihal.rc"
    ]).decode()
    if not re.search(r"^service \S+ /vendor/bin/hw/android\.hardware\.sensors-service\.multihal$", script, re.M):
        fail("motion sensor HAL service does not start the multi-HAL binary")


def check_hardware_features(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    declared: set[str] = set()
    deqp_levels: list[str | None] = []
    for name, info in infos.items():
        path = PurePosixPath(name)
        if (
            info.is_dir()
            or path.suffix != ".xml"
            or "permissions" not in path.parts
        ):
            continue
        try:
            root = ET.fromstring(read_small(archive, info))
        except ET.ParseError as error:
            fail(f"cannot parse hardware permission XML {name}: {error}")
        for element in root.iter():
            if element.tag.rsplit("}", 1)[-1] != "feature":
                continue
            feature = element.attrib.get("name")
            if feature in FORBIDDEN_FEATURE_NAMES:
                fail(f"absent hardware feature is advertised in {name}: {feature}")
            if feature == "android.software.vulkan.deqp.level":
                fail(f"a Vulkan dEQP level is declared in {name}: the Vulkan lists are not qualified")
            if feature == OPENGLES_DEQP_LEVEL_FEATURE:
                deqp_levels.append(element.attrib.get("version"))
            if feature:
                declared.add(feature)
    for feature in sorted(REQUIRED_FEATURE_NAMES - declared):
        fail(f"required feature is not declared: {feature}")
    if deqp_levels != [str(OPENGLES_DEQP_LEVEL)]:
        fail(
            f"{OPENGLES_DEQP_LEVEL_FEATURE} must be declared once at the qualified "
            f"level {OPENGLES_DEQP_LEVEL}: {deqp_levels}"
        )


def app_preopt_paths(apk: str, isa: str = "arm64") -> tuple[str, str]:
    apk_path = PurePosixPath(apk)
    artifact_base = apk_path.parent / "oat" / isa / apk_path.stem
    return f"{artifact_base}.odex", f"{artifact_base}.vdex"


def check_app_dexpreopt(seen: set[str]) -> None:
    for apk in sorted(REQUIRED_PREOPT_APKS):
        if apk not in seen:
            fail(f"required preoptimized application is missing: {apk}")
        for artifact in app_preopt_paths(apk):
            if artifact not in seen:
                fail(f"required application dexpreopt artifact is missing: {artifact}")
    # An application that carries libraries for both ABIs runs under either
    # zygote, so ART Service would compile its secondary ISA on first boot.
    for apk in sorted(REQUIRED_ARM_PREOPT_APKS):
        if apk not in seen:
            fail(f"required multi-ABI application is missing: {apk}")
        for artifact in app_preopt_paths(apk, "arm"):
            if artifact not in seen:
                fail(f"required 32-bit application dexpreopt artifact is missing: {artifact}")

    for canonical in sorted(seen):
        path = PurePosixPath(canonical)
        if path.suffix not in {".odex", ".vdex"}:
            continue
        if len(path.parts) < 6 or path.parts[1] not in {"app", "priv-app"}:
            continue
        if path.parts[-3] != "oat":
            continue
        if path.parts[-2] not in APP_ISAS:
            fail(f"application dexpreopt artifact is not for an ISA of this product: {path}")
        companion_suffix = ".vdex" if path.suffix == ".odex" else ".odex"
        companion = str(path.with_suffix(companion_suffix))
        if companion not in seen:
            fail(f"application dexpreopt artifact lacks its pair: {path}")
        if path.parts[-2] == "arm":
            # The secondary ISA only ever adds to the primary one.
            primary = path.parent.parent / "arm64" / path.name
            if str(primary) not in seen:
                fail(f"32-bit application dexpreopt artifact has no ARM64 one: {path}")


# The virtual Wi-Fi needs these built in: kernarium_net_setup stacks a macvlan on eth0 and a
# virt_wifi device on that, and a kernel without them leaves apps no network at all.
WIFI_KERNEL_OPTIONS = ("CONFIG_MACVLAN=y", "CONFIG_CFG80211=y", "CONFIG_VIRT_WIFI=y")
# First-stage init mounts the five read-only partitions as compressed EROFS before any module
# could load, so the kernel needs the file system and the product's decompressor built in.
EROFS_KERNEL_OPTIONS = (
    "CONFIG_EROFS_FS=y",
    "CONFIG_EROFS_FS_ZIP=y",
    "CONFIG_EROFS_FS_ZIP_ZSTD=y",
)


def check_kernel_policy(
    archive: zipfile.ZipFile,
    infos: dict[str, zipfile.ZipInfo],
    kernel_config: Path,
    kernel_release_file: Path,
    build_variant: str,
) -> None:
    packaged_config = read_small(archive, infos["META/kernel_configs.txt"])
    if packaged_config != kernel_config.read_bytes():
        fail("packaged kernel configuration differs from the UML build")
    packaged_options = {
        line.strip() for line in packaged_config.decode("utf-8", errors="replace").splitlines()
    }
    for option in WIFI_KERNEL_OPTIONS:
        if option not in packaged_options:
            fail(
                f"packaged kernel configuration lacks {option}: the virtual Wi-Fi needs it, "
                "and without it apps have no network"
            )
    for option in EROFS_KERNEL_OPTIONS:
        if option not in packaged_options:
            fail(
                f"packaged kernel configuration lacks {option}: the read-only partitions "
                "are compressed EROFS, and first-stage init cannot mount them without it"
            )

    packaged_release = read_small(
        archive, infos["META/kernel_version.txt"]
    ).decode("utf-8", errors="strict").strip()
    kernel_release = kernel_release_file.read_text(
        encoding="utf-8", errors="strict"
    ).strip()
    if not kernel_release or packaged_release != kernel_release:
        fail("packaged kernel release differs from the UML build")

    vendor_policy = read_small(
        archive, infos["VENDOR/etc/selinux/vendor_sepolicy.cil"]
    )
    if b"(policycap memfd_class)" not in vendor_policy:
        fail("Android 17 vendor policy does not enable policycap memfd_class")
    # Applications must read the vendor patch level that LineageOS lets
    # system_server report to them (overlay/sepolicy/vendor/appdomain.te).
    if not VENDOR_SPL_APP_READ.search(vendor_policy):
        fail("vendor policy does not let applications read vendor_security_patch_level_prop")
    if not AUDIO_HAL_SELF_BINDER.search(vendor_policy):
        fail("vendor policy does not let the audio HAL processes call each other's binder")
    if not WIFI_HAL_DEBUG_PROPERTY_READ.search(vendor_policy):
        fail("vendor policy does not let the Wi-Fi HAL read persist_vendor_debug_wifi_prop")
    check_clearkey_memfd_policy(vendor_policy)
    check_userdata_grow_policy(vendor_policy)
    check_network_setup_policy(
        vendor_policy, read_small(archive, infos["VENDOR/etc/selinux/vendor_file_contexts"])
    )
    platform_policy = read_small(
        archive, infos["SYSTEM/etc/selinux/plat_sepolicy.cil"], PLATFORM_POLICY_LIMIT
    )
    system_ext_policy = read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_sepolicy.cil"])
    check_wifi_network_policy(
        system_ext_policy,
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_file_contexts"]),
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_property_contexts"]).decode(
            "utf-8", errors="strict"
        ),
    )
    check_reclaim_boot_apps_policy(
        system_ext_policy,
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_file_contexts"]),
    )
    check_time_zone_policy(
        system_ext_policy,
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_file_contexts"]),
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_property_contexts"]).decode(
            "utf-8", errors="strict"
        ),
    )
    check_property_writers(
        platform_policy,
        vendor_policy,
        system_ext_policy,
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_property_contexts"]).decode(
            "utf-8", errors="strict"
        ),
    )
    check_policy_relaxations((platform_policy, vendor_policy, system_ext_policy), build_variant)
    check_mediaserver_property_audit((platform_policy, vendor_policy, system_ext_policy))
    contexts = [
        line
        for name in PROPERTY_CONTEXT_FILES
        if name in infos
        for line in read_small(archive, infos[name], PLATFORM_POLICY_LIMIT)
        .decode("utf-8", errors="strict")
        .splitlines()
    ]
    check_vendor_init_triggers(
        init_actions(
            read_small(archive, infos["VENDOR/etc/init/init.kernarium.rc"]).decode(
                "utf-8", errors="strict"
            )
        ),
        contexts,
        (platform_policy, vendor_policy, system_ext_policy),
    )
    check_adbd_launch_policy(contexts, (platform_policy, vendor_policy, system_ext_policy))
    check_lmkd_gpu_policy(contexts, (platform_policy, vendor_policy, system_ext_policy), vendor_policy)
    check_venus_option_policy(contexts, (platform_policy, vendor_policy, system_ext_policy))
    check_audio_ring_policy(contexts, (platform_policy, vendor_policy, system_ext_policy))


def media_codec_limit(codec: ET.Element, name: str) -> ET.Element:
    limits = [limit for limit in codec.findall("Limit") if limit.get("name") == name]
    if len(limits) != 1:
        fail(f"media_codecs_c2.xml: {codec.get('name')} needs exactly one {name} limit")
    return limits[0]


def check_media_codecs_c2(payload: bytes) -> None:
    """The software HEVC and VP9 decoders must be allowed 3840x2160."""
    try:
        root = ET.fromstring(payload)
    except ET.ParseError as error:
        fail(f"media_codecs_c2.xml is not well-formed: {error}")
    decoders = {
        codec.get("name"): codec for codec in root.findall("./Decoders/MediaCodec")
    }
    for name in ("c2.android.hevc.decoder", "c2.android.vp9.decoder"):
        codec = decoders.get(name)
        if codec is None or codec.get("update") != "true":
            fail(f"media_codecs_c2.xml does not update {name}")

    def upper(limit: ET.Element) -> int:
        return int(limit.get("range", "0-0").split("-")[1])

    hevc_rate = upper(
        media_codec_limit(decoders["c2.android.hevc.decoder"], "blocks-per-second")
    )
    if hevc_rate < MEDIA_C2_HEVC_BLOCKS_PER_SECOND:
        fail(
            f"media_codecs_c2.xml allows the HEVC decoder only {hevc_rate} blocks per second; "
            f"3840x2160 at 30 frames per second needs {MEDIA_C2_HEVC_BLOCKS_PER_SECOND}"
        )
    vp9 = decoders["c2.android.vp9.decoder"]
    width, height = (int(side) for side in media_codec_limit(vp9, "size").get("max", "0x0").split("x"))
    if width < MEDIA_C2_SIZE[0] or height < MEDIA_C2_SIZE[1]:
        fail(f"media_codecs_c2.xml limits the VP9 decoder to {width}x{height}, below 3840x2160")
    vp9_blocks = upper(media_codec_limit(vp9, "block-count"))
    if vp9_blocks < MEDIA_C2_VP9_BLOCK_COUNT:
        fail(
            f"media_codecs_c2.xml allows the VP9 decoder {vp9_blocks} blocks, below "
            f"{MEDIA_C2_VP9_BLOCK_COUNT} for 3840x2160"
        )


def cil_attribute_members(policy: bytes, attribute: str) -> set[str]:
    """Types of a CIL attribute, platform version suffixes removed."""
    pattern = re.compile(
        rb"^\(typeattributeset " + re.escape(attribute.encode()) + rb" \(([^)]*)\)\)$",
        re.MULTILINE,
    )
    return {
        CIL_PLATFORM_VERSION_SUFFIX.sub("", name.decode())
        for match in pattern.finditer(policy)
        for name in match.group(1).split()
    }


def check_property_writers(
    platform_policy: bytes,
    vendor_policy: bytes,
    system_ext_policy: bytes,
    system_ext_contexts: str,
) -> None:
    """A core domain may set no vendor-owned property, and adbd reports its host.

    The first rule repeats the platform neverallow of private/property.te so a
    product rule that breaks it fails here and not only in the policy build.
    """
    core_domains = cil_attribute_members(platform_policy, "coredomain") - {"init"}
    vendor_properties = cil_attribute_members(
        vendor_policy, "vendor_property_type"
    ) | cil_attribute_members(system_ext_policy, "vendor_property_type")
    for label, policy in (("vendor", vendor_policy), ("system_ext", system_ext_policy)):
        for match in CIL_PROPERTY_SET_RULE.finditer(policy):
            source = CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode())
            target = CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode())
            if b"set" not in match.group(3).split():
                continue
            if source in core_domains and target in vendor_properties:
                fail(
                    f"{label} policy lets the core domain {source} set the vendor "
                    f"property type {target}; a core domain may set only a "
                    "system-owned property"
                )
    system_properties = cil_attribute_members(system_ext_policy, "system_property_type")
    if ADB_HOST_PROPERTY_TYPE not in system_properties:
        fail(f"system_ext policy does not define the system-owned {ADB_HOST_PROPERTY_TYPE}")
    allowed = False
    for match in CIL_PROPERTY_SET_RULE.finditer(system_ext_policy):
        if (
            match.group(1) == b"adbd"
            and match.group(2).decode() == ADB_HOST_PROPERTY_TYPE
            and b"set" in match.group(3).split()
        ):
            allowed = True
    if not allowed:
        fail(f"system_ext policy does not let adbd set {ADB_HOST_PROPERTY}")
    if not any(
        line.split()[:2] == [ADB_HOST_PROPERTY, f"u:object_r:{ADB_HOST_PROPERTY_TYPE}:s0"]
        for line in system_ext_contexts.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ):
        fail(f"system_ext property contexts do not label {ADB_HOST_PROPERTY}")


def check_clearkey_memfd_policy(vendor_policy: bytes) -> None:
    """The ClearKey HAL may use the memfd files of application domains, not of isolated apps."""
    for match in CLEARKEY_MEMFD_RULE.finditer(vendor_policy):
        permissions = set(match.group(2).decode().split())
        if not CLEARKEY_MEMFD_PERMISSIONS <= permissions:
            continue
        target = match.group(1).decode()
        expression = re.search(
            rb"^\(typeattributeset " + re.escape(target.encode())
            + rb" \(and \(appdomain(?:_[0-9]+)? ?\) \(not \(([^)]*)\) ?\)\)\)$",
            vendor_policy,
            re.MULTILINE,
        )
        if expression is not None and "isolated_app_all" in {
            CIL_PLATFORM_VERSION_SUFFIX.sub("", name)
            for name in expression.group(1).decode().split()
        }:
            return
    fail(
        "vendor policy does not let the ClearKey DRM HAL read, write and map the memfd files "
        "of application domains other than isolated apps"
    )


def file_context_label(contexts: bytes, path: str) -> str | None:
    """The context of the last file_contexts line whose expression matches |path|."""
    label = None
    for line in contexts.decode("utf-8", errors="strict").splitlines():
        fields = line.split()
        if len(fields) < 2 or fields[0].startswith("#"):
            continue
        try:
            matched = re.fullmatch(fields[0], path)
        except re.error:
            continue
        if matched:
            label = fields[-1]
    return label


def check_network_setup_policy(vendor_policy: bytes, vendor_file_contexts: bytes) -> None:
    """The network setup tool runs in its own domain, not in init's."""
    if file_context_label(vendor_file_contexts, NET_SETUP_SERVICE[1]) != (
        "u:object_r:kernarium_net_setup_exec:s0"
    ):
        fail("vendor file contexts do not label /vendor/bin/kernarium_net_setup")
    if not NET_SETUP_TRANSITION.search(vendor_policy):
        fail("vendor policy does not move init into the network setup domain")


def check_wifi_network(rc_payload: str, script: bytes) -> None:
    """The script that saves VirtWifi runs once, after boot, and leaves the Wi-Fi switch alone."""
    name, executable = WIFI_NETWORK_SERVICE
    service = init_services(rc_payload).get(name)
    if service is None or service[0] != executable:
        fail(f"system_ext init policy does not declare the {name} service at {executable}")
    for option in ("oneshot", "disabled"):
        if option not in service[1]:
            fail(f"system_ext init policy does not declare {name} as {option}")
    actions = init_actions(rc_payload)
    for trigger, commands in actions.items():
        if f"start {name}" in commands and trigger != WIFI_NETWORK_TRIGGER:
            fail(f"system_ext init policy starts {name} under '{trigger}'")
    if f"start {name}" not in actions.get(WIFI_NETWORK_TRIGGER, []):
        fail(f"system_ext init policy does not start {name} when boot completes")
    code = "\n".join(
        line
        for line in script.decode("utf-8", errors="strict").splitlines()
        if not line.lstrip().startswith("#")
    )
    for token in WIFI_NETWORK_SCRIPT_REQUIRED:
        if token not in code:
            fail(f"{executable} does not contain '{token}'")
    for token in WIFI_NETWORK_SCRIPT_FORBIDDEN:
        if token in code:
            fail(f"{executable} contains '{token}': the user's Wi-Fi switch is not the product's")


def check_wifi_network_policy(
    system_ext_policy: bytes, system_ext_file_contexts: bytes, system_ext_property_contexts: str
) -> None:
    """The script runs in its own core domain and may set only its own marker."""
    if file_context_label(system_ext_file_contexts, WIFI_NETWORK_SERVICE[1]) != (
        "u:object_r:kernarium_wifi_network_exec:s0"
    ):
        fail(f"system_ext file contexts do not label {WIFI_NETWORK_SERVICE[1]}")
    if not WIFI_NETWORK_TRANSITION.search(system_ext_policy):
        fail("system_ext policy does not move init into the Wi-Fi network domain")
    if WIFI_NETWORK_MARKER_TYPE not in cil_attribute_members(
        system_ext_policy, "system_property_type"
    ):
        fail(f"system_ext policy does not define the system-owned {WIFI_NETWORK_MARKER_TYPE}")
    if not any(
        match.group(1) == b"kernarium_wifi_network"
        and match.group(2).decode() == WIFI_NETWORK_MARKER_TYPE
        and b"set" in match.group(3).split()
        for match in CIL_PROPERTY_SET_RULE.finditer(system_ext_policy)
    ):
        fail(f"system_ext policy does not let the Wi-Fi network domain set {WIFI_NETWORK_MARKER}")
    if not any(
        CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) == "kernarium_wifi_network"
        and CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode()) == "system_server"
        and match.group(3) == b"binder"
        and b"transfer" in match.group(4).split()
        for match in CIL_ALLOW_RULE.finditer(system_ext_policy)
    ):
        fail(
            "system_ext policy does not let the Wi-Fi network domain transfer binders to "
            "system_server: cmd hands the Wi-Fi service the receiver of its result, and "
            "without the rule the network is never saved"
        )
    if not any(
        CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) == "system_server"
        and CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode()) == "kernarium_wifi_network"
        and match.group(3) == b"binder"
        and b"call" in match.group(4).split()
        for match in CIL_ALLOW_RULE.finditer(system_ext_policy)
    ):
        fail(
            "system_ext policy does not let system_server call the Wi-Fi network domain's "
            "binder: cmd waits for the receiver of its result, so without the rule it never "
            "returns and the script never sets its marker"
        )
    if not any(
        line.split()[:2] == [WIFI_NETWORK_MARKER, f"u:object_r:{WIFI_NETWORK_MARKER_TYPE}:s0"]
        for line in system_ext_property_contexts.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ):
        fail(f"system_ext property contexts do not label {WIFI_NETWORK_MARKER}")


def check_reclaim_boot_apps(rc_payload: str, script: bytes) -> None:
    """The reclaim script runs once after boot, kills only cached processes of the reviewed list."""
    name, executable = RECLAIM_SERVICE
    service = init_services(rc_payload).get(name)
    if service is None or service[0] != executable:
        fail(f"system_ext init policy does not declare the {name} service at {executable}")
    for option in ("oneshot", "disabled"):
        if option not in service[1]:
            fail(f"system_ext init policy does not declare {name} as {option}")
    actions = init_actions(rc_payload)
    for trigger, commands in actions.items():
        if f"start {name}" in commands and trigger != RECLAIM_TRIGGER:
            fail(f"system_ext init policy starts {name} under '{trigger}'")
    if f"start {name}" not in actions.get(RECLAIM_TRIGGER, []):
        fail(f"system_ext init policy does not start {name} when boot completes")
    text = script.decode("utf-8", errors="strict")
    code = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
    for token in RECLAIM_SCRIPT_REQUIRED:
        if token not in code:
            fail(f"{executable} does not contain '{token}'")
    for token in RECLAIM_SCRIPT_FORBIDDEN:
        if token in code.replace(RECLAIM_SETTLE, ""):
            fail(
                f"{executable} contains '{token}': it may only kill the cached processes of "
                "the reviewed list, which keeps alarms, jobs and bindings"
            )
    listed = RECLAIM_LIST.search(text)
    if listed is None:
        fail(f"{executable} has no reclaim_list block")
    entries = tuple(
        (fields[0], tuple(fields[1:]))
        for fields in (line.split() for line in listed.group(1).splitlines())
        if fields
    )
    if entries != RECLAIM_APPS:
        fail(
            f"{executable} lists {entries}, not the reviewed packages {RECLAIM_APPS}: "
            "a change to the list is a reviewed change to this verifier too"
        )


def check_reclaim_boot_apps_policy(system_ext_policy: bytes, system_ext_file_contexts: bytes) -> None:
    """The reclaim script runs in its own core domain and may only use the activity manager."""
    domain = RECLAIM_SERVICE[0]
    if file_context_label(system_ext_file_contexts, RECLAIM_SERVICE[1]) != (
        f"u:object_r:{domain}_exec:s0"
    ):
        fail(f"system_ext file contexts do not label {RECLAIM_SERVICE[1]}")
    if not RECLAIM_TRANSITION.search(system_ext_policy):
        fail("system_ext policy does not move init into the reclaim domain")

    def has(source: str, target: str, object_class: bytes, permission: bytes) -> bool:
        return any(
            CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) == source
            and CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode()) == target
            and match.group(3) == object_class
            and permission in match.group(4).split()
            for match in CIL_ALLOW_RULE.finditer(system_ext_policy)
        )

    for source, target, object_class, permission, why in (
        (domain, "activity_service", b"service_manager", b"find",
         "dumpsys and cmd find the activity manager"),
        (domain, "system_server", b"binder", b"transfer",
         "cmd hands the activity manager the receiver of its result"),
        ("system_server", domain, b"binder", b"call",
         "cmd waits for the receiver of its result"),
        ("system_server", domain, b"fd", b"use",
         "the activity manager writes to the descriptors the tools pass"),
        ("system_server", domain, b"fifo_file", b"write",
         "dumpsys reads the process list from a pipe"),
    ):
        if not has(source, target, object_class, permission):
            fail(
                f"system_ext policy does not allow {source} {target} {object_class.decode()} "
                f"{permission.decode()}: {why}"
            )


def check_time_zone(rc_payload: str, script: bytes) -> None:
    """The script that applies the host's time zone runs once after boot and never writes a setting."""
    name, executable = TIME_ZONE_SERVICE
    service = init_services(rc_payload).get(name)
    if service is None or service[0] != executable:
        fail(f"system_ext init policy does not declare the {name} service at {executable}")
    for option in ("oneshot", "disabled"):
        if option not in service[1]:
            fail(f"system_ext init policy does not declare {name} as {option}")
    actions = init_actions(rc_payload)
    for trigger, commands in actions.items():
        if f"start {name}" in commands and trigger != TIME_ZONE_TRIGGER:
            fail(f"system_ext init policy starts {name} under '{trigger}'")
    if f"start {name}" not in actions.get(TIME_ZONE_TRIGGER, []):
        fail(f"system_ext init policy does not start {name} when boot completes")
    code = "\n".join(
        line
        for line in script.decode("utf-8", errors="strict").splitlines()
        if not line.lstrip().startswith("#")
    )
    for token in TIME_ZONE_SCRIPT_REQUIRED:
        if token not in code:
            fail(f"{executable} does not contain '{token}'")
    for token in TIME_ZONE_SCRIPT_FORBIDDEN:
        if token in code:
            fail(
                f"{executable} contains '{token}': it may only read the user's automatic time "
                "zone setting and set the zone through the platform's setter"
            )


def check_time_zone_policy(
    system_ext_policy: bytes, system_ext_file_contexts: bytes, system_ext_property_contexts: str
) -> None:
    """The script runs in its own core domain, uses only the settings and alarm services, sets only its marker."""
    domain = TIME_ZONE_SERVICE[0]
    if file_context_label(system_ext_file_contexts, TIME_ZONE_SERVICE[1]) != (
        f"u:object_r:{domain}_exec:s0"
    ):
        fail(f"system_ext file contexts do not label {TIME_ZONE_SERVICE[1]}")
    if not TIME_ZONE_TRANSITION.search(system_ext_policy):
        fail("system_ext policy does not move init into the time zone domain")

    def has(source: str, target: str, object_class: bytes, permission: bytes) -> bool:
        return any(
            CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) == source
            and CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode()) == target
            and match.group(3) == object_class
            and permission in match.group(4).split()
            for match in CIL_ALLOW_RULE.finditer(system_ext_policy)
        )

    for source, target, object_class, permission, why in (
        (domain, "settings_service", b"service_manager", b"find",
         "cmd finds the settings service to read the automatic time zone setting"),
        (domain, "alarm_service", b"service_manager", b"find",
         "cmd finds the alarm service, the platform's time zone setter"),
        (domain, "system_server", b"binder", b"transfer",
         "cmd hands both services the receiver of its result"),
        ("system_server", domain, b"binder", b"call",
         "cmd waits for the receiver of its result"),
        ("system_server", domain, b"fd", b"use",
         "the services write to the descriptors cmd passes"),
        ("system_server", domain, b"fifo_file", b"write",
         "the settings service writes the setting it reads to a pipe"),
    ):
        if not has(source, target, object_class, permission):
            fail(
                f"system_ext policy does not allow {source} {target} {object_class.decode()} "
                f"{permission.decode()}: {why}"
            )
    if TIME_ZONE_MARKER_TYPE not in cil_attribute_members(system_ext_policy, "system_property_type"):
        fail(f"system_ext policy does not define the system-owned {TIME_ZONE_MARKER_TYPE}")
    settable = {
        CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode())
        for match in CIL_PROPERTY_SET_RULE.finditer(system_ext_policy)
        if CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) == domain
        and b"set" in match.group(3).split()
    }
    if TIME_ZONE_MARKER_TYPE not in settable:
        fail(f"system_ext policy does not let the time zone domain set {TIME_ZONE_MARKER}")
    if settable != {TIME_ZONE_MARKER_TYPE}:
        fail(
            f"system_ext policy lets the time zone domain set {sorted(settable)}, not only its "
            f"own marker {TIME_ZONE_MARKER_TYPE}: system_server writes persist.sys.timezone"
        )
    if not any(
        line.split()[:2] == [TIME_ZONE_MARKER, f"u:object_r:{TIME_ZONE_MARKER_TYPE}:s0"]
        for line in system_ext_property_contexts.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ):
        fail(f"system_ext property contexts do not label {TIME_ZONE_MARKER}")


def check_policy_relaxations(policies: tuple[bytes, ...], build_variant: str) -> None:
    """The policy holds none of the LineageOS grants that Android's neverallow tests refuse.

    LineageOS serves an ext4 SD card and addon.d updates with rules that the
    platform build tolerates only because its fork relaxed the matching
    neverallows; the CTS carries AOSP's. This product has neither feature, and
    its tree restores the neverallows (the system/sepolicy route), so the rules
    are gone. This repeats the checks on the built policy.
    """
    mls_trusted = set()
    for policy in policies:
        mls_trusted |= cil_attribute_members(policy, "mlstrustedsubject")
    if "mediaprovider_app" in mls_trusted:
        fail(
            "policy makes mediaprovider_app an mlstrustedsubject; Android's neverallow "
            "tests refuse it to fork and to modify privapp_data_file"
        )
    groups: dict[str, set[str]] = {}
    for policy in policies:
        for match in CIL_ALLOW_RULE.finditer(policy):
            source, target, object_class, permissions = (
                CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(index).decode())
                for index in (1, 2, 3, 4)
            )
            perms = set(permissions.split())
            if source == "mediaprovider_app" and target == "privapp_data_file":
                fail("policy lets mediaprovider_app use privapp_data_file beyond the app rules")
            if (
                source == "fsck_untrusted"
                and object_class in ("capability", "cap_userns")
                and "sys_admin" in perms
            ):
                fail("policy gives fsck_untrusted sys_admin; Android's neverallow tests refuse it")
            if object_class == "filesystem" and "relabelto" in perms:
                if target not in groups:
                    groups[target] = cil_types_and_attributes(policies, target)
                if "fs_type" in groups[target] and "contextmount_type" not in groups[target]:
                    fail(
                        f"policy lets {source} relabel a filesystem to {target}; only a "
                        "contextmount_type may be a relabelto target"
                    )
        for match in CIL_PERMISSIVE_TYPE.finditer(policy):
            domain = CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode())
            if domain not in PERMISSIVE_DOMAINS[build_variant]:
                fail(
                    f"policy makes the domain {domain} permissive; in a {build_variant} build "
                    f"only {sorted(PERMISSIVE_DOMAINS[build_variant]) or 'no domain'} may be"
                )


def check_mediaserver_property_audit(policies: tuple[bytes, ...]) -> None:
    """mediaserver's read of a vendor property is silenced as it is for the other graphics clients.

    The vendor's gralloc mapper, which mediaserver loads to allocate decoder
    buffers, reads vendor.minigbm.debug when it initialises. That name is a
    vendor_default_prop, which a core domain may not read, and the lookup
    carries on without a value. Without this rule every first use logs an
    audit denial that nothing needs to act on.
    """
    for policy in policies:
        for match in CIL_DONTAUDIT_RULE.finditer(policy):
            source, target, object_class, permissions = (
                CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(index).decode())
                for index in (1, 2, 3, 4)
            )
            if (
                source == "mediaserver"
                and target == "vendor_default_prop"
                and object_class == "file"
                and "read" in permissions.split()
            ):
                return
    fail(
        "policy does not silence the denial of mediaserver reading vendor_default_prop "
        "(overlay/sepolicy/system_ext/private/mediaserver.te)"
    )


def property_label(name: str, contexts: list[str]) -> str | None:
    """The SELinux type that property_contexts give a property: an exact entry, else the longest prefix."""
    best: tuple[int, str] | None = None
    for line in contexts:
        fields = line.split()
        if not fields or fields[0].startswith("#") or len(fields) < 2:
            continue
        label = fields[1].split(":")[2] if fields[1].count(":") >= 3 else None
        if label is None:
            continue
        if "exact" in fields[2:]:
            if fields[0] == name:
                return label
        elif name.startswith(fields[0]) and (best is None or len(fields[0]) > best[0]):
            best = (len(fields[0]), label)
    return best[1] if best else None


@functools.lru_cache(maxsize=8)
def cil_attribute_sets(
    policy: bytes,
) -> tuple[tuple[tuple[str, frozenset[str]], ...], tuple[tuple[str, list[object]], ...]]:
    """The attributes a policy defines, without version suffixes: those whose set is a list of
    names, with the names, and those whose set is an expression, as nested lists of names."""
    lists = []
    expressions = []
    for match in CIL_TYPE_ATTRIBUTE_SET.finditer(policy):
        stack: list[list[object]] = [[]]
        for token in re.findall(r"[()]|[^\s()]+", match.group(2).decode()):
            if token == "(":
                stack.append([])
            elif token != ")":
                stack[-1].append(CIL_PLATFORM_VERSION_SUFFIX.sub("", token))
            elif len(stack) > 1:
                closed = stack.pop()
                stack[-1].append(closed)
        if len(stack) != 1 or len(stack[0]) != 1 or not isinstance(stack[0][0], list):
            continue
        attribute = CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode())
        members = stack[0][0]
        if all(isinstance(member, str) for member in members) and not (
            members and members[0] in CIL_SET_OPERATORS
        ):
            lists.append((attribute, frozenset(members)))
        else:
            expressions.append((attribute, members))
    return tuple(lists), tuple(expressions)


def cil_set_contains(members: object, names: set[str]) -> bool:
    """Whether a CIL set includes a type whose own name and attributes are |names|."""
    if isinstance(members, str):
        return members in names
    assert isinstance(members, list)
    operator = members[0] if members and isinstance(members[0], str) else None
    if operator not in CIL_SET_OPERATORS:
        return any(cil_set_contains(member, names) for member in members)
    operands = [cil_set_contains(operand, names) for operand in members[1:]]
    if operator == "all":
        return True
    if operator == "not":
        return not any(operands)
    if operator == "and":
        return all(operands)
    if operator == "or":
        return any(operands)
    return operands.count(True) == 1


def cil_types_and_attributes(policies: tuple[bytes, ...], type_name: str) -> set[str]:
    """A type and every attribute that includes it, directly, through another attribute or
    through a set expression.

    Lists of names are closed first; set expressions, which may negate, are then evaluated
    against that closure, and the lists that name the attributes they add are closed again.
    """
    lists = [entry for policy in policies for entry in cil_attribute_sets(policy)[0]]
    expressions = [entry for policy in policies for entry in cil_attribute_sets(policy)[1]]

    def close(names: set[str]) -> None:
        changed = True
        while changed:
            changed = False
            for attribute, members in lists:
                if attribute not in names and not members.isdisjoint(names):
                    names.add(attribute)
                    changed = True

    names = {type_name}
    close(names)
    names |= {
        attribute for attribute, members in expressions if cil_set_contains(members, names)
    }
    close(names)
    return names


def cil_allows_file_read(policies: tuple[bytes, ...], domain: str, label: str) -> bool:
    """Whether a domain may read the properties of a label, directly or through attributes."""
    readers = cil_types_and_attributes(policies, domain)
    readable = cil_types_and_attributes(policies, label)
    return any(
        CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) in readers
        and CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode()) in readable
        and b"read" in match.group(3).split()
        for policy in policies
        for match in CIL_FILE_ALLOW_RULE.finditer(policy)
    )


def check_adbd_launch_policy(contexts: list[str], policies: tuple[bytes, ...]) -> None:
    """adbd may read the launch properties and the debugging setting it acts on."""
    for name in ADBD_LAUNCH_PROPERTIES + ADBD_DEBUGGING_PROPERTIES:
        label = property_label(name, contexts) or "default_prop"
        if not cil_allows_file_read(policies, "adbd", label):
            fail(f"adbd may not read {name}, labelled {label}: it would ignore it")


def check_vendor_init_triggers(
    actions: dict[str, list[str]], contexts: list[str], policies: tuple[bytes, ...]
) -> None:
    """Every property that a product init trigger names is readable by vendor_init.

    Init parses a vendor script in the vendor_init subcontext and drops a trigger
    whose property that domain may not read ("unexported property trigger"), so a
    service that only such a trigger starts never runs. The platform's
    neverallow keeps vendor_init off system_internal_prop types.
    """
    for trigger in actions:
        for name in INIT_PROPERTY_TRIGGER.findall(trigger):
            label = property_label(name, contexts) or "default_prop"
            if not cil_allows_file_read(policies, "vendor_init", label):
                fail(
                    f"the trigger 'on {trigger}' names {name}, labelled {label}, which "
                    "vendor_init may not read; init drops the trigger"
                )


def cil_allows_property_set(policies: tuple[bytes, ...], domain: str, label: str) -> bool:
    """Whether a domain may set the properties of a label, directly or through attributes."""
    setters = cil_types_and_attributes(policies, domain)
    settable = cil_types_and_attributes(policies, label)
    return any(
        CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) in setters
        and CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode()) in settable
        and b"set" in match.group(3).split()
        for policy in policies
        for match in CIL_PROPERTY_SET_RULE.finditer(policy)
    )


def check_venus_option_policy(contexts: list[str], policies: tuple[bytes, ...]) -> None:
    """vendor_init may set the Venus driver option, and every domain that loads the driver may read it."""
    label = property_label(VENUS_PERF_PROPERTY, contexts) or "default_prop"
    if not cil_allows_property_set(policies, "vendor_init", label):
        fail(
            f"vendor_init may not set {VENUS_PERF_PROPERTY}, labelled {label}: Venus would "
            "allocate asynchronously"
        )
    for domain in VENUS_DRIVER_DOMAINS:
        if not cil_allows_file_read(policies, domain, label):
            fail(
                f"{domain} may not read {VENUS_PERF_PROPERTY}, labelled {label}: Venus would "
                "allocate asynchronously there"
            )


# lmkd's GPU-aware reclaim (route p0027): while the virtio-gpu driver reports host graphics memory
# pressure in this sysfs file, lmkd ends the cached app that holds the most of it. The product turns
# it on with a property; the binary, the property's label and lmkd's access to the file's type are
# what make that work, and none of them fails loudly when it is missing.
LMKD_BINARY = "SYSTEM/bin/lmkd"
LMKD_GPU_RECLAIM_PROPERTY = "ro.lmk.gpu_reclaim"
LMKD_GPU_REPORT_PATH = "/sys/class/drm/card0/gpu_memory_report"
LMKD_GPU_REPORT_GENFS_PATH = "/devices/virtio-uml-cmdline/virtio-uml.0/drm/card0/gpu_memory_report"
LMKD_GPU_REPORT_TYPE = "sysfs_gpu_memory"
# The directories on the way to the file carry the virtio-gpu device's own type.
LMKD_GPU_DEVICE_TYPE = "sysfs_gpu"
# What only the p0027 topic puts in lmkd.
LMKD_GPU_MARKERS = (
    LMKD_GPU_RECLAIM_PROPERTY.encode(),
    LMKD_GPU_REPORT_PATH.encode(),
    b"graphics memory pressure",
)
CIL_GENFS_RULE = re.compile(
    rb'^\(genfscon sysfs "([^"]*)" (?:\w+ )?\(\S+ \S+ (\S+) ', re.MULTILINE
)


def check_lmkd_gpu_reclaim(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """lmkd holds the GPU reclaim of route p0027, and the product turns it on."""
    info = infos.get(LMKD_BINARY)
    if info is None or info.is_dir():
        fail(f"{LMKD_BINARY} is missing")
    payload = read_small(archive, info, 64 << 20)
    for marker in LMKD_GPU_MARKERS:
        if marker not in payload:
            fail(
                f"{LMKD_BINARY} lacks {marker.decode()!r}: the p0027 topic is missing from the "
                "build, so lmkd would never end the app that holds the host's graphics memory"
            )
    values = [
        line.partition("=")[2].strip()
        for line in read_small(archive, infos["PRODUCT/etc/build.prop"])
        .decode("utf-8", errors="strict")
        .splitlines()
        if line.partition("=")[0] == LMKD_GPU_RECLAIM_PROPERTY
    ]
    if values != ["true"]:
        fail(f"unexpected product property {LMKD_GPU_RECLAIM_PROPERTY}: {values}")


def cil_allows(
    policies: tuple[bytes, ...], domain: str, label: str, object_class: str, permissions: set[str]
) -> bool:
    """Whether a domain holds every one of the permissions of an object class on a label, summed
    over the allow rules that reach it directly or through attributes."""
    subjects = cil_types_and_attributes(policies, domain)
    objects = cil_types_and_attributes(policies, label)
    granted: set[str] = set()
    for policy in policies:
        for match in CIL_ALLOW_RULE.finditer(policy):
            if (
                match.group(3).decode() == object_class
                and CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) in subjects
                and CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode()) in objects
            ):
                granted.update(permission.decode() for permission in match.group(4).split())
    return permissions <= granted


def check_lmkd_gpu_policy(
    contexts: list[str], policies: tuple[bytes, ...], vendor_policy: bytes
) -> None:
    """lmkd may read the setting, the report file has its own sysfs type that the vendor's
    genfs_contexts gives it, and lmkd may walk to the file and read and write it."""
    label = property_label(LMKD_GPU_RECLAIM_PROPERTY, contexts) or "default_prop"
    if not cil_allows_file_read(policies, "lmkd", label):
        fail(
            f"lmkd may not read {LMKD_GPU_RECLAIM_PROPERTY}, labelled {label}: it would keep the "
            "GPU reclaim off"
        )
    if "sysfs_type" not in cil_types_and_attributes(policies, LMKD_GPU_REPORT_TYPE):
        fail(
            f"{LMKD_GPU_REPORT_TYPE} is not a sysfs type of the policy: the report file cannot "
            "be labelled"
        )
    genfs = {
        match.group(1).decode(): CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode())
        for match in CIL_GENFS_RULE.finditer(vendor_policy)
    }
    if genfs.get(LMKD_GPU_REPORT_GENFS_PATH) != LMKD_GPU_REPORT_TYPE:
        fail(
            f"the vendor policy does not label {LMKD_GPU_REPORT_GENFS_PATH} {LMKD_GPU_REPORT_TYPE} "
            f"(it has {genfs.get(LMKD_GPU_REPORT_GENFS_PATH)}): lmkd could not open the report"
        )
    if not cil_allows(policies, "lmkd", LMKD_GPU_REPORT_TYPE, "file", {"open", "read", "write"}):
        fail(f"lmkd may not open, read and write {LMKD_GPU_REPORT_TYPE}: the GPU reclaim stays off")
    if not cil_allows(policies, "lmkd", LMKD_GPU_DEVICE_TYPE, "dir", {"search"}):
        fail(
            f"lmkd may not search {LMKD_GPU_DEVICE_TYPE} directories: it could not reach the "
            "report file under the virtio-gpu device"
        )


# The virtio-gpu device paces the display with a vblank timer, so the present fences of
# drm_hwcomposer are reliable. The inherited Cuttlefish property marks them unreliable, which
# keeps SurfaceFlinger from taking present timestamps from them (service.sf.present_timestamp)
# and the Vulkan loader from exposing VK_GOOGLE_display_timing.
PRESENT_FENCE_PROPERTY = "ro.vendor.hwc.drm.present_fence_not_reliable"
FALSE_PROPERTY_VALUES = {"0", "n", "no", "off", "false"}
# The inherited Cuttlefish configuration adds ro.adb.secure=0 to the product partition of a user
# build, and the product build.prop is read after system's and system_ext's ro.adb.secure=1, so
# the guest reads 0. adbd takes its policy from ro.boot.kernarium.adb_auth, but Settings hides
# "Revoke USB debugging authorizations" when the property is false: the user could no longer
# revoke the computers they allowed. The Cuttlefish topic (route p0001) drops the entry when the
# product sets UML_ANDROID_KEEP_ADB_SECURE.
ADB_SECURE_PROPERTY = "ro.adb.secure"
# Incremental installs (adb install --incremental, the Incremental FS service) need this
# vendor property and the kernel's incfs.
INCREMENTAL_PROPERTY = "ro.incremental.enable"


def parse_properties(payload: str) -> dict[str, str]:
    properties: dict[str, str] = {}
    for line in payload.splitlines():
        key, separator, value = line.partition("=")
        if separator and not key.startswith("#"):
            properties[key] = value.strip()
    return properties


def check_property_file(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> None:
    if info.file_size > 1 << 20:
        fail(f"property file is unexpectedly large: {info.filename}")
    payload = archive.read(info).decode("utf-8", errors="strict")
    for key, value in parse_properties(payload).items():
        if key.startswith("ro.virtual_ab."):
            fail(f"Virtual A/B property is forbidden in {info.filename}: {key}")
        if key == "ro.build.ab_update" and value == "true":
            fail(f"A/B update property is enabled in {info.filename}")
        abi_property = ABI_PROPERTY.fullmatch(key)
        if abi_property is not None:
            expected = EXPECTED_ABIS[abi_property.group(1)]
            if value != expected:
                fail(f"unexpected ABI in {info.filename}: {key}={value!r}, not {expected!r}")
        if key == "ro.zygote" and value != VENDOR_MULTILIB_PROPERTIES["ro.zygote"]:
            fail(f"unexpected zygote in {info.filename}: {value}")
        if key == OVERLAY_THEME_PROPERTY:
            fail(
                f"{info.filename} sets {key}={value}: a vendor overlay theme enables overlays at "
                "boot, and the navigation mode overlays are the platform's to choose"
            )
        if key == "persist.sys.usb.config":
            fail(
                f"{info.filename} sets {key}={value}: USB debugging is off on a fresh data "
                "partition, and AdbService records the user's choice there"
            )
        if key == ADB_SECURE_PROPERTY and value.lower() in FALSE_PROPERTY_VALUES:
            fail(
                f"{info.filename} sets {key}={value}: adb authorization is the stock one, and "
                "Settings hides the revocation of allowed computers when the property is false; "
                "the Cuttlefish topic (route p0001) drops the inherited Cuttlefish value at its source"
            )
        if key == PRESENT_FENCE_PROPERTY and value.lower() not in FALSE_PROPERTY_VALUES:
            fail(
                f"{info.filename} marks the present fences unreliable: {key}={value}; "
                "the virtio-gpu display is paced by a vblank timer"
            )


def check_multilib_properties(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The partitions state both ABIs, and the vendor describes the second."""
    for name, partition in ABI_LIST_PROPERTY_FILES.items():
        properties = parse_properties(
            read_small(archive, infos[name]).decode("utf-8", errors="strict")
        )
        for suffix in ("abilist", "abilist32", "abilist64"):
            key = f"ro.{partition}.product.cpu.{suffix}"
            if properties.get(key) != EXPECTED_ABIS[suffix]:
                fail(f"{name} lacks {key}={EXPECTED_ABIS[suffix]}: {properties.get(key)!r}")
    vendor = parse_properties(
        read_small(archive, infos["VENDOR/build.prop"]).decode("utf-8", errors="strict")
    )
    for key, expected in VENDOR_MULTILIB_PROPERTIES.items():
        if vendor.get(key) != expected:
            fail(f"VENDOR/build.prop lacks {key}={expected}: {vendor.get(key)!r}")
    if vendor.get(INCREMENTAL_PROPERTY) != "1":
        fail(f"VENDOR/build.prop lacks {INCREMENTAL_PROPERTY}=1: {vendor.get(INCREMENTAL_PROPERTY)!r}")
    system = parse_properties(
        read_small(archive, infos["SYSTEM/build.prop"]).decode("utf-8", errors="strict")
    )
    if system.get("ro.product.cpu.abi") != EXPECTED_ABIS["abi"]:
        fail(f"SYSTEM/build.prop lacks ro.product.cpu.abi=arm64-v8a: {system.get('ro.product.cpu.abi')!r}")


def check_erofs_policy(
    archive: zipfile.ZipFile,
    infos: dict[str, zipfile.ZipInfo],
    misc_info: dict[str, str],
) -> None:
    """The images were built with the product's one compressor and cluster size."""
    compressor, pcluster_size = erofs_parameters()
    if misc_info.get("erofs_default_compressor") != compressor:
        fail(
            f"erofs_default_compressor must be {compressor}: "
            f"{misc_info.get('erofs_default_compressor')!r}"
        )
    if misc_info.get("erofs_pcluster_size") != pcluster_size:
        fail(
            f"erofs_pcluster_size must be {pcluster_size}: "
            f"{misc_info.get('erofs_pcluster_size')!r}"
        )
    for key in sorted(misc_info):
        if key in EROFS_FORBIDDEN_POLICY or EROFS_PARTITION_POLICY.match(key):
            fail(f"EROFS policy beyond the product's two parameters is set: {key}")
    hints = infos.get(EROFS_COMPRESS_HINTS_ENTRY)
    if hints is None:
        fail(f"{EROFS_COMPRESS_HINTS_ENTRY} is missing: the .apex files would be compressed")
    tracked_hints = (android_image.ANDROID_ROOT / "overlay/config/erofs_compress_hints.txt").read_bytes()
    if read_small(archive, hints) != tracked_hints:
        fail("EROFS compress hints differ from the tracked product file")
    if misc_info.get("erofs_default_compress_hints") is None:
        fail("erofs_default_compress_hints is not set")
    for partition in READ_ONLY_PARTITIONS:
        name = f"IMAGES/{partition}.img"
        with archive.open(infos[name]) as source:
            prefix = android_image.read_image_prefix(
                source,
                android_image.EROFS_SUPERBLOCK_OFFSET + android_image.EROFS_SUPERBLOCK_SIZE,
                error,
            )
        check_erofs_superblock(name, prefix)


@functools.cache
def erofs_parameters() -> tuple[str, str]:
    return android_image.tracked_erofs_parameters(error)


def check_erofs_superblock(name: str, prefix: bytes) -> None:
    """The image is EROFS on 4 KiB blocks, compressed with the product's algorithm alone and
    with clusters larger than a block, and uses none of the extended layout options."""
    compressor = erofs_parameters()[0]
    superblock = android_image.erofs_superblock(prefix, name, error)
    if superblock.block_size_bits != 12:
        fail(f"{name} does not use 4 KiB blocks")
    if superblock.compression_algorithms != 1 << android_image.EROFS_ALGORITHM_ZSTD:
        fail(
            f"{name} is not compressed with {compressor.partition(',')[0]} alone: "
            f"algorithms {superblock.compression_algorithms:#x}"
        )
    if not superblock.feature_incompat & android_image.EROFS_FEATURE_INCOMPAT_BIG_PCLUSTER:
        fail(f"{name} has no physical cluster larger than a block")
    if superblock.feature_incompat & (
        android_image.EROFS_FEATURE_INCOMPAT_ZTAILPACKING
        | android_image.EROFS_FEATURE_INCOMPAT_FRAGMENTS
    ):
        fail(f"{name} uses an extended option (ztailpacking, fragments or dedupe)")


def check_userdata_quota(fstab: bytes, label: str) -> None:
    """Every userdata line carries the fs_mgr `quota` flag.

    installd answers StorageStatsManager only from a /data device whose user quota
    is on, and fs_mgr's tune2fs step enables it only for an entry with the flag.
    """
    lines = [
        fields
        for fields in (line.split() for line in fstab.decode("utf-8", errors="strict").splitlines())
        if fields and not fields[0].startswith("#") and fields[0] == "/dev/block/by-name/userdata"
    ]
    if not lines:
        fail(f"UML fstab has no userdata entry: {label}")
    for fields in lines:
        flags = fields[4].split(",") if len(fields) > 4 else []
        if "quota" not in flags:
            fail(f"UML fstab userdata entry lacks the fs_mgr quota flag: {label}")


def check_video_encoders(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The codec list names the hardware encoders and decoders, and only they are enabled."""
    try:
        codecs = ET.fromstring(read_small(archive, infos["VENDOR/etc/media_codecs_c2.xml"]))
    except ET.ParseError as error:
        fail(f"VENDOR/etc/media_codecs_c2.xml is not valid XML: {error}")
    encoders = {
        codec.get("name"): codec.get("type")
        for codec in codecs.iterfind("./Encoders/MediaCodec")
    }
    if encoders != VIDEO_ENCODER_CODECS:
        fail(f"the Codec2 list must name exactly the hardware encoders: {encoders}")
    for codec in codecs.iterfind("./Encoders/MediaCodec"):
        limits = {limit.get("name"): limit for limit in codec.iterfind("./Limit")}
        size = limits.get("size")
        if size is None or size.get("min") != VIDEO_ENCODER_MINIMUM_SIZE:
            fail(f"{codec.get('name')} must start at {VIDEO_ENCODER_MINIMUM_SIZE} pixels: "
                 "the host lists no smaller picture")
        bitrate = limits.get("bitrate")
        if bitrate is None or (bitrate.get("range") or "").split("-")[0] != VIDEO_ENCODER_MINIMUM_BITRATE:
            fail(f"{codec.get('name')} must start at {VIDEO_ENCODER_MINIMUM_BITRATE} bit/s: "
                 "the host lists no lower bit rate")
    # A decoder is one of the hardware decoders, or an update to a software decoder's limits.
    hardware_decoders = {
        codec.get("name"): codec.get("type")
        for codec in codecs.iterfind("./Decoders/MediaCodec")
        if (codec.get("name") or "").startswith("c2.v4l2.")
    }
    if hardware_decoders != VIDEO_DECODER_CODECS:
        fail(f"the Codec2 list must name the hardware decoders: {hardware_decoders}")
    decoders = list(codecs.iterfind("./Decoders/MediaCodec"))
    if any(
        (codec.get("name") or "") not in VIDEO_DECODER_CODECS
        and (
            not (codec.get("name") or "").startswith("c2.android.") or codec.get("update") != "true"
        )
        for codec in decoders
    ) or len(list(codecs.iter("MediaCodec"))) != len(encoders) + len(decoders):
        fail("the Codec2 list names a codec outside the hardware codecs and the software updates")
    for codec in decoders:
        if codec.get("name") not in VIDEO_DECODER_CODECS:
            continue
        # CTS wants a decoder's width and height ranges to match, and the adaptive playback and
        # dynamic colour aspects features it implements to be published.
        limits = {limit.get("name"): limit for limit in codec.iterfind("./Limit")}
        size = limits.get("size")
        if size is None or "x" not in (size.get("min") or "") or "x" not in (size.get("max") or ""):
            fail(f"{codec.get('name')} publishes no size limit")
        features = {feature.get("name") for feature in codec.iterfind("./Feature")}
        for feature in ("adaptive-playback", "dynamic-color-aspects"):
            if feature not in features:
                fail(f"{codec.get('name')} does not publish {feature}")
    # CTS requires a hardware codec to publish what it sustains: the performance points that
    # the claims test reads and the measured frame rates that the performance tests check
    # against what they measure.
    hardware_codecs = list(codecs.iterfind("./Encoders/MediaCodec")) + [
        codec for codec in decoders if codec.get("name") in VIDEO_DECODER_CODECS
    ]
    for codec in hardware_codecs:
        limits = [limit.get("name", "") for limit in codec.iterfind("./Limit")]
        for prefix in ("performance-point-", "measured-frame-rate-"):
            if not any(name.startswith(prefix) for name in limits):
                fail(f"{codec.get('name')} publishes no {prefix}* limit: "
                     "a hardware codec states its performance")

    vendor = parse_properties(
        read_small(archive, infos["VENDOR/build.prop"]).decode("utf-8", errors="strict")
    )
    for key, expected in VIDEO_ENCODER_PROPERTIES.items():
        if vendor.get(key) != expected:
            fail(f"VENDOR/build.prop lacks {key}={expected}: {vendor.get(key)!r}")
    for key, expected in VIDEO_DECODER_PROPERTIES.items():
        if vendor.get(key) != expected:
            fail(f"VENDOR/build.prop lacks {key}={expected}: {vendor.get(key)!r}")
    # A codec cannot publish more than the device sustains: the service reports the pixel rate
    # as the capacity that real-time streams of its direction are counted against, and the
    # largest point must use a good share of it.
    for direction, group, key in (
        ("pixel rate", list(codecs.iterfind("./Encoders/MediaCodec")),
         "ro.vendor.v4l2_codec2.video_pixel_rate"),
        ("decode pixel rate",
         [codec for codec in decoders if codec.get("name") in VIDEO_DECODER_CODECS],
         "ro.vendor.v4l2_codec2.video_decode_pixel_rate"),
    ):
        capacity = int(vendor[key])
        largest = 0
        for codec in group:
            for limit in codec.iterfind("./Limit"):
                name = limit.get("name", "")
                if not name.startswith("performance-point-"):
                    continue
                try:
                    width, height = (int(side) for side in name.rpartition("-")[2].split("x"))
                    frame_rate = int((limit.get("range") or "").partition("-")[2])
                except ValueError:
                    fail(f"{codec.get('name')} publishes a malformed {name}: "
                         f"{limit.get('range')!r}")
                if width * height * frame_rate > capacity:
                    fail(f"{codec.get('name')} publishes {name} above the {direction} capacity "
                         f"{capacity}: {width * height * frame_rate}")
                largest = max(largest, width * height * frame_rate)
        if largest < capacity * VIDEO_MIN_CAPACITY_SHARE:
            fail(f"the largest performance point of the {direction} group uses {largest}, "
                 f"below {VIDEO_MIN_CAPACITY_SHARE:.0%} of the capacity {capacity}")
    for key, value in vendor.items():
        if (
            key.startswith(VIDEO_DECODER_PROPERTY_PREFIX)
            and key not in VIDEO_DECODER_PROPERTIES
            and value != "false"
        ):
            fail(f"VENDOR/build.prop enables a hardware decoder outside the approved set: {key}")

    ueventd = {
        line.strip()
        for line in read_small(archive, infos["VENDOR/etc/ueventd.rc"])
        .decode("utf-8", errors="strict")
        .splitlines()
    }
    if VIDEO_ENCODER_UEVENTD not in ueventd:
        fail(f"UML ueventd policy lacks the video nodes' group: {VIDEO_ENCODER_UEVENTD}")

    service_rc = read_small(archive, infos[f"VENDOR/etc/init/{VIDEO_ENCODER_SERVICE}.rc"])
    groups = {
        group
        for line in service_rc.decode("utf-8", errors="strict").splitlines()
        if line.split()[:1] == ["group"]
        for group in line.split()[1:]
    }
    if "camera" not in groups:
        fail("the video encoder service does not join the group that owns the video nodes")

    policy = read_small(
        archive, infos[f"VENDOR/etc/seccomp_policy/{VIDEO_ENCODER_SECCOMP_EXTENDED}"]
    ).decode("utf-8", errors="strict")
    allowed = {
        line.partition(":")[0].strip()
        for line in policy.splitlines()
        if not line.lstrip().startswith("#")
    }
    for call in VIDEO_ENCODER_SECCOMP_CALLS + VIDEO_CRASH_HANDLER_SECCOMP_CALLS:
        if call not in allowed:
            fail(f"the video codec seccomp policy does not allow {call}")

    contexts = read_small(archive, infos["VENDOR/etc/selinux/vendor_file_contexts"]).decode(
        "utf-8", errors="strict"
    )
    if not any(
        "c2-service-v4l2" in line and line.split()[-1:] == ["u:object_r:mediacodec_exec:s0"]
        for line in contexts.splitlines()
        if not line.lstrip().startswith("#")
    ):
        fail("the video encoder service is not labeled mediacodec_exec")


# The primary audio HAL (alsa_utils) sizes its ALSA streams from these vendor properties: four
# periods of one HAL buffer each. Without them it asks for periods of a quarter buffer, shorter
# than the Virtio sound driver's minimum, and the primary output does not open.
AUDIO_RING_PROPERTIES = {
    "ro.hardware.audio.tinyalsa.period_count": "4",
    "ro.hardware.audio.tinyalsa.period_size_multiplier": "4",
}
AUDIO_HAL_DOMAIN = "hal_audio_default"


def check_audio_ring_policy(contexts: list[str], policies: tuple[bytes, ...]) -> None:
    """init may load the ALSA ring properties from the vendor build.prop, and the audio HAL may
    read them."""
    for name in AUDIO_RING_PROPERTIES:
        label = property_label(name, contexts) or "default_prop"
        if not cil_allows_property_set(policies, "vendor_init", label):
            fail(
                f"vendor_init may not set {name}, labelled {label}: init skips it in the "
                "vendor build.prop"
            )
        if not cil_allows_file_read(policies, AUDIO_HAL_DOMAIN, label):
            fail(
                f"{AUDIO_HAL_DOMAIN} may not read {name}, labelled {label}: the primary "
                "output would not open"
            )


def check_audio_policy(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    main_name = "VENDOR/etc/audio_policy_configuration.xml"
    primary_name = "VENDOR/etc/primary_audio_policy_configuration.xml"
    try:
        main_root = ET.fromstring(read_small(archive, infos[main_name]))
        primary_payload = read_small(archive, infos[primary_name])
        primary_root = ET.fromstring(primary_payload)
    except ET.ParseError as error:
        fail(f"cannot parse UML audio policy: {error}")

    includes = {
        element.attrib["href"]
        for element in main_root.iter()
        if "href" in element.attrib
    }
    expected_includes = {
        "audio_policy_volumes.xml",
        "default_volume_tables.xml",
        "primary_audio_policy_configuration.xml",
        "r_submix_audio_policy_configuration.xml",
    }
    if includes != expected_includes:
        fail("UML audio policy does not select only primary PCM and remote submix")

    def elements(name: str) -> list[ET.Element]:
        return [
            element
            for element in primary_root.iter()
            if element.tag.rsplit("}", 1)[-1] == name
        ]

    mix_ports = elements("mixPort")
    profiles = elements("profile")
    device_ports = elements("devicePort")
    routes = elements("route")
    if [port.attrib for port in mix_ports] != [
        {
            "name": "primary output",
            "role": "source",
            "flags": "AUDIO_OUTPUT_FLAG_PRIMARY",
        },
        {"name": "primary input", "role": "sink"},
    ]:
        fail("UML primary audio policy must expose one ordinary mixer output "
             "and one ordinary mixer input")
    if [profile.attrib for profile in profiles] != [
        {
            "name": "",
            "format": "AUDIO_FORMAT_PCM_16_BIT",
            "samplingRates": "48000",
            "channelMasks": "AUDIO_CHANNEL_OUT_STEREO",
        },
        {
            "name": "",
            "format": "AUDIO_FORMAT_PCM_16_BIT",
            "samplingRates": "48000",
            "channelMasks": "AUDIO_CHANNEL_IN_STEREO AUDIO_CHANNEL_IN_MONO",
        },
    ]:
        fail("UML primary audio profiles must match the 48 kHz Virtio playback "
             "and capture streams")
    if [port.attrib for port in device_ports] != [
        {
            "tagName": "Speaker",
            "type": "AUDIO_DEVICE_OUT_SPEAKER",
            "role": "sink",
        },
        {
            "tagName": "Built-In Mic",
            "type": "AUDIO_DEVICE_IN_BUILTIN_MIC",
            "address": "bottom",
            "role": "source",
        },
    ]:
        fail("UML primary audio policy must expose only its speaker and built-in microphone")
    if [route.attrib for route in routes] != [
        {"type": "mix", "sink": "Speaker", "sources": "primary output"},
        {"type": "mix", "sink": "primary input", "sources": "Built-In Mic"},
    ]:
        fail("UML primary audio routes do not connect the mixer to the speaker "
             "and the built-in microphone to the input mixer")
    if [item.text for item in elements("item")] != ["Speaker", "Built-In Mic"]:
        fail("UML primary audio policy must attach only its speaker and built-in microphone")
    if set(re.findall(rb"AUDIO_DEVICE_IN_[A-Z0-9_]+", primary_payload)) != {
        b"AUDIO_DEVICE_IN_BUILTIN_MIC"
    }:
        fail("UML primary audio policy must name no input device but its built-in microphone")

    for forbidden in (
        b"AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD",
        b"AUDIO_OUTPUT_FLAG_DIRECT",
        b"AUDIO_OUTPUT_FLAG_MMAP_NOIRQ",
    ):
        if forbidden in primary_payload:
            fail(f"unsupported primary audio path remains enabled: {forbidden.decode()}")

    product_properties = read_small(
        archive, infos["PRODUCT/etc/build.prop"]
    ).decode("utf-8", errors="strict").splitlines()
    expected_properties = {
        "aaudio.mmap_policy": "1",
        "aaudio.mmap_exclusive_policy": "1",
        "ro.config.vc_call_vol_steps": "15",
        "ro.config.vc_call_vol_default": "15",
        "ro.config.media_vol_default": "15",
        "ro.config.alarm_vol_default": "7",
        "ro.config.system_vol_default": "7",
        "ro.config.assistant_vol_default": "15",
        # The guest boots without a lock screen, and `locksettings set-disabled` can turn
        # it on: LockSettingsService stores this default when it creates its database. The
        # Setup Wizard's screen lock page is hidden, so this is the lock a fresh guest keeps.
        "ro.lockscreen.disable.default": "true",
        # Userdata is a plain ext4 mount of its partition, which the mount holds exclusively:
        # apexd's pinned APEX images (a dm-linear table over that partition) fail with EBUSY,
        # so every staged APEX install is rejected unless apexd stages through /data/app-staging.
        "apexd.config.use_fiemap": "false",
    }
    for property_name, expected in expected_properties.items():
        values = [
            line.partition("=")[2].strip()
            for line in product_properties
            if line.partition("=")[0] == property_name
        ]
        if values != [expected]:
            fail(f"unexpected product property {property_name}: {values}")
    vendor_properties = read_small(
        archive, infos["VENDOR/build.prop"]
    ).decode("utf-8", errors="strict").splitlines()
    for property_name, expected in AUDIO_RING_PROPERTIES.items():
        values = [
            line.partition("=")[2].strip()
            for line in vendor_properties
            if line.partition("=")[0] == property_name
        ]
        if values != [expected]:
            fail(f"unexpected vendor audio property {property_name}: {values}")


def read_elf_prefix(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> bytes | None:
    """Return an ELF file's header and program-header table, or None for any
    other file."""
    if info.file_size < android_image.ELF32_HEADER_SIZE:
        return None
    with archive.open(info) as source:
        header = source.read(android_image.ELF64_HEADER_SIZE)
        if header[:4] != android_image.ELF_MAGIC:
            return None
        elf = android_image.elf_format(info.filename, header, error)
        program_offset, entry_size, entry_count = android_image.program_table(elf, header)
        program_end = program_offset + entry_size * entry_count
        if program_end > info.file_size or program_end > MAX_PROGRAM_HEADERS_SIZE:
            fail(f"malformed ELF program headers: {info.filename}")
        prefix = header
        if program_end > len(prefix):
            prefix += source.read(program_end - len(prefix))
        if len(prefix) < program_end:
            fail(f"truncated ELF program headers: {info.filename}")
        return prefix


def elf_directories(path: PurePosixPath) -> set[tuple[str, str]]:
    return set(zip(path.parts, path.parts[1:]))


def elf32_placement_ok(path: PurePosixPath) -> bool:
    """Whether a 32-bit ELF may sit at this target-files path.

    The ABI directories hold the libraries: lib in a partition, and lib/arm
    beside an application's own. ART writes its images and compiled code under
    arm directories. An executable built for both ABIs takes the stem suffix
    32, and the bootstrap linker is the one 32-bit executable without it.
    Anything else is a 32-bit daemon, which this product does not have.
    """
    parts = path.parts
    directories = elf_directories(path)
    if len(parts) > 1 and parts[1] == "lib":
        return True
    if ("lib", "arm") in directories or ("oat", "arm") in directories:
        return True
    if parts[1:3] == ("framework", "arm"):
        return True
    if len(parts) > 2 and parts[1] == "bin":
        return path.name.endswith("32") or str(path) in ELF32_BIN_EXCEPTIONS
    return False


def elf64_placement_ok(path: PurePosixPath) -> bool:
    """Whether a 64-bit ELF may sit at this target-files path: not where the
    32-bit libraries live."""
    return not (
        (len(path.parts) > 1 and path.parts[1] == "lib")
        or ("lib", "arm") in elf_directories(path)
    )


def check_target_elf(info: zipfile.ZipInfo, payload: bytes) -> None:
    elf = android_image.elf_format(info.filename, payload, error)
    if struct.unpack_from("<H", payload, 16)[0] not in (ET_EXEC, ET_DYN):
        return
    path = PurePosixPath(info.filename)
    if elf is android_image.ELF32:
        android_image.check_arm_elf_header(info.filename, payload, error)
        if not elf32_placement_ok(path):
            fail(f"32-bit ELF outside the 32-bit library and runtime paths: {info.filename}")
    else:
        android_image.check_aarch64_elf_header(info.filename, payload, error)
        if not elf64_placement_ok(path):
            fail(f"64-bit ELF in a 32-bit library directory: {info.filename}")
    android_image.check_load_alignment(info.filename, payload, error)


def check_large_file_imports(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo], entry: str
) -> None:
    """Refuse a 32-bit stack library that imports mmap or lseek without the 64
    suffix when the same library has a 64-bit build."""
    parts = PurePosixPath(entry).parts
    if parts[1] != "lib":
        return
    twin = str(PurePosixPath(parts[0], "lib64", *parts[2:]))
    if twin not in infos:
        return
    info = infos[entry]
    plain = sorted(
        set(LARGE_FILE_FUNCTIONS)
        & android_image.elf_imports(info.filename, archive.read(info), error)
    )
    if plain:
        fail(
            f"32-bit {entry} imports {', '.join(plain)} instead of "
            f"{', '.join(name + '64' for name in plain)}: it was built without "
            "-D_FILE_OFFSET_BITS=64"
        )


def check_mesa_gles_stack(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    for entry in sorted(MESA_ELF_ENTRIES | MESA_HAL_ENTRIES):
        info = infos[entry]
        payload = read_elf_prefix(archive, info)
        if payload is None:
            fail(f"Mesa component is not an ELF file: {entry}")
        elf_type, machine = struct.unpack_from("<HH", payload, 16)
        expected_machine = MESA_ABI_DIRECTORIES[PurePosixPath(entry).parts[1]]
        if elf_type != ET_DYN or machine != expected_machine:
            fail(
                "Mesa component is not "
                f"{'AArch64' if expected_machine == android_image.EM_AARCH64 else 'Arm'}"
                f" ET_DYN: {entry}"
            )
        check_target_elf(info, payload)
        check_large_file_imports(archive, infos, entry)
        if entry in MESA_EGL_ENTRIES:
            needed = android_image.elf_needed(info.filename, archive.read(info), error)
            if MESA_GALLIUM_DRIVER not in needed:
                fail(f"Mesa GLES component does not link the gallium driver: {entry}")

    for name in infos:
        if name in RETIRED_EGL_ENTRIES:
            fail(f"retired AOSP Mesa EGL or GLES library is forbidden: {name}")
        if name.startswith(RETIRED_DRI_DIRECTORIES):
            fail(f"retired DRI driver payload is forbidden: {name}")

    vendor_properties = {
        line.partition("=")[0].strip()
        for line in read_small(archive, infos["VENDOR/build.prop"])
        .decode("utf-8", errors="strict")
        .splitlines()
    }
    for name in sorted(BOOT_HAL_SELECTIONS):
        if f"ro.hardware.{name}" in vendor_properties:
            fail(
                f"VENDOR/build.prop sets ro.hardware.{name}, which the boot "
                "command line owns"
            )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target-files", type=Path, required=True)
    parser.add_argument("--kernel-elf", type=Path, required=True)
    parser.add_argument("--kernel-config", type=Path, required=True)
    parser.add_argument("--kernel-release-file", type=Path, required=True)
    parser.add_argument(
        "--build-variant",
        choices=sorted(BUILD_VARIANTS),
        help="build variant the target files must carry (default: tracked metadata)",
    )
    parser.add_argument(
        "--signed",
        action="store_true",
        help="the target files are the signed archive of a release: check that they trust the "
        "release certificate alone",
    )
    args = parser.parse_args()
    build_variant = args.build_variant or configured_build_variant(
        Path(__file__).resolve().parents[1]
    )

    kernel_config = args.kernel_config
    kernel_release_file = args.kernel_release_file

    if not args.target_files.is_file():
        fail(f"target-files archive does not exist: {args.target_files}")
    if not args.kernel_elf.is_file():
        fail(f"kernel ELF does not exist: {args.kernel_elf}")
    if not kernel_config.is_file():
        fail(f"kernel configuration does not exist: {kernel_config}")
    if not kernel_release_file.is_file():
        fail(f"kernel release file does not exist: {kernel_release_file}")

    seen: set[str] = set()
    infos: dict[str, zipfile.ZipInfo] = {}
    elf_count = 0
    apex_count = 0
    try:
        archive = zipfile.ZipFile(args.target_files)
    except (OSError, zipfile.BadZipFile) as error:
        fail(f"cannot open target-files archive: {error}")

    with archive:
        for info in archive.infolist():
            path = safe_name(info.filename.rstrip("/"))
            canonical = str(path)
            if canonical in seen:
                fail(f"duplicate archive entry: {canonical}")
            seen.add(canonical)
            infos[canonical] = info
            reject_module_payload(path)
            reject_nonstatic_payload(path)
            if info.is_dir():
                continue
            if len(path.parts) >= 3 and path.parts[1] == "apex":
                if path.suffix == ".capex":
                    fail(f"compressed APEX is forbidden: {path}")
                if path.suffix == ".apex":
                    check_apex_payload_filesystem(archive, info)
                    apex_count += 1
            if path.name in {"build.prop", "default.prop"}:
                check_property_file(archive, info)
            if path.parts[0] not in TARGET_PARTITIONS:
                continue
            payload = read_elf_prefix(archive, info)
            if payload is not None:
                elf_count += 1
                check_target_elf(info, payload)

        missing = sorted(REQUIRED_ENTRIES - seen)
        if missing:
            fail(f"required entries are missing: {', '.join(missing)}")
        if apex_count == 0:
            fail("target files do not contain an APEX payload")
        check_app_dexpreopt(seen)
        check_hardware_features(archive, infos)
        check_motion_sensors(archive, infos)
        check_discrete_ops(archive, infos)
        check_default_wallpaper(archive, infos)
        check_no_statistics_uploader(archive, infos)
        check_setup_wizard_branding(archive, infos)
        check_navigation_mode(archive, infos)
        check_easter_egg(infos)
        check_launcher_workspace(archive, infos)
        check_network_time(archive, infos)
        check_flip_to_screen_off(archive, infos)
        check_pinner_overlay(archive, infos)
        check_application_manifests(archive, infos, build_variant)
        check_thermal_hal(archive, infos)
        check_cas_hal(archive, infos)
        check_wifi_hals(archive, infos)
        check_adbd_launch_properties(archive, infos)
        check_adb_root_service(archive, infos)
        check_lmkd_gpu_reclaim(archive, infos)
        check_services_jar(archive, infos)
        check_audio_policy(archive, infos)
        check_multilib_properties(archive, infos)
        check_video_encoders(archive, infos)
        check_mesa_gles_stack(archive, infos)
        check_boot_policy(archive, infos, build_variant)
        check_kernel_policy(
            archive, infos, kernel_config, kernel_release_file, build_variant
        )
        check_ota_certificates(archive, infos, build_variant, args.signed)
        if sha256_member(archive, infos["BOOT/kernel"]) != sha256_path(args.kernel_elf):
            fail("BOOT/kernel does not match the verified UML ELF")

    print(
        f"verified AOSP 17 target files: {len(seen)} entries, "
        f"{elf_count} target executables, {apex_count} APEX payloads"
    )


if __name__ == "__main__":
    main()
