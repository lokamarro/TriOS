#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# The virtual Wi-Fi station sees one open network, VirtWifi. Saving it once
# lets the platform join it whenever the user's Wi-Fi switch is on. This neither
# turns Wi-Fi on nor connects: the switch, and forgetting the network, stay the
# user's. The marker keeps a forgotten network forgotten. A boot that cannot
# save the network (the Wi-Fi service is not up) leaves the marker unset, and
# the next boot tries again.

marker=persist.sys.kernarium.virtwifi_saved

[ "$(getprop $marker)" = 1 ] && exit 0

tries=0
while [ "$tries" -lt 30 ]; do
    # Saving the same network again updates it, so a retry adds no duplicate.
    cmd wifi add-network VirtWifi open >/dev/null 2>&1
    if cmd wifi list-networks 2>/dev/null | grep -q VirtWifi; then
        setprop "$marker" 1
        exit 0
    fi
    tries=$((tries + 1))
    sleep 2
done
exit 1
