#!/bin/bash
# Destructive fault injection ONLY in an explicitly disposable Arch fixture.
# All repairs use real pacman, its signed cache and LinuxRunner's actual helper.
set -euo pipefail
[[ ${LR_DISPOSABLE_FIXTURE:-} == 1 ]] || { echo 'Set LR_DISPOSABLE_FIXTURE=1 only for a disposable fixture'; exit 2; }
G=/home/mobile/Downloads/.linuxrunner/gui
marker=/var/lib/linuxrunner/pacman-recovery-required
work=$(mktemp -d /tmp/lr-pacman-regression.XXXXXXXX)
version=$(pacman -Q gcc); version=${version#* }
entry=/var/lib/pacman/local/gcc-$version
archive=/var/cache/pacman/pkg/gcc-$version-aarch64.pkg.tar.xz
[[ -f $archive && -f $archive.sig && -d $entry ]]
pacman -Dk
if [[ ${LR_PACMAN_STATE_ONLY:-0} != 1 ]]; then
# Reproduce the original mtree-only entry and a legacy persistent marker.
mv "$entry/desc" "$work/desc"; mv "$entry/files" "$work/files"
printf 'database-check-failed\n' > "$marker"
if pacman -Dk > "$work/corrupt.log" 2>&1; then exit 1; fi
sh "$G/installer-run.sh" desktop-profile.sh repair > "$work/desktop-resume.log" 2>&1 || { cat "$work/desktop-resume.log"; exit 1; }
grep -q 'PROFILE: result=PASS' "$work/desktop-resume.log"
pacman -Dk; pacman -Q gcc; [[ $(pacman -Qqo /usr/bin/gcc) == gcc ]]
[[ ! -e $marker ]]; gcc --version
echo 'PACMAN_RECOVERY mtree-only-metadata-and-desktop-resume PASS'

# Reproduce the phone's newer case: clean Dk, entire package record absent,
# existing unowned executable. A clean Dk must NOT conceal this condition.
mv "$entry" "$work/original-complete-entry"
printf 'interrupted-package-write\n' > "$marker"
pacman -Dk
if pacman -Q gcc >/dev/null 2>&1; then exit 1; fi
if pacman -Qo /usr/bin/gcc >/dev/null 2>&1; then exit 1; fi
/bin/bash "$G/pacman-recover.sh" > "$work/orphan-repair.log" 2>&1 || { cat "$work/orphan-repair.log"; exit 1; }
pacman -Dk; pacman -Q gcc; [[ $(pacman -Qqo /usr/bin/gcc) == gcc ]]
[[ ! -e $marker ]]; gcc --version >/dev/null
echo 'PACMAN_RECOVERY clean-database-orphaned-GCC-restored PASS'

# A modified unowned executable is not safe to reclaim automatically.
mv "$entry" "$work/entry-for-conflict"
cp -p /usr/bin/gcc "$work/gcc-original"
printf '\nmodified-by-regression\n' >> /usr/bin/gcc
printf 'database-check-failed\n' > "$marker"
if /bin/bash "$G/pacman-recover.sh" > "$work/conflict.log" 2>&1; then exit 1; fi
grep -q 'existing file differs from signed gcc payload' "$work/conflict.log"
[[ ! -e $entry && -e $marker ]]
cp -p "$work/gcc-original" /usr/bin/gcc
mv "$work/entry-for-conflict" "$entry"
echo 'PACMAN_RECOVERY changed-unowned-file-refused PASS'

# Missing detached signatures may never weaken Required TrustedOnly.
mv "$entry" "$work/entry-for-signature"
mv "$archive.sig" "$work/package.sig"
if /bin/bash "$G/pacman-recover.sh" > "$work/signature.log" 2>&1; then exit 1; fi
grep -q 'trusted detached signature missing' "$work/signature.log"
[[ ! -e $entry && -e $marker ]]
mv "$work/package.sig" "$archive.sig"
mv "$work/entry-for-signature" "$entry"
echo 'PACMAN_RECOVERY missing-signature-refused PASS'

fi
# Clean current state with a historical incomplete transaction is resumable.
cp /var/log/pacman.log "$work/pacman-log"
printf '[regression] [ALPM] transaction started\n' >> /var/log/pacman.log
printf 'interrupted-package-write\n' > "$marker"
/bin/bash "$G/pacman-recover.sh" > "$work/obsolete.log" 2>&1
[[ ! -e $marker ]]
grep -q 'ownership checks passed' "$work/obsolete.log"
if grep -q 'Reinstalling validated packages' "$work/obsolete.log"; then echo 'FAIL healthy meta-package triggered reinstall'; exit 1; fi
cp "$work/pacman-log" /var/log/pacman.log
# Explicit Repair Center action uses the same functional recovery path.
sh "$G/package-manager.sh" repair > "$work/repair-center.log" 2>&1
pacman -Dk; pacman -Qk gcc; [[ $(pacman -Qqo /usr/bin/gcc) == gcc ]]
echo "PACMAN_OWNERSHIP_RECOVERY PASS state_only=${LR_PACMAN_STATE_ONLY:-0} evidence=$work"
