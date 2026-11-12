#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the x86-64 LTP qualification root on native x86-64 Linux.
set -euo pipefail

PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEBIAN_SUITE=trixie
LTP_TAG=20260529
LTP_REPOSITORY=https://github.com/linux-test-project/ltp.git

fail()
{
	printf '%s\n' "tools/rootfs/ltp-x86_64/build.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: tools/rootfs/ltp-x86_64/build.sh --work PATH --output PATH.erofs

Run as an ordinary user with passwordless sudo on native x86-64 Debian Linux.
Both paths must be absolute, distinct, and absent. The completed EROFS image,
package inventory, ELF inventory, and checksum are published after validation.
EOF
}

. "$SCRIPT_DIR/../producer.sh"
producer_parse_erofs_options "$@"
producer_require_host x86_64 chroot debootstrap git mkfs.erofs mountpoint \
	python3 sha256sum sort sudo
unmount_root()
{
	for mount_point in dev sys proc; do
		if mountpoint -q "$root/$mount_point"; then
			sudo umount -R "$root/$mount_point"
		fi
	done
}
producer_exit_hook=unmount_root
producer_stage_erofs_output

# The runtime set matches the LTP-relevant userspace of the arm64 product
# rootfs. The build set stays installed because several LTP command
# tests exercise the toolchain, archivers, and gdb.
runtime_packages='adduser bash bc bzip2 ca-certificates coreutils curl dash
diffutils dmsetup e2fsprogs file findutils gdb grep gzip hostname iproute2
iptables iputils-ping kmod less libacl1 libaio1t64 libblkid1 libbpf1
libcap2-bin libcrypt1 libfuse3-4 libibverbs1 libkeyutils1 libmnl0 libmount1
libnuma1 libselinux1 libssl3t64 libtirpc3t64 login mawk mount mtr-tiny nano
net-tools nftables openssh-client parted passwd pciutils perl procps psmisc
python3 quota sed sudo systemd systemd-sysv tar udev unzip util-linux
xz-utils'
build_packages='autoconf automake build-essential git libacl1-dev libaio-dev
libattr1-dev libcap-dev libnuma-dev libselinux1-dev linux-libc-dev make
pkgconf'

printf '%s\n' '==> Bootstrapping Debian 13 amd64'
sudo debootstrap --arch=amd64 --variant=minbase \
	--include="$(printf '%s\n' $runtime_packages $build_packages | paste -sd, -)" \
	"$DEBIAN_SUITE" "$root" http://deb.debian.org/debian

printf '%s\n' "==> Building LTP $LTP_TAG"
git clone --quiet --depth 1 --branch "$LTP_TAG" "$LTP_REPOSITORY" \
	"$work_dir/ltp"
sudo install -d -m 0755 "$root/usr/src"
sudo cp -a "$work_dir/ltp" "$root/usr/src/ltp"
for mount_point in proc sys dev; do
	sudo mount --rbind "/$mount_point" "$root/$mount_point"
done
sudo chroot "$root" /bin/bash -euc '
	cd /usr/src/ltp
	make autotools
	./configure --prefix=/opt/ltp
	make -j"$(nproc)"
	make install
'
unmount_root
[ "$(cat "$root/opt/ltp/Version")" = "$LTP_TAG" ] || \
	fail "installed LTP does not report version $LTP_TAG"
sudo rm -rf -- "$root/usr/src/ltp"

printf '%s\n' '==> Auditing the installed package state'
package_audit=$(sudo chroot "$root" dpkg --audit)
[ -z "$package_audit" ] || fail "dpkg --audit reported incomplete packages:
$package_audit"
sudo chroot "$root" dpkg-query -W -f='${binary:Package}\t${Version}\n' |
	sort > "$staging.packages.tsv"

printf '%s\n' '==> Verifying that every ELF payload is x86-family'
sudo python3 "$SCRIPT_DIR/../x86_64-desktop/verify-elf.py" \
	--root "$root" --output "$staging.elf.tsv"
sudo chown "$owner_uid:$owner_gid" "$staging.elf.tsv"

sudo chroot "$root" apt-get clean
sudo rm -rf -- "$root/var/lib/apt/lists"/* "$root/var/cache/apt"/* \
	"$root/tmp"/* "$root/var/tmp"/* "$root/run"/*
sudo find "$root/var/log" -type f -exec truncate -s 0 {} \;
printf '%s\n' uml | sudo tee "$root/etc/hostname" >/dev/null
sudo truncate -s 0 "$root/etc/machine-id"

printf '%s\n' '==> Building EROFS'
sudo mkfs.erofs -L uml-ltp-x86-64 -zlz4hc "$staging" "$root"
producer_publish_erofs
printf '%s\n' "Built x86-64 LTP rootfs: $output"
