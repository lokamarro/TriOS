#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Buildroot passes TARGET_DIR as the first argument and the base archive's
# architecture (BR2_ROOTFS_POST_SCRIPT_ARGS) as the second.
set -eu

target=${1:?Buildroot TARGET_DIR is required}
architecture=${2:?the base archive architecture is required}
case "$architecture" in
aarch64) machine='AArch64' ;;
x86_64) machine='Advanced Micro Devices X86-64' ;;
*)
	printf '%s\n' "boot base: unsupported architecture $architecture" >&2
	exit 1
	;;
esac
[ -x "$target/bin/busybox" ] || {
	printf '%s\n' "boot base: BusyBox is missing" >&2
	exit 1
}
for program in dumpe2fs e2fsck mke2fs resize2fs; do
	[ -x "$target/sbin/$program" ] || {
		printf '%s\n' "boot base: $program is missing" >&2
		exit 1
	}
done
set -- "$target/bin/busybox" "$target/sbin/dumpe2fs" "$target/sbin/e2fsck" \
	"$target/sbin/mke2fs" "$target/sbin/resize2fs"
if readelf -l "$@" | grep -q 'Requesting program interpreter'; then
	printf '%s\n' "boot base: retained programs must be static" >&2
	exit 1
fi
for program in "$@"; do
	readelf -h "$program" | grep -q "Machine:[[:space:]]*$machine\$" || {
		printf '%s\n' "boot base: $program is not $architecture" >&2
		exit 1
	}
	if ! readelf -lW "$program" | awk '
		$1 == "LOAD" { found = 1; if ($NF != "0x4000") exit 1 }
		END { if (!found) exit 1 }
	'; then
		printf '%s\n' \
			"boot base: $program lacks 16 KiB ELF segments" >&2
		exit 1
	fi
done

staging=$(mktemp -d "${target}.minimal.XXXXXX")
cleanup()
{
	rm -rf -- "$staging"
}
trap cleanup EXIT HUP INT TERM

install -d -m 0755 \
	"$staging/bin" "$staging/dev" "$staging/etc" "$staging/newroot" \
	"$staging/proc" "$staging/sbin" "$staging/sys"
install -m 0755 "$target/init" "$staging/init"
install -m 0755 "$target/bin/busybox" "$staging/bin/busybox"
for program in dumpe2fs e2fsck mke2fs resize2fs; do
	install -m 0755 "$target/sbin/$program" "$staging/sbin/$program"
done
install -m 0644 "$target/etc/passwd" "$staging/etc/passwd"
install -m 0600 "$target/etc/shadow" "$staging/etc/shadow"
for applet in sh cp echo mkdir mount printf sleep sync umount [; do
	ln -s busybox "$staging/bin/$applet"
done
ln -s ../bin/busybox "$staging/sbin/blkid"
ln -s ../bin/busybox "$staging/sbin/switch_root"
# BusyBox's own mke2fs has no -E, so the ext4 formatter is e2fsprogs', and
# it picks the filesystem type from the name it is run as.
ln -s mke2fs "$staging/sbin/mkfs.ext4"
ln -s ../proc/mounts "$staging/etc/mtab"

find "$target" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
cp -a "$staging/." "$target/"
