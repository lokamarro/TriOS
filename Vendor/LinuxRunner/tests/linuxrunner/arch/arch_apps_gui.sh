#!/bin/sh
# Arch Linux ARM desktop applications inside a LinuxRunner desktop session. Run
# as the desktop user from a login shell (run_guest_session.py
# --terminal-command) after arch_apps.sh installed the packages. Each
# application must map a top-level window with the expected title within its
# time limit; it is then closed, except the last, which stays open for the
# final frame. Prints ARCH_GUI <app> PASS|FAIL seconds=N and ARCH_GUI_TEST
# PASS|FAIL; touches $ARCH_GUI_MARKER (default /tmp/arch-gui-done) at the end (passive probe marker).
set -u
fails=0
work=$(mktemp -d /tmp/arch-gui.XXXXXX)
cp /home/mobile/Downloads/.linuxrunner/gui/desktop-wallpaper.png "$work/picture.png" 2>/dev/null || true
printf 'LinuxRunner on Arch Linux ARM\n' > "$work/note.txt"
window_up() { xwininfo -root -tree 2>/dev/null | grep -qi -- "$1"; }
run() {  # name, title pattern, seconds, keep(0|1), command...
 name=$1 pattern=$2 limit=$3 keep=$4; shift 4
 # A window left over from an earlier step must not count as this launch.
 i=0; while window_up "$pattern" && [ "$i" -lt 15 ]; do sleep 1; i=$((i + 1)); done
 if window_up "$pattern"; then echo "ARCH_GUI $name FAIL stale window matching '$pattern' before launch"; fails=$((fails + 1)); return; fi
 "$@" > "$work/$name.log" 2>&1 & pid=$!
 start=$(date +%s); ok=0
 while [ $(( $(date +%s) - start )) -lt "$limit" ]; do
  if window_up "$pattern"; then ok=1; break; fi
  kill -0 "$pid" 2>/dev/null || break
  sleep 1
 done
 elapsed=$(( $(date +%s) - start ))
 if [ "$ok" = 1 ]; then echo "ARCH_GUI $name PASS seconds=$elapsed"
 else echo "ARCH_GUI $name FAIL seconds=$elapsed"; tail -8 "$work/$name.log" | sed 's/^/    /'; fails=$((fails + 1)); fi
 if [ "$keep" = 0 ]; then kill "$pid" 2>/dev/null; sleep 2; pkill -f "$work" 2>/dev/null; sleep 1; fi
}
run xterm 'ArchAppsTerm' 60 0 xterm -title ArchAppsTerm -e sh -c 'sleep 600'
run mousepad 'note.txt' 120 0 mousepad "$work/note.txt"
run feh 'feh' 60 0 feh --title 'feh picture' "$work/picture.png"
run thunar ' - Thunar' 120 0 thunar "$work"
mkdir -p "$work/firefox-profile"
run firefox 'Mozilla Firefox' 300 0 firefox --no-remote --profile "$work/firefox-profile" --new-instance about:blank
run chromium 'Chromium' 300 1 chromium --no-first-run --user-data-dir="$work/chromium-profile" about:blank
echo "ARCH_GUI_TEST $([ $fails = 0 ] && echo PASS || echo FAIL) failures=$fails"
sleep 8; touch "${ARCH_GUI_MARKER:-/tmp/arch-gui-done}"
