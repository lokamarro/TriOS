#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Assemble the verified Android partition images into the read-only system disk.
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ANDROID_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
POLICY="$ANDROID_ROOT/support/image/disk-policy.json"
TARGET_FILES_VERIFIER="$ANDROID_ROOT/tools/verify-target-files.py"
DISK_TOOL="$ANDROID_ROOT/tools/ubd-disk.py"
BOOT_IMAGE_TOOL="$ANDROID_ROOT/tools/boot-image.py"
INITRAMFS_VERIFIER="$ANDROID_ROOT/tools/verify-initramfs.py"
RECOVERY_VERIFIER="$ANDROID_ROOT/tools/verify-lineage-recovery.py"

TARGET_FILES=
SIGNED_OPTION=
KERNEL_ARTIFACT=
MKBOOTFS=
MKBOOTIMG=
OUTPUT_DIR=
WORK_DIR=

# shellcheck disable=SC1090
source "$SCRIPT_DIR/common.sh"

usage()
{
	cat <<'EOF'
Usage: android/lineage-24/build/assemble-image.sh \
       --target-files ABSOLUTE_PATH \
       --kernel-artifact ABSOLUTE_PATH \
       --mkbootfs ABSOLUTE_PATH \
       --mkbootimg ABSOLUTE_PATH \
       --output-dir ABSOLUTE_PATH [--signed]

--signed marks the target files as the signed archive of a release, which the
verifier then requires to trust the release certificate alone.

Run locally on the x86_64 Linux builder after build-aosp.sh. The target-files
staging directory must remain beside its .zip archive. The generic and UML
first-stage ramdisks are composed into one raw newc ramdisk, wrapped in a
kernelless boot-image v4 init_boot partition. The target files' own LineageOS
recovery image, verified here, is the recovery partition. Those two boot
partitions and the five read-only EROFS images (sparse in the signed target
files, expanded here) are packed, each exactly its raw image rounded up to
4 KiB, into one raw GPT disk, system.img, and described by
partition-layout.json beside it, with a SHA-256 for every partition and the
data disk template the host turns into each guest's writable disk. Neither the
seed userdata and metadata images nor a misc partition are assembled: the app
creates the writable disk. The output directory receives the two files; an
existing file is never replaced.
EOF
}

cleanup()
{
	if [[ -n "$WORK_DIR" && -d "$WORK_DIR" ]]; then
		rm -rf -- "$WORK_DIR"
	fi
}
trap cleanup EXIT

while [[ $# -gt 0 ]]; do
	case "$1" in
	--target-files)
		[[ $# -ge 2 ]] || fail "--target-files requires a path"
		TARGET_FILES=$2
		shift 2
		;;
	--signed)
		SIGNED_OPTION=--signed
		shift
		;;
	--kernel-artifact)
		[[ $# -ge 2 ]] || fail "--kernel-artifact requires a path"
		KERNEL_ARTIFACT=$2
		shift 2
		;;
	--mkbootfs)
		[[ $# -ge 2 ]] || fail "--mkbootfs requires a path"
		MKBOOTFS=$2
		shift 2
		;;
	--mkbootimg)
		[[ $# -ge 2 ]] || fail "--mkbootimg requires a path"
		MKBOOTIMG=$2
		shift 2
		;;
	--output-dir)
		[[ $# -ge 2 ]] || fail "--output-dir requires a path"
		OUTPUT_DIR=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[[ -n "$TARGET_FILES" ]] || fail "--target-files is required"
[[ -n "$KERNEL_ARTIFACT" ]] || fail "--kernel-artifact is required"
[[ -n "$MKBOOTFS" ]] || fail "--mkbootfs is required"
[[ -n "$MKBOOTIMG" ]] || fail "--mkbootimg is required"
[[ -n "$OUTPUT_DIR" ]] || fail "--output-dir is required"
absolute_path "target-files archive" "$TARGET_FILES"
absolute_path "kernel artifact" "$KERNEL_ARTIFACT"
absolute_path "mkbootfs" "$MKBOOTFS"
absolute_path "mkbootimg" "$MKBOOTIMG"
absolute_path "output directory" "$OUTPUT_DIR"
[[ "$(uname -s)" == Linux ]] || fail "builder host must be Linux"
[[ "$(uname -m)" == x86_64 ]] || fail "builder host must be x86_64"
[[ -x "$TARGET_FILES_VERIFIER" ]] || fail "target-files verifier is missing"
[[ -x "$DISK_TOOL" ]] || fail "system disk tool is missing"
[[ -x "$BOOT_IMAGE_TOOL" ]] || fail "boot-image verifier is missing"
[[ -x "$INITRAMFS_VERIFIER" ]] || fail "initramfs verifier is missing"
[[ -x "$RECOVERY_VERIFIER" ]] || fail "LineageOS recovery verifier is missing"
[[ -f "$POLICY" ]] || fail "disk policy is missing"
[[ -f "$TARGET_FILES" && ! -L "$TARGET_FILES" ]] ||
	fail "target-files archive must be a regular non-symlink file"
[[ -d "$KERNEL_ARTIFACT" && ! -L "$KERNEL_ARTIFACT" ]] ||
	fail "kernel artifact must be a regular non-symlink directory"
KERNEL_ELF="$KERNEL_ARTIFACT/kernel/vmlinux"
[[ -f "$KERNEL_ELF" && ! -L "$KERNEL_ELF" ]] ||
	fail "kernel artifact ELF must be a regular non-symlink file"
[[ -f "$MKBOOTFS" && -x "$MKBOOTFS" ]] || fail "mkbootfs is not executable"
[[ -f "$MKBOOTIMG" && -x "$MKBOOTIMG" ]] || fail "mkbootimg is not executable"
case "$TARGET_FILES" in
*.zip) ;;
*) fail "target-files archive must end in .zip" ;;
esac
TARGET_FILES_DIR=${TARGET_FILES%.zip}
IMAGES_DIR="$TARGET_FILES_DIR/IMAGES"
[[ -d "$IMAGES_DIR" && ! -L "$IMAGES_DIR" ]] ||
	fail "target-files IMAGES staging directory is missing"
RECOVERY_IMAGE="$IMAGES_DIR/recovery.img"
[[ -f "$RECOVERY_IMAGE" && ! -L "$RECOVERY_IMAGE" ]] ||
	fail "target-files recovery image must be a regular non-symlink file"
RAMDISK_SOURCE="$TARGET_FILES_DIR/INIT_BOOT/RAMDISK"
VENDOR_FSTAB="$TARGET_FILES_DIR/VENDOR_BOOT/RAMDISK/first_stage_ramdisk/fstab.uml"
FILESYSTEM_CONFIG_SOURCE="$TARGET_FILES_DIR/META/init_boot_filesystem_config.txt"
NODE_LIST="$TARGET_FILES_DIR/META/ramdisk_node_list"
for path in "$RAMDISK_SOURCE" "$FILESYSTEM_CONFIG_SOURCE" "$NODE_LIST" "$VENDOR_FSTAB"; do
	[[ -e "$path" && ! -L "$path" ]] || fail "required staging input is missing: $path"
done
"$TARGET_FILES_VERIFIER" --target-files "$TARGET_FILES" ${SIGNED_OPTION:+"$SIGNED_OPTION"} \
	--kernel-elf "$KERNEL_ELF" \
	--kernel-config "$KERNEL_ARTIFACT/kernel/config" \
	--kernel-release-file "$KERNEL_ARTIFACT/kernel/kernel.release"

# The init_boot ramdisk is the generic ramdisk without its module and snapshot
# tools, plus the UML first-stage fstab.
WORK_DIR=$(mktemp -d)
RAMDISK_TREE="$WORK_DIR/ramdisk"
FILESYSTEM_CONFIG="$WORK_DIR/filesystem-config.txt"
RAMDISK="$WORK_DIR/ramdisk.img"
INIT_BOOT_IMAGE="$WORK_DIR/init_boot.img"
mkdir -p -- "$RAMDISK_TREE"
cp -a -- "$RAMDISK_SOURCE/." "$RAMDISK_TREE/"
for path in \
	system/bin/modprobe \
	system/bin/snapuserd \
	system/bin/snapuserd_ramdisk \
	system/etc/init/snapuserd.rc; do
	[[ ! -d "$RAMDISK_TREE/$path" ]] || fail "refusing to remove directory: $path"
	rm -f -- "$RAMDISK_TREE/$path"
done
install -d -m 0755 -- "$RAMDISK_TREE/first_stage_ramdisk/system/etc"
install -m 0644 -- "$VENDOR_FSTAB" \
	"$RAMDISK_TREE/first_stage_ramdisk/system/etc/fstab.uml"
if grep -Eq '^first_stage_ramdisk/system(/| )' "$FILESYSTEM_CONFIG_SOURCE"; then
	fail "generic ramdisk already owns first_stage_ramdisk/system"
fi
grep -Ev '^(system/bin/(modprobe|snapuserd|snapuserd_ramdisk)|system/etc/init/snapuserd\.rc) ' \
	"$FILESYSTEM_CONFIG_SOURCE" > "$FILESYSTEM_CONFIG"
cat >> "$FILESYSTEM_CONFIG" <<'EOF'
first_stage_ramdisk/system 0 0 755 capabilities=0x0
first_stage_ramdisk/system/etc 0 0 755 capabilities=0x0
first_stage_ramdisk/system/etc/fstab.uml 0 0 644 capabilities=0x0
EOF
"$MKBOOTFS" -f "$FILESYSTEM_CONFIG" -n "$NODE_LIST" "$RAMDISK_TREE" > "$RAMDISK"
"$INITRAMFS_VERIFIER" "$RAMDISK"

"$MKBOOTIMG" --header_version 4 --ramdisk "$RAMDISK" --output "$INIT_BOOT_IMAGE"
"$BOOT_IMAGE_TOOL" verify --image "$INIT_BOOT_IMAGE" --ramdisk "$RAMDISK"
"$BOOT_IMAGE_TOOL" verify --image "$RECOVERY_IMAGE"
"$RECOVERY_VERIFIER" --android-root "$ANDROID_ROOT" --image "$RECOVERY_IMAGE"
"$DISK_TOOL" assemble --policy "$POLICY" --images-dir "$IMAGES_DIR" \
	--image "init_boot=$INIT_BOOT_IMAGE" --output-dir "$OUTPUT_DIR"
LAYOUT="$OUTPUT_DIR/partition-layout.json"
DISK="$OUTPUT_DIR/system.img"
"$DISK_TOOL" verify --layout "$LAYOUT" --disk "$DISK" --hash
"$BOOT_IMAGE_TOOL" verify --layout "$LAYOUT" --disk "$DISK" \
	--partition init_boot --ramdisk "$RAMDISK"
"$BOOT_IMAGE_TOOL" verify --layout "$LAYOUT" --disk "$DISK" \
	--partition recovery
"$RECOVERY_VERIFIER" --android-root "$ANDROID_ROOT" \
	--layout "$LAYOUT" --disk "$DISK"
printf '%s\n' "assembled the Android 17 system disk: $DISK"
