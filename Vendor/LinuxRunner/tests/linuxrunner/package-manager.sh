#!/bin/sh
# Run only in an isolated guest fixture with the canonical support deployed.
set -eu
fixture_package=${1:-xterm}
cd /home/mobile/Downloads/.linuxrunner/gui
for name in 'x;id' '-y' '../bad' 'two names'; do
 code=0; /bin/sh package-manager.sh install "$name" || code=$?
 [ "$code" = 2 ] || exit 1
done
code=0; /bin/sh package-manager.sh upgrade || code=$?; [ "$code" = 2 ]
# A real second managed operation must refuse a live owner.
held=$(mktemp -u /tmp/lr-package-held.XXXXXX)
/bin/sh -c '. ./package-lock.sh; package_lock_acquire; touch "$1"; sleep 3' package-manager.sh "$held" & locker=$!
while [ ! -f "$held" ]; do kill -0 "$locker"; sleep .05; done
code=0; /bin/sh package-manager.sh installed || code=$?; [ "$code" = 75 ]
wait "$locker"; rm "$held"
# Recover only an abandoned managed lock, not the distro package database.
if ! command -v flock >/dev/null; then mkdir /var/lib/linuxrunner/package-operation.lock.d; echo 999999 > /var/lib/linuxrunner/package-operation.lock.d/pid; fi
/bin/sh package-manager.sh installed
[ "$(cat package-result)" = 0 ] && [ -s package-state.tsv ] && [ ! -d /var/lib/linuxrunner/package-operation.lock.d ]
/bin/sh package-manager.sh search "$fixture_package"
[ "$(cat package-result)" = 0 ] && grep -q "$fixture_package" package-state.tsv
/bin/sh package-manager.sh details "$fixture_package"
[ "$(cat package-result)" = 0 ] && [ -s package-details.txt ]
/bin/sh package-manager.sh dependencies
[ "$(cat package-result)" = 0 ]
echo 'PACKAGE_MANAGER_PASS invalid argv, live lock, stale lock, real installed/search/details/dependency state'
