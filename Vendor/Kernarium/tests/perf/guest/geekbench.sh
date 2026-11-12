#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Geekbench as every benchmark harness runs it, from the payload an image
# carries and without uploading results. The program is Geekbench 6's
# geekbench6 or, for a 32-bit Arm guest, Geekbench 5's geekbench_armv7 (and
# geekbench_aarch64 for the 64-bit control of that comparison), found by name
# and never by version. /etc/uml-geekbench.env may set
# UML_GEEKBENCH_WORKLOADS to comma-separated workload numbers, which runs only
# those single-core workloads. For an image that carries no payload,
# UML_GEEKBENCH_PAYLOAD may name a block device holding an EROFS image with a
# Geekbench release at its root; the script mounts it read-only and runs its
# program instead. The run prints UML_GEEKBENCH_READY and
# UML_GEEKBENCH_PROGRAM:<path>, then UML_GEEKBENCH_TARGETS:<list> when
# targeted, and ends with UML_GEEKBENCH_END:<status>, exiting with
# Geekbench's status. A problem before the benchmark starts prints
# UML_GEEKBENCH_ERROR:<reason> instead. Every workload runs one iteration
# (user rule, 2026-10-12), so scores taken before that rule, at Geekbench's
# default iteration count, are not comparable with later ones.

parameters=/etc/uml-geekbench.env
payload_root=/opt/geekbench-payload

fail()
{
	echo "UML_GEEKBENCH_ERROR:$*"
	exit 1
}

workloads=
payload=
if [ -f "$parameters" ]; then
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in
		''|'#'*) ;;
		UML_GEEKBENCH_WORKLOADS=*) workloads=${line#*=} ;;
		UML_GEEKBENCH_PAYLOAD=/dev/*) payload=${line#*=} ;;
		*) fail "unknown $parameters line: $line" ;;
		esac
	done < "$parameters"
	case "$workloads" in
	*[!0-9,]*|,*|*,|*,,*) fail "malformed workload list: $workloads" ;;
	esac
fi
if [ -n "$payload" ]; then
	mkdir -p "$payload_root" &&
		mount -t erofs -o ro "$payload" "$payload_root" ||
		fail "cannot mount the Geekbench payload $payload"
	candidates="$payload_root/geekbench6 $payload_root/geekbench_armv7"
	candidates="$candidates $payload_root/geekbench_aarch64"
else
	candidates="/opt/geekbench/*/geekbench6 /opt/geekbench/*/geekbench_armv7"
	candidates="$candidates /opt/geekbench/*/geekbench_aarch64"
fi
benchmark=
for candidate in $candidates; do
	if [ -x "$candidate" ]; then
		benchmark=$candidate
		break
	fi
done
[ -n "$benchmark" ] || fail "the image has no Geekbench payload"

export HOME=/root
echo "UML_GEEKBENCH_READY"
echo "UML_GEEKBENCH_PROGRAM:$benchmark"
if [ -n "$workloads" ]; then
	echo "UML_GEEKBENCH_TARGETS:$workloads"
	"$benchmark" --no-upload --iterations 1 --single-core --section 1 \
		--workload "$workloads"
else
	"$benchmark" --no-upload --iterations 1
fi
status=$?
printf '\nUML_GEEKBENCH_END:%s\n' "$status"
exit "$status"
