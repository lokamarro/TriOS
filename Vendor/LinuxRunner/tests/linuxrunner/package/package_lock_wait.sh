#!/bin/sh
# Package-operation serialization (IPA 890: Open Desktop's repair collided with
# Debian's first-start dpkg). Run as root in a Debian environment whose support
# folder is $1 (default /home/mobile/Downloads/.linuxrunner/gui).
#  1. package_system_ready waits while another package process runs.
#  2. A second installer-run.sh during an active one ends with an
#     install-result (PACKAGE_MANAGER_BUSY), so the app never waits forever.
G=${1:-/home/mobile/Downloads/.linuxrunner/gui}; fails=0
cd "$G" || exit 2
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; fails=$((fails+1)); fi; }
# 1: a process named dpkg runs for 8 s
mkdir -p /tmp/lr-lock-test; cp "$(command -v sleep)" /tmp/lr-lock-test/dpkg
/tmp/lr-lock-test/dpkg 8 & fake=$!
sleep 1; start=$(date +%s)
out=$(sh -c '. ./package-lock.sh; package_system_ready' 2>&1); rc=$?
waited=$(( $(date +%s) - start ))
wait "$fake" 2>/dev/null
[ "$rc" = 0 ] && [ "$waited" -ge 6 ] && printf '%s' "$out" | grep -q 'Waiting for another package installation'; check "waits for a running package process (waited ${waited}s)" $?
# 2: hold LinuxRunner's lock with a long installer, start a second one
# The holder's command line names installer-run.sh, as a real installer's does.
cat > /tmp/lr-lock-test/installer-run.sh-holder.sh <<EOS
cd "$G"; . ./package-lock.sh; package_lock_acquire >/dev/null 2>&1; sleep 12
EOS
sh /tmp/lr-lock-test/installer-run.sh-holder.sh & holder=$!
sleep 2
printf "holder-result\n" > install-result
printf "holder-log\n" > desktop-profile.sh.log
sh ./installer-run.sh desktop-profile.sh repair > /tmp/lock-test-second.log 2>&1; second=$?
result=$(cat install-result 2>/dev/null)
wait "$holder" 2>/dev/null; rm -rf /tmp/lr-lock-test
[ "$second" = 75 ] && [ "$result" = holder-result ] && grep -qx holder-log desktop-profile.sh.log && grep -q 'category=PACKAGE_MANAGER_BUSY' /tmp/lock-test-second.log; check "second installer reports BUSY without changing active output (exit=$second result=${result:-none})" $?
echo "PACKAGE_LOCK_WAIT_TEST $([ $fails = 0 ] && echo PASS || echo FAIL) failures=$fails"
[ $fails = 0 ]
