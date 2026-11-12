#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Gives back the memory of the apps the system starts around boot and nobody
# uses. Privacy sandbox services, the backup apps, key provisioning, domain
# verification and Health Connect each start from a boot receiver or a bind by
# the system and then sit cached, holding memory until the low memory killer
# wants it. Their receivers and bindings stay: this runs once, after boot has
# settled, and kills the processes of the listed packages that are cached, which
# is a death the system could have chosen itself. Alarms, jobs and bindings
# survive it, and the next use starts the app again. A package with a process
# that is not cached (a started service, the previous app, the home app) is
# logged and left alone; so is one the system keeps higher than cached.
#
# Each line below is a package and the names of its processes. The list is
# reviewed: the target-files verifier requires exactly this one.

reclaim_list='
android.ext.services .adservices android.ext.services
com.android.federatedcompute.services com.android.federatedcompute.services
com.android.ondevicepersonalization.services com.android.ondevicepersonalization.services
com.stevesoltys.seedvault com.stevesoltys.seedvault
org.calyxos.backup.contacts org.calyxos.backup.contacts
com.android.localtransport com.android.localtransport
com.android.rkpdapp com.android.rkpdapp
com.android.statementservice com.android.statementservice
com.android.healthconnect.controller com.android.healthconnect.controller
'

tag=kernarium_reclaim_boot_apps
settle=120

note() {
    log -t "$tag" -p i "$*"
}

sleep "$settle"

lru=$(dumpsys activity lru 2>/dev/null)
if [ -z "$lru" ]; then
    note "no process list: nothing reclaimed"
    exit 1
fi

echo "$reclaim_list" | while read -r pkg processes; do
    [ -n "$pkg" ] || continue
    cached=""
    busy=""
    kept=""
    for proc in $processes; do
        line=$(echo "$lru" | grep -F ":$proc/")
        [ -n "$line" ] || continue
        set -- $line
        # "#29: cch ..." and "# 9: cch ..."
        if [ "$1" = "#" ]; then
            label=$3
        else
            label=$2
        fi
        case "$label" in
            cch*) cached="$cached $proc" ;;
            svc|svcb|prev|home) busy="$busy $proc:$label" ;;
            *) kept="$kept $proc:$label" ;;
        esac
    done
    if [ -n "$busy" ]; then
        note "$pkg left alone, not cached:$busy"
    elif [ -z "$cached" ]; then
        note "$pkg has no cached process (kept:${kept:- none})"
    else
        out=$(cmd activity kill --user 0 "$pkg" 2>&1)
        rc=$?
        note "$pkg killed cached$cached (kept:${kept:- none}) rc=$rc $out"
    fi
done
exit 0
