#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Produce the mainline Linux starter artifacts on a native arm64 Linux host.
set -eu

PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILDROOT_VERSION=2026.05.3
BUILDROOT_SHA256=f43f1627082a45848f73083b3b8d59f32bb4c77a449a8ead8a2a1f91849dea00
BUSYBOX_VERSION=1.38.0
BUSYBOX_SHA256=34f9ea6ff8636f2c9241153b9114eefa9e65674a45318ae1ef95bb5f31c53bb2
ALPINE_VERSION=3.24.2
ALPINE_SHA256=9bf70a7f18ea44094cbb5f70c58f9af129c8214745743db0e68e5502cc2ce773
DEBIAN_SUITE=trixie
DEBIAN_MIRROR=http://deb.debian.org/debian
# The armhf twin of debian-13-minimal installs the same package list through
# debootstrap alone, so that no ARM code needs the network. debootstrap
# resolves a dependency alternative to its first package, where apt keeps one
# that is already installed, so the twin has one package more: adduser, from
# dbus-system-bus-common's "adduser | systemd-sysusers", which systemd
# provides. --exclude cannot drop it, because the dependency pulls it back.
ARMHF_INCLUDE=systemd-sysv,systemd-resolved,udev,kmod,dbus,ca-certificates,libbpf1
DEFAULT_PRODUCTS="boot-base-aarch64 boot-base-x86_64 alpine debian-13-minimal debian-13-plasma-wayland"
KNOWN_PRODUCTS="$DEFAULT_PRODUCTS debian-13-minimal-armhf"

fail()
{
	printf '%s\n' "tools/rootfs/starter/build-all.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: tools/rootfs/starter/build-all.sh --work PATH --output PATH [--jobs N]
       [--product NAME]...

Run as an ordinary user with passwordless sudo on native arm64 Linux. PATHs
must be absolute and distinct outside the source tree. The output path must
not exist; the completed release directory is published atomically.

--product builds only the named products, each at most once; by default the
release holds every product but the armhf one:
  boot-base-aarch64 boot-base-x86_64 alpine debian-13-minimal
  debian-13-plasma-wayland debian-13-minimal-armhf

debian-13-minimal-armhf runs ARM programs under qemu-arm (Debian's qemu-user)
in private namespaces.
EOF
}

. "$SCRIPT_DIR/../producer.sh"
work_dir=
output_dir=
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1\n')
products=
while [ "$#" -gt 0 ]; do
	case "$1" in
	--work)
		[ "$#" -ge 2 ] || fail "--work requires a path"
		work_dir=$2
		shift 2
		;;
	--output)
		[ "$#" -ge 2 ] || fail "--output requires a path"
		output_dir=$2
		shift 2
		;;
	--jobs)
		[ "$#" -ge 2 ] || fail "--jobs requires a positive integer"
		jobs=$2
		shift 2
		;;
	--product)
		[ "$#" -ge 2 ] || fail "--product requires a name"
		case " $KNOWN_PRODUCTS " in
		*" $2 "*) ;;
		*) fail "unknown product: $2" ;;
		esac
		case " $products " in
		*" $2 "*) fail "product named twice: $2" ;;
		esac
		products="$products $2"
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$work_dir" ] && [ -n "$output_dir" ] || {
	usage >&2
	exit 2
}
case "$work_dir:$output_dir" in
/*:/*) ;;
*) fail "--work and --output must be absolute" ;;
esac
[ "$work_dir" != "$output_dir" ] || fail "work and output directories must differ"
case "$jobs" in ''|*[!0-9]*) fail "--jobs must be a positive integer" ;; esac
[ "$jobs" -gt 0 ] || fail "--jobs must be a positive integer"
[ -n "$products" ] || products=$DEFAULT_PRODUCTS

wants()
{
	case " $products " in *" $1 "*) return 0 ;; esac
	return 1
}

producer_require_host aarch64 chroot cpio curl debootstrap e2fsck gzip make \
	mkfs.ext4 python3 resize2fs sgdisk sha256sum sudo tar
if wants debian-13-minimal-armhf; then
	sudo -n -- sh "$SCRIPT_DIR/armhf-run.sh" --check
fi

requested_output_dir=$output_dir
output_parent=$(dirname -- "$requested_output_dir")
output_name=$(basename -- "$requested_output_dir")
case "$output_name" in ''|.|..|/) fail "unsafe output name: $output_name" ;; esac
mkdir -p -- "$work_dir" "$output_parent"
work_dir=$(CDPATH= cd -- "$work_dir" && pwd -P)
output_parent=$(CDPATH= cd -- "$output_parent" && pwd -P)
final_output_dir=$output_parent/$output_name
case "$work_dir" in /|/home|/Users|/tmp) fail "unsafe work directory: $work_dir" ;; esac
case "$final_output_dir" in
/|/home|/Users|/tmp) fail "unsafe output directory: $final_output_dir" ;;
esac
[ "$work_dir" != "$final_output_dir" ] || fail "work and output directories must differ"
producer_require_absent "output directory" "$final_output_dir"
chroot_proc=
producer_exit_hook=unmount_chroot_proc
producer_stage "$output_parent" "$output_name"
output_dir=$staging_dir

sources_dir=$work_dir/sources
roots_dir=$work_dir/roots
builds_dir=$work_dir/builds
mkdir -p -- "$sources_dir" "$roots_dir" "$builds_dir"
owner_uid=$(id -u)
owner_gid=$(id -g)

as_root()
{
	sudo -- "$@"
}

reset_root_directory()
{
	directory=$1
	case "$directory" in "$roots_dir"/*) ;;
	*) fail "refusing to reset a directory outside $roots_dir: $directory" ;;
	esac
	if [ -e "$directory" ]; then
		as_root rm -rf -- "$directory"
	fi
	as_root mkdir -p -- "$directory"
}

# Package maintainer scripts need /proc in the chroot. Without it,
# systemd-hwdb cannot replace hwdb.bin, and the udev hardware database
# silently misses the entries of every later package.
mount_chroot_proc()
{
	as_root mount -t proc proc "$1/proc"
	chroot_proc=$1/proc
}

unmount_chroot_proc()
{
	[ -n "$chroot_proc" ] || return 0
	as_root umount -- "$chroot_proc"
	chroot_proc=
}

download_checked()
{
	url=$1
	expected=$2
	destination=$3
	if [ ! -f "$destination" ]; then
		partial=$destination.partial
		rm -f -- "$partial"
		curl -fL --retry 3 --output "$partial" "$url"
		mv -- "$partial" "$destination"
	fi
	actual=$(sha256sum "$destination" | awk '{print $1}')
	[ "$actual" = "$expected" ] || fail "checksum mismatch for $destination"
}

artifact_path()
{
	path=$output_dir/$1
	[ ! -e "$path" ] && [ ! -L "$path" ] || fail "refusing to replace artifact: $path"
	printf '%s\n' "$path"
}

prepare_busybox_config()
{
	buildroot_archive=$sources_dir/buildroot-$BUILDROOT_VERSION.tar.xz
	busybox_archive=$sources_dir/busybox-$BUSYBOX_VERSION.tar.bz2
	download_checked \
		"https://buildroot.org/downloads/buildroot-$BUILDROOT_VERSION.tar.xz" \
		"$BUILDROOT_SHA256" "$buildroot_archive"
	download_checked \
		"https://www.busybox.net/downloads/busybox-$BUSYBOX_VERSION.tar.bz2" \
		"$BUSYBOX_SHA256" "$busybox_archive"
	buildroot_source=$sources_dir/buildroot-$BUILDROOT_VERSION
	busybox_source=$sources_dir/busybox-$BUSYBOX_VERSION
	[ -d "$buildroot_source" ] || tar -C "$sources_dir" -xf "$buildroot_archive"
	[ -d "$busybox_source" ] || tar -C "$sources_dir" -xf "$busybox_archive"

	busybox_config=$builds_dir/busybox-minimal.config
	busybox_log=$builds_dir/busybox-allnoconfig.log
	make -s -C "$busybox_source" allnoconfig > "$busybox_log"
	while IFS= read -r symbol; do
		case "$symbol" in \#*|'') continue ;; esac
		if grep -qx "CONFIG_$symbol=y" "$busybox_source/.config"; then
			continue
		fi
		grep -qx "# CONFIG_$symbol is not set" "$busybox_source/.config" || \
			fail "BusyBox does not define required option CONFIG_$symbol"
		sed -i "s/^# CONFIG_$symbol is not set\$/CONFIG_$symbol=y/" \
			"$busybox_source/.config"
	done < "$SCRIPT_DIR/buildroot/busybox.fragment"
	make -s -C "$busybox_source" silentoldconfig
	while IFS= read -r symbol; do
		case "$symbol" in \#*|'') continue ;; esac
		grep -qx "CONFIG_$symbol=y" "$busybox_source/.config" || \
			fail "BusyBox rejected required option CONFIG_$symbol"
	done < "$SCRIPT_DIR/buildroot/busybox.fragment"
	cp -- "$busybox_source/.config" "$busybox_config"
}

# The base archive holds only the static tools a per-run /init needs; the
# launcher appends that /init. Buildroot builds its own cross toolchain, so
# this aarch64 host also produces the archive for x86_64-userspace guests.
build_boot_base()
{
	architecture=$1
	case "$architecture" in
	aarch64) architecture_config='BR2_aarch64=y
BR2_ARM64_PAGE_SIZE_16K=y' ;;
	x86_64) architecture_config='BR2_x86_64=y' ;;
	*) fail "unsupported boot base architecture: $architecture" ;;
	esac
	printf '%s\n' "==> Building the $architecture boot base archive"
	buildroot_output=$builds_dir/buildroot-$architecture
	mkdir -p -- "$buildroot_output"
	if [ -d "$buildroot_output/build" ]; then
		# A failed post-build may leave the generated target deliberately
		# pruned while package stamps still describe the full tree. Reinstall
		# target payloads, but retain the expensive compiled toolchain cache.
		rm -rf -- \
			"$buildroot_output/images" \
			"$buildroot_output/staging" \
			"$buildroot_output/target"
		find "$buildroot_output/build" -name .stamp_target_installed \
			-delete
	fi
	generated_defconfig=$buildroot_output/uml-boot-base-defconfig
	{
		printf '%s\n' "$architecture_config"
		sed \
			-e "s|@BUSYBOX_CONFIG@|$busybox_config|g" \
			-e "s|@OVERLAY@|$SCRIPT_DIR/buildroot/overlay|g" \
			-e "s|@POST_BUILD@|$SCRIPT_DIR/buildroot/post-build.sh|g" \
			-e "s|@ARCHITECTURE@|$architecture|g" \
			"$SCRIPT_DIR/buildroot/defconfig.in"
	} > "$generated_defconfig"
	make -C "$buildroot_source" O="$buildroot_output" \
		BR2_DL_DIR="$sources_dir/downloads" \
		BR2_DEFCONFIG="$generated_defconfig" defconfig
	make -C "$buildroot_source" O="$buildroot_output" \
		BR2_DL_DIR="$sources_dir/downloads" -j"$jobs"

	image=$buildroot_output/images/rootfs.cpio.gz
	[ -f "$image" ] || fail "Buildroot did not produce $image"
	gzip -t "$image"
	listing=$buildroot_output/boot-base.list
	gzip -dc "$image" | cpio -it > "$listing" 2>/dev/null
	for required in init bin/busybox bin/cp bin/printf bin/umount sbin/blkid \
		sbin/dumpe2fs sbin/e2fsck sbin/mke2fs sbin/mkfs.ext4 \
		sbin/resize2fs sbin/switch_root; do
		grep -qx "\./$required\|$required" "$listing" || \
			fail "boot base is missing $required"
	done
	destination=$(artifact_path "uml-boot-base-$architecture.cpio.gz")
	cp -- "$image" "$destination"

	# The native toolchain is several GiB and is no longer needed once the
	# self-contained cpio archive has been verified and copied into the release.
	rm -rf -- \
		"$buildroot_output/build" \
		"$buildroot_output/host" \
		"$buildroot_output/target" \
		"$buildroot_output/staging" \
		"$buildroot_output/per-package"
}

configure_alpine()
{
	root=$1
	write_root_file "$root/etc/apk/repositories" 0644 <<EOF
https://dl-cdn.alpinelinux.org/alpine/v3.24/main
https://dl-cdn.alpinelinux.org/alpine/v3.24/community
EOF
	as_root cp -L /etc/resolv.conf "$root/etc/resolv.conf"
	# The kernels have no uevent helper for BusyBox mdev, so mdevd, Alpine's
	# netlink device manager, owns /dev as setup-devd would configure it.
	as_root chroot "$root" /sbin/apk add --no-cache alpine-base ifupdown-ng \
		mdevd mdevd-openrc
	for service_level in \
		"devfs sysinit" "procfs sysinit" "sysfs sysinit" "mdevd sysinit" \
		"mdevd-init sysinit" "hostname boot" "bootmisc boot" \
		"networking boot" "local default" "killprocs shutdown" \
		"mount-ro shutdown"; do
		set -- $service_level
		if [ -x "$root/etc/init.d/$1" ]; then
			as_root chroot "$root" /sbin/rc-update add "$1" "$2"
		fi
	done
	# The kernel creates a virtio port's name attribute only for a named
	# port, and the console port is unnamed.
	mdev_conf=$root/etc/mdev.conf
	unguarded_rule='SUBSYSTEM=virtio-ports;vport.* root:root 0600 @mkdir -p virtio-ports; ln -sf ../$MDEV virtio-ports/$(cat /sys/class/virtio-ports/$MDEV/name)'
	guarded_rule='SUBSYSTEM=virtio-ports;vport.* root:root 0600 @if [ -r /sys/class/virtio-ports/$MDEV/name ]; then mkdir -p virtio-ports; ln -sf ../$MDEV virtio-ports/$(cat /sys/class/virtio-ports/$MDEV/name); fi'
	[ "$(as_root grep -cxF -- "$unguarded_rule" "$mdev_conf")" = 1 ] || \
		fail "unexpected virtio-ports rule in $mdev_conf"
	as_root cat "$mdev_conf" | \
		awk -v old="$unguarded_rule" -v new="$guarded_rule" \
			'$0 == old { print new; next } { print }' | \
		write_root_file "$mdev_conf" 0644
	as_root grep -qxF -- "$guarded_rule" "$mdev_conf" || \
		fail "could not guard the virtio-ports rule in $mdev_conf"
	# Nothing is loadable into the monolithic kernels; the default entries
	# (af_packet and ipv6) are built in.
	write_root_file "$root/etc/modules" 0644 <<'EOF'
# The Linux kernels for this image are monolithic; list no modules here.
EOF
	write_root_file "$root/etc/network/interfaces" 0644 <<'EOF'
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet dhcp
EOF
	write_root_file "$root/etc/hostname" 0644 <<'EOF'
alpine
EOF
	write_root_file "$root/etc/fstab" 0644 <<'EOF'
/dev/ubda1 / ext4 defaults,noatime 0 1
EOF
	write_root_file "$root/sbin/autologin-root" 0755 <<'EOF'
#!/bin/sh
exec /bin/login -f root
EOF
	write_root_file "$root/etc/inittab" 0644 <<'EOF'
::sysinit:/sbin/openrc sysinit
::sysinit:/sbin/openrc boot
::wait:/sbin/openrc default
hvc0::respawn:/sbin/getty -n -l /sbin/autologin-root 115200 hvc0 vt100
::shutdown:/sbin/openrc shutdown
EOF
	if ! as_root grep -qx hvc0 "$root/etc/securetty"; then
		printf '%s\n' hvc0 | as_root tee -a "$root/etc/securetty" >/dev/null
	fi
	producer_run_captured as_root chroot "$root" /usr/bin/passwd -d root
	as_root rm -rf -- "$root/var/cache/apk"/* "$root/tmp"/* "$root/var/tmp"/*
	: > "$work_dir/alpine.packages.tsv"
	as_root chroot "$root" /sbin/apk info -v | sort > "$work_dir/alpine.packages.tsv"
}

build_alpine()
{
	printf '%s\n' "==> Building Alpine $ALPINE_VERSION minimal GPT image"
	archive=$sources_dir/alpine-minirootfs-$ALPINE_VERSION-aarch64.tar.gz
	download_checked \
		"https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/$(basename "$archive")" \
		"$ALPINE_SHA256" "$archive"
	root=$roots_dir/alpine
	reset_root_directory "$root"
	as_root tar --numeric-owner -C "$root" -xzf "$archive"
	configure_alpine "$root"
	destination=$(artifact_path alpine-$ALPINE_VERSION-minimal.img)
	as_root python3 "$SCRIPT_DIR/make-gpt-image.py" \
		--root "$root" --output "$destination" --label alpine \
		--headroom-mib 64
	as_root chown "$owner_uid:$owner_gid" "$destination"
	cp "$work_dir/alpine.packages.tsv" "$destination.packages.tsv"
}

# Everything an image's package set needs before its packages are installed.
prepare_debian_base()
{
	root=$1
	write_root_file "$root/etc/apt/sources.list" 0644 <<EOF
deb http://deb.debian.org/debian $DEBIAN_SUITE main
deb http://deb.debian.org/debian $DEBIAN_SUITE-updates main
deb http://security.debian.org/debian-security $DEBIAN_SUITE-security main
EOF
	write_root_file "$root/usr/sbin/policy-rc.d" 0755 <<'EOF'
#!/bin/sh
exit 101
EOF
}

install_debian_base()
{
	root=$1
	as_root cp -L /etc/resolv.conf "$root/etc/resolv.conf"
	as_root chroot "$root" /usr/bin/env DEBIAN_FRONTEND=noninteractive \
		apt-get update
	# systemd loads libbpf at runtime for its cgroup BPF features and reports
	# them disabled at every boot when the library is absent.
	as_root chroot "$root" /usr/bin/env DEBIAN_FRONTEND=noninteractive \
		apt-get install -y --no-install-recommends \
		systemd-sysv systemd-resolved udev kmod dbus ca-certificates \
		libbpf1
}

# The image's own policy, written and enabled from the host: it executes
# nothing from the root, so it serves every architecture alike.
configure_debian_base()
{
	root=$1
	hostname=$2
	autologin_user=$3
	write_root_file "$root/etc/hostname" 0644 <<EOF
$hostname
EOF
	write_root_file "$root/etc/hosts" 0644 <<EOF
127.0.0.1 localhost
127.0.1.1 $hostname
::1 localhost ip6-localhost ip6-loopback
EOF
	write_root_file "$root/etc/fstab" 0644 <<'EOF'
/dev/ubda1 / ext4 defaults,noatime 0 1
EOF
	# Debian's systemd links /etc/default/locale to this file at boot, and
	# pam_env reads it for every login. C.UTF-8 needs no locales package.
	write_root_file "$root/etc/locale.conf" 0644 <<'EOF'
LANG=C.UTF-8
EOF
	# Match wired links by type rather than by kernel-assigned name, and keep
	# the image's own hostname: the DHCP-supplied one could only be applied
	# through hostnamed, which refuses networkd without polkit.
	write_root_file "$root/etc/systemd/network/20-wired.network" 0644 <<'EOF'
[Match]
Type=ether
Kind=!*

[Network]
DHCP=yes
IPv6AcceptRA=yes

[DHCPv4]
UseDNS=yes
UseHostname=no

[DHCPv6]
UseHostname=no
EOF
	write_root_file \
		"$root/etc/systemd/system/serial-getty@hvc0.service.d/autologin.conf" \
		0644 <<EOF
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin $autologin_user --noclear - \$TERM
EOF
	as_root systemctl --root="$root" enable \
		systemd-networkd.service systemd-resolved.service \
		serial-getty@hvc0.service
	as_root ln -sfn /run/systemd/resolve/stub-resolv.conf "$root/etc/resolv.conf"
}

prune_debian()
{
	root=$1
	as_root rm -f -- "$root/usr/sbin/policy-rc.d"
	as_root rm -rf -- "$root/var/lib/apt/lists"/* "$root/var/cache/apt"/* \
		"$root/tmp"/* "$root/var/tmp"/* "$root/run"/*
	as_root find "$root/var/log" -type f -exec truncate -s 0 {} \;
	as_root find "$root/usr/share/doc" -type f ! -name copyright -delete
	as_root find "$root/usr/share/doc" -type l -delete
	as_root find "$root/usr/share/doc" -depth -type d -empty -delete
	as_root rm -rf -- "$root/usr/share/man"/* "$root/usr/share/info"/* \
		"$root/usr/share/lintian" "$root/usr/share/doc-base"
	as_root find "$root/usr/share/locale" -mindepth 1 -maxdepth 1 \
		! -name 'en' ! -name 'en_*' -exec rm -rf -- {} +
	producer_reset_machine_id "$root"
}

debootstrap_root()
{
	root=$1
	reset_root_directory "$root"
	as_root debootstrap --arch=arm64 --variant=minbase --components=main \
		"$DEBIAN_SUITE" "$root" http://deb.debian.org/debian
}

build_debian_minimal()
{
	printf '%s\n' "==> Building Debian 13 minimal GPT image"
	root=$roots_dir/debian-minimal
	debootstrap_root "$root"
	mount_chroot_proc "$root"
	prepare_debian_base "$root"
	install_debian_base "$root"
	configure_debian_base "$root" debian root
	as_root chroot "$root" /usr/bin/passwd -d root
	as_root systemctl --root="$root" set-default multi-user.target
	as_root chroot "$root" dpkg-query -W -f='${binary:Package}\t${Version}\n' | \
		sort > "$work_dir/debian-minimal.packages.tsv"
	unmount_chroot_proc
	cat "$root/etc/debian_version" > "$work_dir/debian.version"
	prune_debian "$root"
	destination=$(artifact_path debian-13-minimal.img)
	as_root python3 "$SCRIPT_DIR/make-gpt-image.py" \
		--root "$root" --output "$destination" --label debian-min \
		--headroom-mib 96
	as_root chown "$owner_uid:$owner_gid" "$destination"
	cp "$work_dir/debian-minimal.packages.tsv" "$destination.packages.tsv"
}

configure_plasma_user()
{
	root=$1
	# PipeWire and WirePlumber are the session's audio server: pipewire-pulse
	# serves the PulseAudio clients (Plasma and Chromium), pipewire-alsa
	# routes plain ALSA clients through the server rather than around it,
	# and RealtimeKit grants the audio threads their realtime priority.
	# Bluetooth audio is left out because the guest has no Bluetooth.
	as_root chroot "$root" /usr/bin/env DEBIAN_FRONTEND=noninteractive \
		apt-get install -y --no-install-recommends \
		plasma-desktop systemsettings konsole chromium sudo fonts-dejavu-core \
		libgl1-mesa-dri mesa-va-drivers vainfo ffmpeg \
		xdg-desktop-portal-kde \
		pipewire wireplumber pipewire-pulse pipewire-alsa rtkit
	# Chromium's hardware video encode and decode go through VA-API, and the
	# Virtio video device has a VA-API driver of its own: built here against
	# the image's libva, with the Chromium configuration that selects it.
	producer_install_uml_va_driver "$root" "$SCRIPT_DIR/../../video/va-v4l2"
	write_root_file \
		"$root/etc/wireplumber/wireplumber.conf.d/50-no-bluetooth.conf" \
		0644 <<'EOF'
# The guest has no Bluetooth controller, and the image carries neither BlueZ
# nor PipeWire's Bluetooth plugin, so WirePlumber does not monitor Bluetooth.
wireplumber.profiles = {
  main = {
    hardware.bluetooth = disabled
  }
}
EOF
	# bubblewrap's sysctl.d file sets kernel.unprivileged_userns_clone, a
	# Debian kernel patch that the mainline kernels booting this image lack;
	# they already allow unprivileged user namespaces. Mask the file so
	# systemd-sysctl does not warn about the missing knob at every boot.
	as_root test -e "$root/usr/lib/sysctl.d/50-bubblewrap.conf" || \
		fail "bubblewrap no longer ships 50-bubblewrap.conf; drop its mask"
	as_root install -d -m 0755 "$root/etc/sysctl.d"
	as_root ln -sfn /dev/null "$root/etc/sysctl.d/50-bubblewrap.conf"
	as_root chroot "$root" useradd -m -s /bin/bash debian
	as_root chroot "$root" passwd -d debian
	debian_uid=$(as_root chroot "$root" id -u debian)
	debian_gid=$(as_root chroot "$root" id -g debian)
	for group in sudo video audio input render; do
		if as_root chroot "$root" getent group "$group" >/dev/null; then
			as_root chroot "$root" usermod -a -G "$group" debian
		fi
	done
	write_root_file "$root/etc/sudoers.d/debian" 0440 <<'EOF'
debian ALL=(ALL:ALL) NOPASSWD: ALL
EOF
	write_root_file \
		"$root/etc/systemd/system/getty@tty1.service.d/autologin.conf" \
		0644 <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin debian --noclear %I $TERM
EOF
	write_root_file "$root/home/debian/.profile" 0644 <<'EOF'
# Start the image-owned Plasma Wayland session from the autologin VT.
if [ "${XDG_VTNR:-}" = 1 ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    exec /usr/bin/dbus-run-session -- /usr/bin/startplasma-wayland
fi
EOF
	as_root chown "$debian_uid:$debian_gid" "$root/home/debian/.profile"
	# The session's XDG directories exist before its first login, so the
	# Folder View containment below has a Desktop to show.
	for directory in Desktop Documents Downloads Music Pictures Public \
		Templates Videos .config .local .local/share .local/share/pki \
		.local/share/Trash .local/share/Trash/files \
		.local/share/Trash/info; do
		as_root install -d -m 0755 "$root/home/debian/$directory"
	done
	write_root_file "$root/home/debian/.config/user-dirs.dirs" 0644 <<'EOF'
XDG_DESKTOP_DIR="$HOME/Desktop"
XDG_DOWNLOAD_DIR="$HOME/Downloads"
XDG_TEMPLATES_DIR="$HOME/Templates"
XDG_PUBLICSHARE_DIR="$HOME/Public"
XDG_DOCUMENTS_DIR="$HOME/Documents"
XDG_MUSIC_DIR="$HOME/Music"
XDG_PICTURES_DIR="$HOME/Pictures"
XDG_VIDEOS_DIR="$HOME/Videos"
EOF
	# A benchmark or diagnostic session must stay visible and rendering, so
	# the idle lock never replaces it.
	write_root_file "$root/home/debian/.config/kscreenlockerrc" 0644 <<'EOF'
[Daemon]
Autolock=false
LockOnResume=false
Timeout=0
EOF
	as_root chown -R "$debian_uid:$debian_gid" "$root/home/debian"
	# The desktop:/ KIO worker crashes under UML. Keep Plasma's Folder View
	# containment, but point it at the Desktop directory so it uses the
	# stable local-file worker.
	containment_config=$root/usr/share/plasma/plasmoids/org.kde.desktopcontainment/contents/config/main.xml
	as_root grep -q '<default>desktop:/</default>' "$containment_config" || \
		fail "Folder View no longer defaults to desktop:/; drop its override"
	as_root sed -i \
		's#<default>desktop:/</default>#<default>file:///home/debian/Desktop</default>#' \
		"$containment_config"
	# KInfoCenter counts Mesa's CPU-only Lavapipe Vulkan device as the guest
	# GPU and labels an accelerated Virgl session "llvmpipe". The image has
	# no Vulkan render path, so it carries no software ICD.
	as_root rm -f "$root/usr/share/vulkan/icd.d/lvp_icd.json"
	# The task manager's default launchers include Discover, which needs the
	# PackageKit daemon. Pin only the default launchers whose applications
	# the image installs; Plasma runs this update script once per user,
	# right after it creates the default panel.
	taskmanager_config=$root/usr/share/plasma/plasmoids/org.kde.plasma.taskmanager/contents/config/main.xml
	default_launchers=$(as_root cat "$taskmanager_config" | awk '
		/<entry name="launchers"/ { entry = 1 }
		entry && /<default>/ {
			sub(/.*<default>/, ""); sub(/<\/default>.*/, ""); print; exit
		}')
	[ -n "$default_launchers" ] || \
		fail "no default launchers in $taskmanager_config"
	installed_launchers=
	old_ifs=$IFS
	IFS=,
	for launcher in $default_launchers; do
		case $launcher in
		applications:*)
			as_root test -e \
				"$root/usr/share/applications/${launcher#applications:}" ||
				continue
			;;
		esac
		installed_launchers=${installed_launchers:+$installed_launchers,}$launcher
	done
	IFS=$old_ifs
	write_root_file \
		"$root/usr/share/plasma/shells/org.kde.plasma.desktop/contents/updates/starter-installed-launchers.js" \
		0644 <<EOF
// Pin only the default launchers whose applications this image installs.
panels().forEach(function (panel) {
    panel.widgets().forEach(function (widget) {
        if (widget.type === "org.kde.plasma.icontasks" ||
            widget.type === "org.kde.plasma.taskmanager") {
            widget.currentConfigGroup = ["General"];
            widget.writeConfig("launchers", "$installed_launchers".split(","));
        }
    });
});
EOF
	as_root systemctl --root="$root" enable getty@tty1.service
	as_root systemctl --root="$root" set-default graphical.target
}

build_debian_plasma()
{
	printf '%s\n' "==> Building Debian 13 minimal Plasma Wayland GPT image"
	root=$roots_dir/debian-plasma
	debootstrap_root "$root"
	mount_chroot_proc "$root"
	prepare_debian_base "$root"
	install_debian_base "$root"
	configure_debian_base "$root" debian debian
	configure_plasma_user "$root"
	as_root chroot "$root" dpkg-query -W -f='${binary:Package}\t${Version}\n' | \
		sort > "$work_dir/debian-plasma.packages.tsv"
	unmount_chroot_proc
	cat "$root/etc/debian_version" > "$work_dir/debian.version"
	prune_debian "$root"
	destination=$(artifact_path debian-13-plasma-wayland.img)
	as_root python3 "$SCRIPT_DIR/make-gpt-image.py" \
		--root "$root" --output "$destination" --label debian-kde \
		--headroom-mib 256
	as_root chown "$owner_uid:$owner_gid" "$destination"
	cp "$work_dir/debian-plasma.packages.tsv" "$destination.packages.tsv"
}

# The armhf twin of debian-13-minimal. debootstrap's first stage only
# downloads and unpacks, so it runs natively. Its second stage, which runs the
# packages' maintainer scripts, is the only step that executes ARM code. It
# runs under qemu-arm in private namespaces (armhf-run.sh), with no network,
# from the packages the first stage kept in $cache. Everything else is done
# from the host.
build_debian_minimal_armhf()
{
	printf '%s\n' "==> Building Debian 13 minimal armhf GPT image"
	root=$roots_dir/debian-minimal-armhf
	cache=$work_dir/debootstrap-armhf-cache
	as_root rm -rf -- "$cache"
	mkdir -p -- "$cache"
	reset_root_directory "$root"
	as_root debootstrap --foreign --arch=armhf --variant=minbase \
		--components=main --include="$ARMHF_INCLUDE" \
		--cache-dir="$cache" \
		"$DEBIAN_SUITE" "$root" "$DEBIAN_MIRROR"
	prepare_debian_base "$root"
	as_root sh "$SCRIPT_DIR/armhf-run.sh" "$root" /usr/bin/env -i \
		PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
		HOME=/root TERM=dumb LANG=C DEBIAN_FRONTEND=noninteractive \
		/debootstrap/debootstrap --second-stage
	configure_debian_base "$root" debian-armhf root
	# passwd -d root, without running ARM code: an empty password field.
	as_root sed -i \
		"s/^root:[^:]*:[0-9]*:/root::$(($(date +%s) / 86400)):/" \
		"$root/etc/shadow"
	as_root grep -q '^root::' "$root/etc/shadow" || \
		fail "could not clear the root password"
	as_root systemctl --root="$root" set-default multi-user.target
	python3 -I "$SCRIPT_DIR/debootstrap-pins.py" --root "$root" \
		--cache "$cache" --architecture armhf --suite "$DEBIAN_SUITE" \
		--inventory "$work_dir/debian-minimal-armhf.packages.tsv" \
		--pins "$work_dir/debian-minimal-armhf.pins.json"
	cat "$root/etc/debian_version" > "$work_dir/debian.version"
	prune_debian "$root"
	as_root python3 -I "$SCRIPT_DIR/audit-elf.py" --root "$root" \
		--output "$work_dir/debian-minimal-armhf.elf.tsv" \
		--machine arm --closed
	as_root chown "$owner_uid:$owner_gid" \
		"$work_dir/debian-minimal-armhf.elf.tsv"
	# The dynamic programs are the audit's rows that name an interpreter.
	awk -F '\t' '$6 != "-" { print $1 }' \
		"$work_dir/debian-minimal-armhf.elf.tsv" | \
		as_root sh "$SCRIPT_DIR/armhf-run.sh" "$root" /bin/sh -c \
		"$(cat "$SCRIPT_DIR/armhf-verify.sh")" armhf-verify
	destination=$(artifact_path debian-13-minimal-armhf.img)
	as_root python3 "$SCRIPT_DIR/make-gpt-image.py" \
		--root "$root" --output "$destination" --label debian-min-armhf \
		--headroom-mib 96
	as_root chown "$owner_uid:$owner_gid" "$destination"
	cp "$work_dir/debian-minimal-armhf.packages.tsv" \
		"$destination.packages.tsv"
	cp "$work_dir/debian-minimal-armhf.elf.tsv" "$destination.elf.tsv"
	as_root rm -rf -- "$cache"
}

if wants boot-base-aarch64 || wants boot-base-x86_64; then
	prepare_busybox_config
fi
if wants boot-base-aarch64; then
	build_boot_base aarch64
fi
if wants boot-base-x86_64; then
	build_boot_base x86_64
fi
if wants alpine; then
	build_alpine
fi
if wants debian-13-minimal; then
	build_debian_minimal
fi
if wants debian-13-plasma-wayland; then
	build_debian_plasma
fi
if wants debian-13-minimal-armhf; then
	build_debian_minimal_armhf
fi
set --
for product in $products; do
	set -- "$@" --product "$product"
done
if wants debian-13-minimal-armhf; then
	set -- "$@" \
		--debian-armhf-pins "$work_dir/debian-minimal-armhf.pins.json" \
		--armhf-include "$ARMHF_INCLUDE" \
		--debian-mirror "$DEBIAN_MIRROR" \
		--debootstrap-version "$(debootstrap --version)" \
		--emulator-version "$(qemu-arm --version | sed 1q)"
fi
if wants boot-base-aarch64 || wants boot-base-x86_64; then
	set -- "$@" \
		--buildroot-version "$BUILDROOT_VERSION" \
		--buildroot-sha256 "$BUILDROOT_SHA256" \
		--busybox-version "$BUSYBOX_VERSION" \
		--busybox-sha256 "$BUSYBOX_SHA256"
fi
if wants alpine; then
	set -- "$@" --alpine-version "$ALPINE_VERSION" \
		--alpine-sha256 "$ALPINE_SHA256"
fi
if wants debian-13-minimal || wants debian-13-plasma-wayland || \
	wants debian-13-minimal-armhf; then
	set -- "$@" --debian-suite "$DEBIAN_SUITE" \
		--debian-version "$(cat "$work_dir/debian.version")"
fi
python3 "$SCRIPT_DIR/write-manifest.py" --output "$output_dir" "$@"
producer_publish "$output_dir" "$final_output_dir"
producer_finish
printf '%s\n' "Built starter release: $final_output_dir"
