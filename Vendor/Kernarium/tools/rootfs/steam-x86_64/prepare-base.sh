#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Network preparation, called by prepare-inputs.py only.
set -euo pipefail
inputs=$1
root=$inputs/base-root
# The recorded builder package list decides whether a prepared builder still
# matches this policy, so a builder missing a newly required development
# package is rebuilt instead of silently reused.
if [ -f "$inputs/base.tar.zst" ] && [ -f "$inputs/builder.tar.zst" ] &&
	grep -q '^libva-dev	' "$inputs/builder.packages.tsv" 2>/dev/null; then
	exit 0
fi
if [ ! -d "$root" ]; then
	mkdir "$root"
	printf '%s\n' "$root" > "$inputs/.steam-base-owned"
	sudo -n debootstrap --arch=amd64 --variant=minbase \
		--components=main,contrib,non-free,non-free-firmware \
		trixie "$root" http://deb.debian.org/debian
fi
test "$(cat "$inputs/.steam-base-owned")" = "$root"
sudo -n tee "$root/etc/apt/sources.list" >/dev/null <<'EOF'
deb http://deb.debian.org/debian trixie main contrib non-free non-free-firmware
deb http://deb.debian.org/debian trixie-updates main contrib non-free non-free-firmware
deb http://security.debian.org/debian-security trixie-security main contrib non-free non-free-firmware
deb http://deb.debian.org/debian trixie-backports main contrib non-free non-free-firmware
EOF
sudo -n tee "$root/usr/sbin/policy-rc.d" >/dev/null <<'EOF'
#!/bin/sh
exit 101
EOF
sudo -n chmod +x "$root/usr/sbin/policy-rc.d"
sudo -n cp -L /etc/resolv.conf "$root/etc/resolv.conf"
sudo -n chroot "$root" apt-get update
sudo -n chroot "$root" env DEBIAN_FRONTEND=noninteractive \
	apt-get install -y --no-install-recommends \
	systemd-sysv systemd-resolved udev kmod dbus dbus-user-session \
	polkitd pkexec ca-certificates sudo procps lsof util-linux e2fsprogs iproute2 \
	iputils-ping netbase locales python3 curl xz-utils zstd \
	xwayland xauth xvfb x11-xserver-utils x11-utils fonts-dejavu-core \
	libgl1-mesa-dri mesa-vulkan-drivers vulkan-tools mesa-utils \
	libasound2t64 libcups2t64 libdbus-1-3 libfontconfig1 libfreetype6 \
	libgl1 libgnutls30t64 libpulse0 libudev1 libunwind8 \
	libx11-6 libxcomposite1 libxcursor1 libxext6 libxfixes3 libxi6 \
	libxinerama1 libxrandr2 libxrender1 libxxf86vm1 libxcb-keysyms1 \
	libwayland-client0 libwayland-egl1 libxkbcommon0 \
	libavcodec61 libavformat61 libavutil59 \
	pipewire pipewire-pulse wireplumber libnss3 libnspr4 \
	libatomic1 libcurl4t64 libgtk-3-0t64 libgtk2.0-0t64 libsdl3-0 libsm6 libice6 \
	libxtst6 libgdk-pixbuf-2.0-0 libva2 libvdpau1 libopenal1 libnm0
sudo -n chroot "$root" env DEBIAN_FRONTEND=noninteractive \
	apt-get install -y --no-install-recommends -t trixie-backports gamescope
test -z "$(sudo -n chroot "$root" dpkg --audit)"
test -z "$(sudo -n chroot "$root" dpkg --print-foreign-architectures)"
sudo -n chroot "$root" dpkg-query -W -f='${binary:Package}\t${Version}\n' | sort > "$inputs/base.packages.tsv"
sudo -n chroot "$root" apt-get clean
if [ ! -f "$inputs/base.tar.zst" ]; then
	sudo -n tar --zstd -cf "$inputs/base.tar.zst.partial" -C "$root" .
	sudo -n chown "$(id -u):$(id -g)" "$inputs/base.tar.zst.partial"
	mv "$inputs/base.tar.zst.partial" "$inputs/base.tar.zst"
fi
# The splash renderer is compiled offline against exactly the libraries the
# image ships, so the builder is the runtime closure plus its development
# packages. It never becomes part of the image.
sudo -n chroot "$root" env DEBIAN_FRONTEND=noninteractive \
	apt-get install -y --no-install-recommends \
	cmake ninja-build g++ pkgconf \
	libcairo2-dev libdrm-dev libgbm-dev libegl-dev libgles-dev libx11-dev \
	libva-dev
test -z "$(sudo -n chroot "$root" dpkg --audit)"
sudo -n chroot "$root" dpkg-query -W -f='${binary:Package}\t${Version}\n' | sort > "$inputs/builder.packages.tsv"
sudo -n chroot "$root" apt-get clean
sudo -n tar --zstd -cf "$inputs/builder.tar.zst.partial" -C "$root" .
sudo -n chown "$(id -u):$(id -g)" "$inputs/builder.tar.zst.partial"
mv "$inputs/builder.tar.zst.partial" "$inputs/builder.tar.zst"
