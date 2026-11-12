#!/bin/sh
# IPA 890 on a fresh Debian: Open Desktop's package step started while Debian's
# first-start configuration (init.sh → initialize-debian.sh → dpkg
# --configure -a, then update-ca-certificates) was still running. apt then
# failed with "dpkg frontend lock was locked by /usr/bin/dpkg" and
# "certificate verify failed". Run as root on a freshly unpacked shipped image.
#   firstboot_race.sh race <gui dir>     init.sh starts, package lock taken 2 s later
#   firstboot_race.sh resume <gui dir>   first start was interrupted earlier; nothing runs it now
# After package_lock_acquire returns, Debian must be configured, the trust
# store built, and dpkg's own lock free (dpkg --configure -a succeeds).
mode=$1; G=${2:-/home/mobile/Downloads/.linuxrunner/gui}; fails=0
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; fails=$((fails+1)); fi; }
[ ! -f /var/lib/linuxrunner/initialized ] || { echo 'FIRSTBOOT_RACE_TEST SKIP image already initialized'; exit 2; }
if [ "$mode" = race ]; then
    /bin/sh /usr/lib/linuxrunner/init.sh > /dev/null 2>&1 &
    sleep 2
fi
cd "$G" || exit 2
start=$(date +%s)
sh -c '. ./package-lock.sh; package_lock_acquire' > /tmp/firstboot-race-lock.log 2>&1; rc=$?
echo "lock returned rc=$rc after $(( $(date +%s) - start ))s"; sed 's/^/  /' /tmp/firstboot-race-lock.log | tail -n 4
check "package lock taken" "$rc"
[ -f /var/lib/linuxrunner/initialized ]; check "Debian first-start configuration finished first" $?
certs=$(grep -c 'BEGIN CERTIFICATE' /etc/ssl/certs/ca-certificates.crt 2>/dev/null || echo 0)
[ "$certs" -gt 100 ]; check "CA trust store built ($certs certificates)" $?
# The device's failure: dpkg's frontend lock still held by first start's dpkg.
dpkg --configure -a > /tmp/firstboot-race-dpkg.log 2>&1; drc=$?
[ "$drc" = 0 ] && ! grep -q 'lock' /tmp/firstboot-race-dpkg.log; check "dpkg lock free afterwards (rc=$drc)" $?
grep 'lock' /tmp/firstboot-race-dpkg.log | head -n 2 | sed 's/^/  /'
[ -z "$(dpkg --audit 2>&1)" ]; check "dpkg database consistent (dpkg --audit empty)" $?
echo "FIRSTBOOT_RACE_TEST $mode $([ $fails = 0 ] && echo PASS || echo FAIL) failures=$fails"
[ $fails = 0 ]
