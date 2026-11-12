#!/bin/sh
# Disposable Debian fixture, official hello 2.10-3 already installed for this test.
set -eu
cd /home/mobile/Downloads/.linuxrunner/gui
[ "$(dpkg-query -W -f='${Version}' hello)" = 2.10-3 ]
code=0; printf 'n\n' | /bin/sh package-manager.sh upgrade hello || code=$?
[ "$code" = 1 ]
[ "$(dpkg-query -W -f='${Version}' hello)" = 2.10-3 ]
printf 'yes\n' | /bin/sh package-manager.sh upgrade hello
[ "$(dpkg-query -W -f='${Version}' hello)" = 2.10-5 ]
[ "$(dpkg-query -W -f='${Status}' hello)" = 'install ok installed' ]
apt-get check
echo PACKAGE_UPGRADE_CONFIRMATION_PASS old_version_preserved_on_rejection actual_version_upgrade_on_acceptance
