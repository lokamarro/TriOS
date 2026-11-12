#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Time discarding 512 MiB of the second UBD disk, /dev/ubdb, which the run
# must leave unused: Kernarium's overlay disk under a tmpfs writable layer.
# /etc/uml-discard.env sets UML_DISCARD_MODE: batched-128m keeps the driver's
# 128 MiB discard limit, fragmented-32k lowers it to 32 KiB. Each of seven
# runs refills the range, discards it and prints
# UML_DISCARD_SAMPLE:<mode>:<run>:<ns>:<requests>:<sectors>:<ticks> from the
# disk's own counters, between UML_DISCARD_READY:<mode>:<limit> and
# UML_DISCARD_END:0. A problem prints UML_DISCARD_ERROR:<reason> instead.

disk=/dev/ubdb
queue=/sys/block/ubdb/queue
parameters=/etc/uml-discard.env

fail()
{
	echo "UML_DISCARD_ERROR:$*"
	exit 1
}

mode=
if [ -f "$parameters" ]; then
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in
		''|'#'*) ;;
		UML_DISCARD_MODE=*) mode=${line#*=} ;;
		*) fail "unknown $parameters line: $line" ;;
		esac
	done < "$parameters"
fi
[ -b "$disk" ] || fail "the overlay block device is unavailable"
[ -x /usr/sbin/blkdiscard ] || fail "blkdiscard is unavailable"
case "$mode" in
batched-128m)
	expected_max=134217728
	;;
fragmented-32k)
	expected_max=32768
	echo "$expected_max" > "$queue/discard_max_bytes" ||
		fail "discard limit update failed"
	;;
*)
	fail "invalid benchmark mode: $mode"
	;;
esac
actual_max=$(cat "$queue/discard_max_bytes")
[ "$actual_max" = "$expected_max" ] ||
	fail "unexpected discard limit $actual_max"
echo "UML_DISCARD_READY:$mode:$actual_max"
run=1
while [ "$run" -le 7 ]; do
	dd if=/dev/zero of="$disk" bs=1M count=512 conv=fsync status=none ||
		fail "payload write failed"
	set -- $(cat /sys/block/ubdb/stat)
	requests_before=${12}
	sectors_before=${14}
	ticks_before=${15}
	started=$(date +%s%N)
	/usr/sbin/blkdiscard -o 0 -l 536870912 "$disk" ||
		fail "discard failed"
	sync
	finished=$(date +%s%N)
	set -- $(cat /sys/block/ubdb/stat)
	requests=$((${12} - requests_before))
	sectors=$((${14} - sectors_before))
	ticks=$((${15} - ticks_before))
	elapsed=$((finished - started))
	echo "UML_DISCARD_SAMPLE:$mode:$run:$elapsed:$requests:$sectors:$ticks"
	run=$((run + 1))
done
echo "UML_DISCARD_END:0"
