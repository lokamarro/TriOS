#!/bin/sh
# Open Desktop on the native iOS path, in the iOS Simulator (IPA 889–891 device
# reports: Open Desktop showed the Standalone GUI Test unless GUI Test had been
# opened first). Drives the app's own button functions through the simulator-only
# LR_SIM_SCRIPT workflow and checks the Open Desktop timeline
# (Documents/Diagnostics/open-desktop-trace.log).
#   sim_open_desktop.sh <LinuxRunner.app built for iphonesimulator> [device]
# The active environment must already have desktop support (or network for the
# repair). Sequences, each from a fresh app launch:
#   open        Open Desktop
#   reopen      Open Desktop, Done, Open Desktop
#   after-test  GUI Test, Done, Open Desktop
# Each Open Desktop must present the desktop controller and paint a frame.
app=${1:?LinuxRunner.app}; device=${2:-booted}; fails=0
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
xcrun simctl install "$device" "$app" || exit 2
data=$(xcrun simctl get_app_container "$device" dev.linuxrunner.app data) || exit 2
trace="$data/Documents/Diagnostics/open-desktop-trace.log"
run() { # name script expected-paints
    xcrun simctl terminate "$device" dev.linuxrunner.app 2>/dev/null; sleep 2
    start=$(cat "$trace" 2>/dev/null | wc -l)
    SIMCTL_CHILD_LR_SIM_SCRIPT="$2;wait:1;report" xcrun simctl launch "$device" dev.linuxrunner.app >/dev/null || return 1
    waited=0
    until tail -n +$((start + 1)) "$trace" 2>/dev/null | grep -q 'workflow step report'; do
        sleep 5; waited=$((waited + 5)); [ $waited -lt 1800 ] || break
    done
    new=$(tail -n +$((start + 1)) "$trace")
    buttons=$(printf '%s\n' "$new" | grep -c 'stage=01 button')
    desktop=$(printf '%s\n' "$new" | grep -c 'viewDidAppear controller=desktop')
    wrong=$(printf '%s\n' "$new" | grep 'stage=01 button' -A3 | grep -c 'full-screen content built desktop=false')
    painted=$(printf '%s\n' "$new" | grep -c 'stage=13 first-frame result=ok')
    if [ "$desktop" -ge "$3" ] && [ "$painted" -ge "$3" ] && [ "$wrong" = 0 ]; then echo "PASS $1 (open-desktop=$buttons desktop-shown=$desktop painted=$painted)"
    else echo "FAIL $1 (open-desktop=$buttons desktop-shown=$desktop painted=$painted gui-test-instead=$wrong)"; fails=$((fails + 1)); fi
}
run open "wait:3;desktop;wait:40" 1
run reopen "wait:3;desktop;wait:40;close;wait:5;desktop;wait:20" 2
run after-test "wait:3;guitest;wait:6;close;wait:4;desktop;wait:30" 1
echo "SIM_OPEN_DESKTOP_TEST $([ $fails = 0 ] && echo PASS || echo FAIL) failures=$fails"
[ $fails = 0 ]
