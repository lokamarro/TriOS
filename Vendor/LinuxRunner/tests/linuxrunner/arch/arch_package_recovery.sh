#!/bin/sh
# Run only as root in a disposable LinuxRunner Arch fixture, with no other test
# using it. Exercises real guest /proc, fd links, flock and libalpm validation.
set -eu
G=${1:-/home/mobile/Downloads/.linuxrunner/gui}
. "$G/package-lock.sh"
work=$(mktemp -d /tmp/lr-arch-lock.XXXXXX)
trap 'rm -rf "$work"' EXIT
[ ! -e /var/lib/pacman/db.lck ] || { echo 'FAIL fixture already has a pacman lock'; exit 2; }
command -v flock >/dev/null

# Ownership applies across deployment directories, with atomic publication.
cat > "$work/hold.sh" <<'HOLDER'
. "$1/package-lock.sh"
package_lock_acquire defer-ready || exit $?
touch "$2/held"
sleep 5
HOLDER
sh "$work/hold.sh" "$G" "$work" & holder=$!
i=0; until [ -f "$work/held" ]; do i=$((i+1)); [ "$i" -lt 100 ]; sleep .1; done
code=0
(cd /tmp; sh -c '. "$1/package-lock.sh"; package_lock_acquire defer-ready' lock "$G") > "$work/duplicate.log" 2>&1 || code=$?
[ "$code" = 75 ]; grep -q PACKAGE_MANAGER_BUSY "$work/duplicate.log"
wait "$holder"
echo 'ARCH_LOCK cross-directory-serialization PASS'

# If its parent is killed, a live worker retains the inherited descriptor.
rm "$work/held"
cat > "$work/worker.sh" <<'WORKER'
. "$1/package-lock.sh"
package_lock_acquire defer-ready || exit $?
sleep 5 &
touch "$2/held"
wait
WORKER
sh "$work/worker.sh" "$G" "$work" & holder=$!
i=0; until [ -f "$work/held" ]; do i=$((i+1)); [ "$i" -lt 100 ]; sleep .1; done
kill -9 "$holder"; wait "$holder" 2>/dev/null || true
code=0; (package_lock_acquire defer-ready) > "$work/orphan.log" 2>&1 || code=$?
[ "$code" = 75 ]
sleep 6
(package_lock_acquire defer-ready)
echo 'ARCH_LOCK interrupted-parent-preserves-worker PASS'

# Pacman is recognized by argv[0], while grep/echo mentioning it are ignored.
cp "$(command -v sleep)" "$work/pacman"
"$work/pacman" 5 & holder=$!
sleep .2
package_processes | grep -q "^$holder "
start=$(date +%s)
package_system_ready > "$work/wait.log" 2>&1
wait "$holder"
[ $(( $(date +%s) - start )) -ge 3 ]
grep -q 'Waiting for another package installation' "$work/wait.log"
echo 'ARCH_LOCK active-pacman-wait PASS'

# A differently named client holding libalpm's descriptor prevents recovery.
: > /var/lib/pacman/db.lck
sleep 5 6<>/var/lib/pacman/db.lck & holder=$!
sleep .2
code=0; pacman_database_ready > "$work/descriptor.log" 2>&1 || code=$?
[ "$code" = 75 ]; [ -f /var/lib/pacman/db.lck ]
grep -q 'fd=6' "$work/descriptor.log"
wait "$holder"
echo 'ARCH_LOCK active-descriptor-preserved PASS'

# After the owner exits, recovery logs the identity and checks the real DB.
(package_lock_acquire) > "$work/recovery.log" 2>&1
[ ! -e /var/lib/pacman/db.lck ]
grep -q 'Removed an abandoned pacman lock' "$work/recovery.log"
grep -q 'recovery=stale-lock' /var/log/linuxrunner-package-recovery.log
pacman -Dk
echo 'ARCH_LOCK verified-stale-recovery PASS'

# Symlinks are never followed, even if their target looks like a lock.
ln -s "$work/important" /var/lib/pacman/db.lck
printf 'keep\n' > "$work/important"
code=0; pacman_database_ready > "$work/unsafe.log" 2>&1 || code=$?
[ "$code" = 1 ]; [ -L /var/lib/pacman/db.lck ]; grep -qx keep "$work/important"
rm /var/lib/pacman/db.lck
echo 'ARCH_LOCK unsafe-lock-refused PASS'

# Interrupt real libalpm after it exclusively creates db.lck, before a response
# from an intentionally unreachable TEST-NET server. No package writes occur.
sed 's@^Include = /etc/pacman.d/mirrorlist$@Server = http://192.0.2.1@' /etc/pacman.conf > "$work/pacman.conf"
pacman --config "$work/pacman.conf" -Sy --noconfirm > "$work/interrupted.log" 2>&1 & holder=$!
i=0; until [ -f /var/lib/pacman/db.lck ]; do
    kill -0 "$holder"; i=$((i+1)); [ "$i" -lt 200 ]; sleep .01
done
kill -9 "$holder"; wait "$holder" 2>/dev/null || true
[ -f /var/lib/pacman/db.lck ]
echo "ARCH_LOCK real-pacman-interrupted pid=$holder lock=present"
(package_lock_acquire) > "$work/interrupted-recovery.log" 2>&1
[ ! -e /var/lib/pacman/db.lck ]
cat "$work/interrupted-recovery.log"
echo 'ARCH_LOCK real-pacman-interrupted-recovery PASS'

# Historical log entries alone cannot prove current corruption. An obsolete
# marker is cleared only after current DB, package files and GCC ownership pass.
cp /var/log/pacman.log "$work/pacman.log"
printf '[test] [ALPM] transaction started\n' >> /var/log/pacman.log
printf 'interrupted-package-write\n' > /var/lib/linuxrunner/pacman-recovery-required
: > /var/lib/pacman/db.lck
(package_lock_acquire) > "$work/obsolete-marker.log" 2>&1
[ ! -e /var/lib/pacman/db.lck ]
[ ! -e /var/lib/linuxrunner/pacman-recovery-required ]
grep -q 'ownership checks passed' "$work/obsolete-marker.log"
pacman -Dk
cp "$work/pacman.log" /var/log/pacman.log
echo 'ARCH_LOCK obsolete-recovery-marker-cleared PASS'
echo 'ARCH_PACKAGE_RECOVERY_TEST PASS'
