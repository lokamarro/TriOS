#!/bin/sh
# Destructive to the test package only: run in a disposable Debian fixture.
set -eu
cd /home/mobile/Downloads/.linuxrunner/gui
command -v apt-get >/dev/null
/bin/sh package-manager.sh install hello
code=0; printf 'n\n' | /bin/sh package-manager.sh remove hello || code=$?
[ "$code" = 1 ]
dpkg-query -W -f='${Status}\n' hello | grep -q 'install ok installed'
printf 'y\n' | /bin/sh package-manager.sh remove hello
! dpkg-query -W -f='${Status}\n' hello 2>/dev/null | grep -q 'install ok installed'
echo PACKAGE_REMOVE_CONFIRMATION_PASS
