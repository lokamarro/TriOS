# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Retain the tested page-agnostic Cuttlefish board foundation. The product
# makefile removes transports and HALs that Darwin UML cannot provide, while
# this board owns its static storage and module-free kernel.
include device/google/cuttlefish/vsoc_arm64_pgagnostic/BoardConfig.mk

# The inherited board names itself cutf, which ro.product.board and so
# Build.BOARD report. The device's own name replaces it. ro.board.platform and
# ro.hardware keep their inherited values: init scripts and HAL modules are
# selected by them.
TARGET_BOOTLOADER_BOARD_NAME := andarium

# The SoC identity (ro.soc.manufacturer, ro.soc.model) is a property file
# because a model with a space cannot be a PRODUCT_VENDOR_PROPERTIES word. The
# build adds it to ODM/etc/build.prop, and lineage_andarium.mk drops the
# Cuttlefish values from the vendor partition's.
TARGET_ODM_PROP += device/kernarium/andarium/config/odm.prop

# 32-bit applications run beside the 64-bit ones. The second architecture is
# ARMv8-A AArch32 built as the softfp armeabi-v7a ABI, with armeabi as the
# fallback for older native applications. The parent board is 64-bit only; the
# UML_ANDROID_MULTILIB switch of the product makes it start the secondary
# zygote. Native daemons and HALs stay 64-bit: IGNORE_PREFER32_ON_DEVICE turns
# every prefer32 module into a 64-bit one, so only applications, the zygotes
# and the WebView RELRO process run 32-bit code. Recovery, init_boot and the
# vendor ramdisk are primary-architecture images and stay 64-bit.
TARGET_2ND_ARCH := arm
TARGET_2ND_ARCH_VARIANT := armv8-a
TARGET_2ND_CPU_ABI := armeabi-v7a
TARGET_2ND_CPU_ABI2 := armeabi
TARGET_2ND_CPU_VARIANT := cortex-a53
TARGET_TRANSLATE_2ND_ARCH := false
IGNORE_PREFER32_ON_DEVICE := true

# The inherited page-size-agnostic product boots a 16 KiB kernel. This guest
# runs 4 KiB pages, so clear that choice and let ro.product.page_size report
# 4096. The product makefile cannot do this: its parent assigns the variable
# after the child file is read.
TARGET_BOOTS_16K :=

# The inherited mainline board routes ordinary application dexpreopt outputs
# into system_other. This product does not build that image, so keep the
# artifacts adjacent to their APKs in the immutable partition images instead.
BOARD_USES_SYSTEM_OTHER_ODEX :=

# The five immutable partitions are compressed EROFS images, which fstab.uml
# mounts read-only in the guest. The inherited board gives each its own file
# system type, so every one is named here. The compressor and the physical
# cluster size are the product's two parameters (lineage_andarium.mk), applied to
# every partition alike: no per-partition override, no extended option
# (ztailpacking, fragments, dedupe) and no block size other than the default
# 4 KiB, which the guest's 4 KiB pages require. The compress hints keep the
# .apex files uncompressed, so apexd's loop devices read them directly.
BOARD_SYSTEMIMAGE_FILE_SYSTEM_TYPE := erofs
BOARD_SYSTEM_EXTIMAGE_FILE_SYSTEM_TYPE := erofs
BOARD_PRODUCTIMAGE_FILE_SYSTEM_TYPE := erofs
BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE := erofs
BOARD_ODMIMAGE_FILE_SYSTEM_TYPE := erofs
BOARD_SYSTEMIMAGE_EROFS_BLOCKSIZE :=
BOARD_EROFS_COMPRESSOR := $(UML_ANDROID_EROFS_COMPRESSOR)
BOARD_EROFS_PCLUSTER_SIZE := $(UML_ANDROID_EROFS_PCLUSTER_SIZE)
BOARD_EROFS_COMPRESS_HINTS := device/kernarium/andarium/config/erofs_compress_hints.txt

# This product has no SwiftShader: its Vulkan driver is the Mesa Venus HAL.
# The inherited SwiftShader policy fragment grants bootanim, surfaceflinger,
# system_server and zygote self:process execmem, which the platform
# neverallow only tolerates while the SwiftShader insecure-execmem product
# flag is set.
BOARD_VENDOR_SEPOLICY_DIRS := $(filter-out \
    device/google/cuttlefish/shared/swiftshader/sepolicy, \
    $(BOARD_VENDOR_SEPOLICY_DIRS))

BOARD_VENDOR_SEPOLICY_DIRS += device/kernarium/andarium/sepolicy/vendor
# The Wi-Fi interface is virt_wifi, a station only: the kernel gives it no
# access-point or peer-to-peer interface type. The inherited board offers
# station, access point and peer-to-peer interfaces and a second station, which
# the HAL would report and the framework would then fail to create. One station
# keeps SoftAP, Wi-Fi Direct and Aware out of the framework. The flag that
# selects dual interfaces is ignored when the combinations are given, but it
# stays cleared so the board says one thing.
WIFI_HIDL_FEATURE_DUAL_INTERFACE :=
WIFI_HAL_INTERFACE_COMBINATIONS := {{{STA}, 1}}

# Properties the platform's core domains set belong to the system partitions: the
# Treble split forbids a core domain to set a vendor-owned one.
SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS += device/kernarium/andarium/sepolicy/system_ext/private
# The type of the file with the host's report of graphics memory pressure is public:
# the vendor's genfs_contexts labels the file with it.
SYSTEM_EXT_PUBLIC_SEPOLICY_DIRS += device/kernarium/andarium/sepolicy/system_ext/public

# The Android UML ELF is built and published independently. Android packaging
# must neither substitute a Cuttlefish kernel nor stage loadable modules.
PRODUCT_COPY_FILES := $(filter-out %:kernel,$(PRODUCT_COPY_FILES))
TARGET_KERNEL_PATH :=
SYSTEM_DLKM_SRC :=
KERNEL_MODULES_PATH :=
SYSTEM_VIRTIO_PREBUILTS_PATH :=
RAMDISK_KERNEL_MODULES :=

BOARD_GENERIC_RAMDISK_KERNEL_MODULES :=
BOARD_GENERIC_RAMDISK_KERNEL_MODULES_LOAD :=
BOARD_RECOVERY_KERNEL_MODULES :=
BOARD_VENDOR_CHARGER_KERNEL_MODULES :=
BOARD_VENDOR_CHARGER_KERNEL_MODULES_LOAD :=
BOARD_VENDOR_KERNEL_RAMDISK_CHARGER_KERNEL_MODULES_LOAD :=
BOARD_VENDOR_KERNEL_RAMDISK_KERNEL_MODULES :=
BOARD_VENDOR_KERNEL_RAMDISK_KERNEL_MODULES_BLOCKLIST_FILE :=
BOARD_VENDOR_KERNEL_RAMDISK_RECOVERY_KERNEL_MODULES_LOAD :=
BOARD_VENDOR_RAMDISK_CHARGER_KERNEL_MODULES_LOAD :=
BOARD_VENDOR_RAMDISK_KERNEL_MODULES :=
BOARD_VENDOR_RAMDISK_KERNEL_MODULES_BLOCKLIST_FILE :=
BOARD_VENDOR_RAMDISK_KERNEL_MODULES_LOAD :=
BOARD_VENDOR_RAMDISK_KERNEL_MODULES_OPTIONS_FILE :=
BOARD_VENDOR_RAMDISK_RECOVERY_KERNEL_MODULES_LOAD :=
BOARD_VENDOR_KERNEL_MODULES :=
BOARD_VENDOR_KERNEL_MODULES_BLOCKLIST_FILE :=
BOARD_VENDOR_KERNEL_MODULES_LOAD :=
BOARD_VENDOR_KERNEL_MODULES_2ND_STAGE_16KB_MODE :=
BOARD_SYSTEM_KERNEL_MODULES :=
BOARD_SYSTEM_KERNEL_MODULES_BLOCKLIST_FILE :=
BOARD_SYSTEM_KERNEL_MODULES_LOAD :=
BOARD_ODM_KERNEL_MODULES :=
BOARD_ODM_KERNEL_MODULES_BLOCKLIST_FILE :=
BOARD_KERNEL_MODULES_ZIP :=
BOARD_KERNEL_MODULES_ZIP_EXTRA_BLOCKED_16K_MODULES :=
BOARD_KERNEL_MODULES_ZIP_EXTRA_VENDOR_KERNEL_MODULES :=
BOARD_KERNEL_MODULES_ZIP_EXTRA_VENDOR_KERNEL_RAMDISK_LOADS :=
BOARD_KERNEL_MODULE_INTERFACE_VERSIONS :=
BOARD_KERNEL_PATH_16K :=
BOARD_KERNEL_MODULES_16K :=
BOARD_KERNEL_MODULES_LOAD_16K :=
TARGET_COPY_OUT_SYSTEM_DLKM := system/system_dlkm
TARGET_COPY_OUT_VENDOR_DLKM := vendor/vendor_dlkm
TARGET_COPY_OUT_ODM_DLKM := vendor/odm_dlkm
BOARD_SYSTEM_DLKMIMAGE_FILE_SYSTEM_TYPE :=
BOARD_VENDOR_DLKMIMAGE_FILE_SYSTEM_TYPE :=
BOARD_ODM_DLKMIMAGE_FILE_SYSTEM_TYPE :=

BOARD_AVB_VBMETA_CUSTOM_PARTITIONS := \
    $(filter-out vendor_dlkm system_dlkm odm_dlkm,$(BOARD_AVB_VBMETA_CUSTOM_PARTITIONS))
BOARD_AVB_VBMETA_VENDOR_DLKM :=
BOARD_AVB_VBMETA_SYSTEM_DLKM :=
BOARD_GOOGLE_SYSTEM_DYNAMIC_PARTITIONS_PARTITION_LIST := \
    $(filter-out system_dlkm,$(BOARD_GOOGLE_SYSTEM_DYNAMIC_PARTITIONS_PARTITION_LIST))
BOARD_GOOGLE_VENDOR_DYNAMIC_PARTITIONS_PARTITION_LIST := \
    $(filter-out vendor_dlkm odm_dlkm,$(BOARD_GOOGLE_VENDOR_DYNAMIC_PARTITIONS_PARTITION_LIST))
AB_OTA_PARTITIONS := $(filter-out vendor_dlkm system_dlkm odm_dlkm \
    vbmeta_vendor_dlkm vbmeta_system_dlkm,$(AB_OTA_PARTITIONS))

# The image builder's size limits. The release packs each image at its own
# size into the system disk (tools/ubd-disk.py), so a limit only has to stay
# above its image. The metadata and userdata images are built into the
# target files but not shipped: the app creates those partitions.
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 1677721600
BOARD_SYSTEM_EXTIMAGE_PARTITION_SIZE := 536870912
BOARD_PRODUCTIMAGE_PARTITION_SIZE := 603979776
BOARD_VENDORIMAGE_PARTITION_SIZE := 268435456
BOARD_ODMIMAGE_PARTITION_SIZE := 4194304
BOARD_METADATAIMAGE_PARTITION_SIZE := 16777216
BOARD_USERDATAIMAGE_PARTITION_SIZE := 134217728
# mke2fs gives a filesystem under 512 MiB one inode per 4 KiB, and the first
# boot's resize2fs keeps the inodes per group, so the grown /data would hold
# more inodes than blocks. 8192 inodes in the image's one 32768-block group is
# one per 16 KiB, ext4's default ratio, and survives the resize.
BOARD_USERDATAIMAGE_EXTFS_INODE_COUNT := 8192
BOARD_INIT_BOOT_IMAGE_PARTITION_SIZE := 8388608
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 20971520

# The final GPT has init_boot and recovery ramdisk containers, but the Android
# UML kernel remains the separately signed executable. Both images therefore
# use Android boot image v4 with no embedded kernel.
BOARD_INIT_BOOT_HEADER_VERSION := 4
BOARD_BOOT_HEADER_VERSION := 4
BOARD_EXCLUDE_KERNEL_FROM_RECOVERY_IMAGE := true
BOARD_USES_FULL_RECOVERY_IMAGE := true
TARGET_RECOVERY_FSTAB := device/kernarium/andarium/config/recovery.fstab
# The inherited recovery UI reaches ADB over eth1, which Cuttlefish provides and
# this machine does not. Darwin's own UI uses eth0, the Virtio-net interface.
TARGET_RECOVERY_UI_LIB := librecovery_ui_andarium
# Recovery draws into a dumb buffer, which the Virtio-GPU driver always creates
# as B8G8R8X8 whatever format the framebuffer declares afterwards. The inherited
# ABGR_8888 declares RGBA8888 for bytes in R, G, B, A order that nothing
# converts, so red and blue swap on screen. BGRX_8888 is XRGB8888, the
# buffer's own byte order.
TARGET_RECOVERY_PIXEL_FORMAT := BGRX_8888
TARGET_NO_RECOVERY := false

BOARD_SUPER_PARTITION_SIZE :=
BOARD_SUPER_PARTITION_GROUPS :=
BOARD_GOOGLE_SYSTEM_DYNAMIC_PARTITIONS_PARTITION_LIST :=
BOARD_GOOGLE_SYSTEM_DYNAMIC_PARTITIONS_SIZE :=
BOARD_GOOGLE_VENDOR_DYNAMIC_PARTITIONS_PARTITION_LIST :=
BOARD_GOOGLE_VENDOR_DYNAMIC_PARTITIONS_SIZE :=
BOARD_BUILD_SUPER_IMAGE_BY_DEFAULT := false
BOARD_SUPER_IMAGE_IN_UPDATE_PACKAGE := false
AB_OTA_UPDATER := false
AB_OTA_PARTITIONS :=
BOARD_PARTIAL_OTA_UPDATE_PARTITIONS_LIST :=
BOARD_BOOTLOADER_IN_UPDATE_PACKAGE := false
TARGET_SKIP_OTA_PACKAGE := true

# The outer release manifest authenticates the complete fresh-only image.
BOARD_AVB_ENABLE := false
BOARD_AVB_VBMETA_SYSTEM :=
BOARD_AVB_VBMETA_CUSTOM_PARTITIONS :=
BOARD_AVB_VBMETA_VENDOR_DLKM :=
BOARD_AVB_VBMETA_SYSTEM_DLKM :=
BOARD_AVB_BOOT_KEY_PATH :=
BOARD_AVB_BOOT_ALGORITHM :=
BOARD_AVB_INIT_BOOT_KEY_PATH :=
BOARD_AVB_INIT_BOOT_ALGORITHM :=
