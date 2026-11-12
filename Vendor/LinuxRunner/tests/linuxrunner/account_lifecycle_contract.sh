#!/bin/sh
# Disposable guest fixtures only; never used on the owner's rootfs.
set -eu
# The contract keeps the removed account's home, so a later run can meet a home
# left by an earlier run with the same pid: pick a name with neither an account nor a home.
n=$$
while grep -q "^lrabi_remove_$n:" /etc/passwd || [ -e "/home/lrabi_remove_$n" ]; do n=$((n+1)); done
name=lrabi_remove_$n
if grep -q "^$name:" /etc/passwd; then echo ACCOUNT_LIFECYCLE_REFUSED_EXISTING; exit 70; fi
if [ -x /usr/sbin/useradd ]; then
 /usr/sbin/useradd --create-home --shell /bin/sh -- "$name"
 remove=/usr/sbin/userdel
else
 adduser -D -h "/home/$name" -s /bin/sh "$name"
 remove=$(command -v deluser)
fi
home=/home/$name
uid=$(id -u "$name")
test "$uid" -ge 1000
test -d "$home"
test "$(stat -c %u "$home")" = "$uid"
su -l "$name" -c 'test "$(id -u)" -ne 0; test "$HOME" = "/home/$(id -un)"; printf kept > "$HOME/account-lifecycle-kept"'
test "$(cat "$home/account-lifecycle-kept")" = kept
if [ "$remove" = /usr/sbin/userdel ]; then "$remove" -- "$name"; else "$remove" "$name"; fi
if grep -q "^$name:" /etc/passwd || grep -q "^$name:" /etc/shadow; then echo ACCOUNT_LIFECYCLE_FAIL_DATABASE; exit 1; fi
test -d "$home"
test "$(cat "$home/account-lifecycle-kept")" = kept
test "$(stat -c %u "$home/account-lifecycle-kept")" = "$uid"
echo "ACCOUNT_LIFECYCLE_PASS uid=$uid nonroot_login=PASS databases=PASS home_kept=PASS numeric_owner_kept=PASS"
