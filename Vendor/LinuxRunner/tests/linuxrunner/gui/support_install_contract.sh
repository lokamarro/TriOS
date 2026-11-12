#!/bin/sh
# Destructive integration test: invoke only in a disposable LinuxRunner rootfs.
# The normal user-facing installers and preflight scripts are exercised unchanged.
set -eu
cd /home/mobile/Downloads/.linuxrunner/gui
backup() { mv browser browser.contract-saved; }
restore() { [ ! -f browser.contract-saved ] || { rm -f browser; mv browser.contract-saved browser; }; }
cleanup() { restore; [ ! -f browser-support.meta.saved ] || mv browser-support.meta.saved browser-support.meta; [ ! -f browser.cpp.saved ] || mv browser.cpp.saved browser.cpp; [ ! -f browser ] || chmod 755 browser; }
trap 'cleanup' EXIT
check() {
 expected=$1;shift
 if /bin/sh ./browser-preflight.sh chromium > contract-error.log 2>&1; then echo unexpected-preflight-success;exit 1;fi
 grep -q "$expected" contract-error.log || { cat contract-error.log;exit 1; }
 echo "SUPPORT_CONTRACT: $expected PASS"
}
/bin/sh ./browser-preflight.sh chromium
backup
check browser-binary-missing
/bin/sh ./browser-preflight.sh firefox
echo 'SUPPORT_CONTRACT: Firefox independent of Chromium PASS'
printf 'not an ELF' > browser;chmod 755 browser
check invalid-linux-elf
rm browser;restore
chmod 644 browser
check executable-permission-missing
chmod 755 browser
cp browser-support.meta browser-support.meta.saved
sed 's/^version=.*/version=invalid/' browser-support.meta.saved > browser-support.meta
check support-version-mismatch
mv browser-support.meta.saved browser-support.meta
mv browser.cpp browser.cpp.saved
check browser-source-missing
mv browser.cpp.saved browser.cpp
sha256sum browser > contract-before.sha
stat -c %Y browser > contract-before.mtime
/bin/sh ./installer-run.sh install.sh
/bin/sh ./installer-run.sh install-browser.sh
grep -q build_required=no install-browser.sh.log
sha256sum -c contract-before.sha
[ "$(stat -c %Y browser)" = "$(cat contract-before.mtime)" ]
echo 'SUPPORT_CONTRACT: idempotent install, no recompile, binary unchanged PASS'
# Recover an interrupted publisher after its grace period.
mkdir .install-lock
touch -t 202601010000 .install-lock
/bin/sh ./installer-run.sh install.sh
grep -q 'SUPPORT_INSTALL: script=install.sh exit=0' install.sh.log
[ ! -d .install-lock ]
echo 'SUPPORT_CONTRACT: abandoned unpublished lock recovered PASS'
/bin/sh ./browser-preflight.sh chromium
/bin/sh ./browser-preflight.sh firefox
echo SUPPORT_CONTRACT_ALL_PASS
