#!/bin/sh
# Arch Linux ARM userspace acceptance inside LinuxRunner (run as root in a fresh
# image assembled by the app installer / tools/arch/alarm-bootstrap.py):
#   first-boot initialization, identity, users, DNS, HTTPS, pacman sync and a
#   signature-verified package install with hooks, then a non-root account.
# Prints ARCH_CHECK <name> PASS|FAIL lines and ARCH_FIRSTBOOT_TEST PASS|FAIL.
fails=0
check() { name=$1; shift; if "$@" >/tmp/arch-check.out 2>&1; then echo "ARCH_CHECK $name PASS"; else echo "ARCH_CHECK $name FAIL"; sed 's/^/    /' /tmp/arch-check.out | tail -15; fails=$((fails + 1)); fi; }

check init /bin/sh -c '/usr/lib/linuxrunner/initialize-arch.sh; test -f /var/lib/linuxrunner/initialized'
check os-release grep -q 'Arch Linux ARM' /etc/os-release
check glibc /usr/lib/ld-linux-aarch64.so.1 --version
check alpm-user getent passwd alpm
check machine-id test -s /etc/machine-id
check keyring /bin/sh -c 'pacman-key --list-keys 68B3537F39A313B3E574D06777193F152BDBE6A6'
check dns getent hosts archlinuxarm.org
check https /bin/sh -c 'curl -fsS -o /dev/null https://archlinuxarm.org/'
check pacman-sync pacman -Sy --noconfirm
check pacman-install pacman -S --noconfirm --needed tree
check tree-runs tree -L 1 /usr
check pacman-query /bin/sh -c 'pacman -Qi tree | grep -q "^Validated By *: Signature"'
check useradd /bin/sh -c 'id lrarch >/dev/null 2>&1 || useradd -m -s /bin/bash lrarch'
check user-shell su - lrarch -c 'id -un; touch ~/ok; test -f ~/ok'
check sudo-present /usr/bin/sudo -V
echo "ARCH_FIRSTBOOT_TEST $([ $fails = 0 ] && echo PASS || echo FAIL) failures=$fails"
