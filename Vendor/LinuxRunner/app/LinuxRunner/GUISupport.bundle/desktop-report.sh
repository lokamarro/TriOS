#!/bin/sh
# Guest-side state for the Open Desktop diagnostic report, run by the app as
# root: desktop-related processes and their states, Debian first start, X
# display sockets, package processes. Prints no passwords, cookies, environment
# variables or user file contents (command lines are cut to 100 characters and
# only desktop/package programs are listed).
cd "$(dirname "$0")" || exit 2
echo "== guest processes (desktop, display, package)"
for p in /proc/[0-9]*; do
    cmd=$( { tr '\000' ' ' < "$p/cmdline"; } 2>/dev/null | cut -c1-100)
    [ -n "$cmd" ] || continue
    case "$cmd" in
        *Xvfb*|*x11vnc*|*openbox*|*lxpanel*|*lxqt-panel*|*pcmanfm*|*xfce4*|*xfconfd*|*dbus-daemon*|*pulseaudio*|*session.sh*|*desktop-components.sh*|*desktop-probe*|*focus-manager*|*window-observer*|*desktop-profile.sh*|*installer-run.sh*|*initialize-debian.sh*|*initialize-arch.sh*|*pacman-recover.sh*|*/pacman*|dpkg*|*/dpkg\ *|apt*|*/apt-get*|*/usr/lib/apt/methods/*) ;;
        *) continue;;
    esac
    state=$(sed -n 's/^State:[[:space:]]*//p' "$p/status" 2>/dev/null)
    uid=$(sed -n 's/^Uid:[[:space:]]*\([0-9]*\).*/\1/p' "$p/status" 2>/dev/null)
    ppid=$(sed -n 's/^PPid:[[:space:]]*//p' "$p/status" 2>/dev/null)
    echo "${p#/proc/} ppid=$ppid uid=$uid state=$state $cmd"
done
echo "== debian first start"
if [ -e /usr/lib/linuxrunner/initialize-debian.sh ]; then
    ls /var/lib/linuxrunner 2>&1 | tr '\n' ' '; echo
    echo "ca-certificates=$(grep -c 'BEGIN CERTIFICATE' /etc/ssl/certs/ca-certificates.crt 2>/dev/null || echo 0)"
    echo "dpkg-audit-lines=$(dpkg --audit 2>/dev/null | wc -l)"
else echo "not debian"; fi
if command -v pacman >/dev/null 2>&1; then
    echo "== Arch package health (current)"
    pacman -Dk 2>&1
    if [ -e /usr/bin/gcc ]; then pacman -Q gcc 2>&1; pacman -Qo /usr/bin/gcc 2>&1; fi
    echo "== package recovery history (last 40 records)"
    tail -n 40 /var/log/linuxrunner-package-recovery.log 2>/dev/null
    if [ -f /var/lib/linuxrunner/pacman-recovery-required ] && [ ! -L /var/lib/linuxrunner/pacman-recovery-required ]; then
        echo "== pending package repair"
        head -c 2048 /var/lib/linuxrunner/pacman-recovery-required
    fi
fi
echo "== x display sockets"
ls -la /tmp/.X11-unix 2>&1 | tail -n +2
echo "== time"
date -u '+%Y-%m-%dT%H:%M:%SZ'
echo "== end"
