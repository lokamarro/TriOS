#!/bin/sh
set -eu
. "$(dirname "$0")/support-common.sh"
verify_resources
if ! command -v Xvfb >/dev/null || [ ! -x ./gui-test ]; then /bin/sh ./install.sh; fi
if command -v apt-get >/dev/null; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends openbox lxpanel pcmanfm adwaita-icon-theme fonts-dejavu-core dbus-daemon dbus-bin xterm
    # Sound is optional: the desktop installs and runs without it.
    apt-get install -y --no-install-recommends pulseaudio pulseaudio-utils libasound2-plugins || echo 'DESKTOP_INSTALL: sound packages unavailable; desktop continues without sound'
elif command -v apk >/dev/null; then
    apk add --no-cache openbox xfce4-panel pcmanfm adwaita-icon-theme gdk-pixbuf-loaders font-dejavu dbus xterm
    apk add --no-cache pulseaudio pulseaudio-utils alsa-plugins-pulse || echo 'DESKTOP_INSTALL: sound packages unavailable; desktop continues without sound'
elif command -v pacman >/dev/null; then
    exec /bin/sh ./desktop-profile.sh repair
else support_fail DESKTOP_INSTALL package-manager-unsupported; fi
# D-Bus has a real persistent guest machine identity; never replace an existing one.
mkdir -p /var/lib/dbus
dbus-uuidgen --ensure
for tool in openbox pcmanfm xterm gcc; do command -v "$tool" >/dev/null || support_fail DESKTOP_INSTALL "missing:$tool"; done
command -v lxpanel >/dev/null || command -v xfce4-panel >/dev/null || support_fail DESKTOP_INSTALL panel-missing
key=$(sha256sum desktop-probe.c | awk '{print $1}')
if [ ! -x desktop-probe ] || [ "$(cat desktop-probe.key 2>/dev/null || true)" != "$key" ]; then
 gcc desktop-probe.c -O2 -Wall -Wextra -lX11 -o desktop-probe.new
 verify_elf DESKTOP_INSTALL "$SUPPORT_DIR/desktop-probe.new"
 chmod 755 desktop-probe.new; mv desktop-probe.new desktop-probe
 echo "$key" > desktop-probe.key
fi
# Sound relay (only when the sound packages installed).
if command -v parec >/dev/null; then
 key=$(sha256sum audio-relay.c | awk '{print $1}')
 if [ ! -x audio-relay ] || [ "$(cat audio-relay.key 2>/dev/null || true)" != "$key" ]; then
  if gcc audio-relay.c -O2 -Wall -Wextra -o audio-relay.new && verify_elf DESKTOP_INSTALL "$SUPPORT_DIR/audio-relay.new"; then
   chmod 755 audio-relay.new; mv audio-relay.new audio-relay; echo "$key" > audio-relay.key
  else rm -f audio-relay.new; echo 'DESKTOP_INSTALL: sound relay build failed; desktop continues without sound'; fi
 fi
fi
/bin/sh ./desktop-configure.sh
echo 'DESKTOP_INSTALL: packages=verified configuration=preserved probe=ARM64 result=PASS'
