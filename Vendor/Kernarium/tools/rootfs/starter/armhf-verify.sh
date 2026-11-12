#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Prove that an armhf root's userspace is complete, from inside the root.
# armhf-run.sh runs it under qemu-arm with the ELF audit's dynamic programs,
# one path relative to the root per line, on standard input.
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH

status=0

# A clean package database: every package installed and configured.
problems=$(dpkg --audit 2>&1)
if [ -n "$problems" ]; then
	printf '%s\n' "dpkg --audit reports:" "$problems"
	status=1
fi

# The programs that make the image boot and log in start.
for program in /usr/lib/systemd/systemd /usr/lib/systemd/systemd-networkd \
	/usr/lib/systemd/systemd-resolved /usr/bin/systemctl /usr/bin/udevadm \
	/sbin/agetty /usr/bin/dbus-daemon /usr/bin/kmod /usr/bin/mount \
	/usr/bin/bash /usr/bin/dpkg-query; do
	if ! "$program" --version >/dev/null 2>&1; then
		echo "does not run: $program"
		status=1
	fi
done
dash -c : || {
	echo "does not run: dash"
	status=1
}

# The real loader resolves every dynamic program's libraries.
checked=0
while IFS= read -r file; do
	if ! listing=$(/lib/ld-linux-armhf.so.3 --list "/$file" 2>&1); then
		echo "cannot list /$file: $listing"
		status=1
		continue
	fi
	case $listing in
	*"not found"*)
		echo "unresolved libraries for /$file:"
		echo "$listing"
		status=1
		;;
	esac
	checked=$((checked + 1))
done
echo "armhf userspace: $checked dynamic programs resolve their libraries"
exit "$status"
