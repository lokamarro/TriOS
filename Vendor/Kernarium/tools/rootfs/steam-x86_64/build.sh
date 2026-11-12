#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Seal a gaming-only pure x86-64 Steam root from prepared inputs.
set -euo pipefail
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository=$(CDPATH= cd -- "$here/../../.." && pwd)
fail()
{
	printf '%s\n' "tools/rootfs/steam-x86_64/build.sh: $*" >&2
	exit 1
}
usage()
{
	printf '%s\n' 'Usage: build.sh --inputs PATH --work ABSENT_PATH --output ABSENT_PATH.erofs [--vulkan-root PATH --vulkan-receipt PATH]'
}
. "$here/../producer.sh"
inputs= work_dir= output= vulkan_root= vulkan_receipt=
while [ "$#" -gt 0 ]; do
	case "$1" in
	--inputs) inputs=$2; shift 2 ;;
	--work) work_dir=$2; shift 2 ;;
	--output) output=$2; shift 2 ;;
	--vulkan-root) vulkan_root=$2; shift 2 ;;
	--vulkan-receipt) vulkan_receipt=$2; shift 2 ;;
	-h|--help) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
done
[ -z "$vulkan_root$vulkan_receipt" ] || { [ -d "$vulkan_root" ] && [ -f "$vulkan_receipt" ]; }
case "$inputs:$work_dir:$output" in /*:/*:/*.erofs) ;; *) usage >&2; exit 2 ;; esac
case "$(basename "$output")" in steam-x86_64-*.erofs) ;; *) exit 2 ;; esac
producer_require_host x86_64
producer_require_absent "work path" "$work_dir"
producer_require_absent output "$output" "$output.sha256" "$output.packages.tsv" \
	"$output.binaries.tsv" "$output.provenance.json"
inputs=$(realpath "$inputs")
python3 -B "$here/prepare-inputs.py" --root "$inputs" --verify
mkdir -p "$work_dir/root" "$(dirname "$output")"
work_dir=$(realpath "$work_dir")
root=$work_dir/root
sudo -n tar -xf "$inputs/base.tar.zst" -C "$root"
sudo -n cp -a "$here/overlay/." "$root/"
sudo -n chroot "$root" useradd -m -s /bin/bash uml
sudo -n chroot "$root" passwd -d uml
for group in sudo video audio input render; do
	if sudo -n chroot "$root" getent group "$group" >/dev/null; then
		sudo -n chroot "$root" usermod -a -G "$group" uml
	fi
done
sudo -n python3 -B "$here/assemble-steam.py" --inputs "$inputs" \
	--steam "$root/home/uml/.local/share/Steam"
sudo -n mkdir -p "$root/home/uml/.steam" "$root/home/uml/.local/share/Steam/compatibilitytools.d"
for name in steam root; do
	sudo -n ln -s ../.local/share/Steam "$root/home/uml/.steam/$name"
done
# Valve's client publishes its redistributable SDK through these names.
sudo -n ln -s ../.local/share/Steam/linux32 "$root/home/uml/.steam/sdk32"
sudo -n ln -s ../.local/share/Steam/linux64 "$root/home/uml/.steam/sdk64"
sudo -n chown -R 1000:1000 "$root/home/uml"

write_root_file "$root/etc/hostname" 0644 <<'EOF'
uml-steam-x86
EOF
write_root_file "$root/etc/hosts" 0644 <<'EOF'
127.0.0.1 localhost
127.0.1.1 uml-steam-x86
::1 localhost ip6-localhost ip6-loopback
EOF
write_root_file "$root/etc/fstab" 0644 <<'EOF'
/dev/ubda / erofs ro 0 0
/dev/ubdb1 /userdata ext4 noatime 0 0
/userdata/home /home none bind 0 0
/userdata/var /var none bind 0 0
/userdata/root /root none bind 0 0
none /tmp tmpfs mode=1777,nosuid,nodev 0 0
EOF
write_root_file "$root/etc/sudoers.d/uml" 0440 <<'EOF'
uml ALL=(ALL:ALL) NOPASSWD: ALL
EOF
write_root_file "$root/etc/systemd/network/20-wired.network" 0644 <<'EOF'
[Match]
Name=eth* en*
[Network]
DHCP=yes
IPv6AcceptRA=yes
EOF
write_root_file "$root/etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf" 0644 <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin uml --noclear %I $TERM
EOF
write_root_file "$root/etc/locale.gen" 0644 <<'EOF'
en_US.UTF-8 UTF-8
EOF
write_root_file "$root/etc/default/locale" 0644 <<'EOF'
LANG=en_US.UTF-8
EOF
sudo -n chroot "$root" locale-gen en_US.UTF-8
sudo -n systemctl --root="$root" enable systemd-networkd systemd-resolved steam-gaming
sudo -n systemctl --root="$root" set-default graphical.target
sudo -n ln -sfn /run/systemd/resolve/stub-resolv.conf "$root/etc/resolv.conf"
sudo -n rm -f "$root/etc/machine-id" "$root/var/lib/dbus/machine-id"
sudo -n ln -s /var/lib/dbus/machine-id "$root/etc/machine-id"
sudo -n systemctl --root="$root" mask getty@tty1.service

# The splash renderer is compiled offline in the prepared builder root, whose
# libraries are exactly the ones this image ships.
builder=$work_dir/builder
mkdir -p "$builder"
sudo -n tar -xf "$inputs/builder.tar.zst" -C "$builder"
sudo -n cp -a "$here/splash" "$builder/splash"
sudo -n chroot "$builder" env CCACHE_DISABLE=1 sh -eu -c \
	'cd /splash; cmake --preset steam-sdk -B /splash-build; cmake --build /splash-build'
sudo -n install -m 0755 "$builder/splash-build/kernarium-screen" "$root/usr/local/libexec/kernarium-screen"
sudo -n chroot "$root" ldd /usr/local/libexec/kernarium-screen > "$work_dir/kernarium-screen.ldd"
! grep -q 'not found' "$work_dir/kernarium-screen.ldd"

# The VA-API backend over the Virtio video stateful decoder is compiled in the
# same builder root, against the libva and libdrm the image ships, and its own
# tests run there before the module is installed.
sudo -n cp -a "$repository/tools/video/va-v4l2" "$builder/va-v4l2"
sudo -n rm -rf "$builder/va-v4l2/build"
sudo -n chroot "$builder" env CCACHE_DISABLE=1 sh -eu -c \
	'cd /va-v4l2; cmake --preset guest -B /va-v4l2-build; cmake --build /va-v4l2-build;
	 /va-v4l2-build/uml_va_v4l2_tests'
sudo -n install -D -m 0644 "$builder/va-v4l2-build/uml_drv_video.so" \
	"$root/usr/lib/x86_64-linux-gnu/dri/uml_drv_video.so"
sudo -n chroot "$root" ldd /usr/lib/x86_64-linux-gnu/dri/uml_drv_video.so > "$work_dir/uml_drv_video.ldd"
! grep -q 'not found' "$work_dir/uml_drv_video.ldd"
sudo -n rm -rf "$builder"

if [ -n "$vulkan_root" ]; then
	# The maintained Vulkan guest owns its own architecture-aware importer.
	sudo -n python3 -B "$repository/tools/rootfs/vulkan/install-runtime.py" \
		--architecture amd64 --source "$vulkan_root" \
		--receipt "$vulkan_receipt" --root "$root"
fi
# Keep versioned package and client seeds in the immutable image. Only the
# image-owned init copies them to a new data partition, never on an update.
sudo -n install -d "$root/usr/share/uml-steam/home" "$root/userdata"
sudo -n mv "$root/home/uml" "$root/usr/share/uml-steam/home/uml"
sudo -n mv "$root/var/lib/dpkg" "$root/usr/share/uml-steam/dpkg"
sudo -n ln -s /usr/share/uml-steam/dpkg "$root/var/lib/dpkg"
sudo -n cp -a "$root/var" "$root/usr/share/uml-steam/var"
sudo -n chmod 0755 "$root/usr/local/libexec/kernarium-init"
"$here/seal-image.sh" "$inputs" "$work_dir" "$output"
