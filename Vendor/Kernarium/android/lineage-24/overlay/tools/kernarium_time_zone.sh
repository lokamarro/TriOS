#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Gives the guest the time zone of the host. A new guest starts on GMT and
# Android's automatic time zone has no source here (no telephony, no location
# provider), so the host application states its own zone at every start
# (ro.boot.kernarium.timezone, an IANA name) and this script applies it once
# boot completes, through the platform's own setter (`cmd alarm set-timezone`):
# the framework then persists it as persist.sys.timezone and tells the running
# apps.
#
# The guest follows the host only while nobody in the guest has chosen a zone:
#  - "Set time zone automatically" (Settings.Global.auto_time_zone) is not off.
#    It is on by default, and this platform has nothing to detect a zone with,
#    so Settings offers no switch for it: the user's choice is the zone picker.
#  - The zone is still the one this script applied last (the marker), or the
#    factory GMT of a guest that has never had one. Settings' picker and the
#    setup wizard's both set the zone without turning the setting off, so a zone
#    that is not the marker's was chosen by the user and is never replaced.
# A name the guest's tzdata lacks is refused by the platform, which leaves the
# zone as it was. The script logs one line (tag kernarium_time_zone) for what it
# did, and nothing here can fail a boot.

tag=kernarium_time_zone
marker=persist.sys.kernarium.timezone_applied

note() {
    log -t "$tag" -p i "$*"
}

zone=$(getprop ro.boot.kernarium.timezone)
# The launch gave none (the standalone launcher, an older host application).
[ -n "$zone" ] || exit 0

# The host application checks the name too. Nothing else may reach `cmd`.
case "$zone" in
    *[!A-Za-z0-9_+/-]* | /* | */ | *//*)
        note "ignoring the host's time zone: not a time zone name"
        exit 0
        ;;
esac

# The services start before boot completes; a retry covers a slow one.
auto=
tries=0
while [ "$tries" -lt 30 ]; do
    auto=$(cmd settings get global auto_time_zone 2>/dev/null)
    case "$auto" in
        0 | 1 | null) break ;;
    esac
    tries=$((tries + 1))
    sleep 2
done

current=$(getprop persist.sys.timezone)
applied=$(getprop "$marker")

case "$auto" in
    0)
        note "automatic time zone is off: keeping $current"
        exit 0
        ;;
    1 | null) ;;
    *)
        note "automatic time zone is unreadable: keeping $current"
        exit 1
        ;;
esac

if [ "$current" = "$zone" ]; then
    [ "$applied" = "$zone" ] || setprop "$marker" "$zone"
    note "time zone is already $zone"
    exit 0
fi

# Whether the zone is one a person chose in the guest.
chosen=
if [ -n "$applied" ]; then
    [ "$current" = "$applied" ] || chosen=1
else
    case "$current" in
        "" | GMT) ;;
        *) chosen=1 ;;
    esac
fi
if [ -n "$chosen" ]; then
    note "keeping the zone chosen in the guest, $current"
    exit 0
fi

cmd alarm set-timezone "$zone" >/dev/null 2>&1
if [ "$(getprop persist.sys.timezone)" = "$zone" ]; then
    setprop "$marker" "$zone"
    note "time zone is $zone, as the host's (was ${current:-unset})"
else
    note "tzdata has no $zone: keeping $current"
fi
exit 0
