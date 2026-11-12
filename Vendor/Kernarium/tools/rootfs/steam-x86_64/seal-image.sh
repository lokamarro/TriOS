#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Resume image auditing and sealing after assembly has completed.
set -euo pipefail
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
fail()
{
    printf '%s\n' "tools/rootfs/steam-x86_64/seal-image.sh: $*" >&2
    exit 1
}
. "$here/../producer.sh"
if [ "$#" -ne 3 ]; then
    printf '%s\n' 'Usage: seal-image.sh INPUTS ASSEMBLED_WORK ABSENT_OUTPUT.erofs' >&2
    exit 2
fi
inputs=$(realpath "$1")
work=$(realpath "$2")
output=$3
root=$work/root
case "$output" in /*/steam-x86_64-*.erofs) ;; *) exit 2 ;; esac
producer_require_absent output "$output" "$output.sha256" "$output.packages.tsv" \
    "$output.binaries.tsv" "$output.provenance.json"
mkdir -p "$(dirname "$output")"
# Every artifact is written into a directory this user owns beside the output
# and is complete before the first one is published. Each rename must take its
# own absent name, so a lost race fails instead of pairing these sidecars with
# another run's image.
producer_stage "$(dirname "$output")" "$(basename "$output")"
stage=$staging_dir
test -z "$(sudo -n chroot "$root" dpkg --audit)"
test -z "$(sudo -n chroot "$root" dpkg --print-foreign-architectures)"
sudo -n python3 -B "$here/verify-image.py" --root "$root" --output "$stage/binaries.tsv"
sudo -n chroot "$root" dpkg-query -W -f='${binary:Package}\t${Version}\n' | sort > "$stage/packages.tsv"
sudo -n rm -f "$root/usr/sbin/policy-rc.d"
sudo -n find "$root/var/log" -type f -exec truncate -s 0 '{}' +
sudo -n mkfs.erofs --quiet -L uml-steam-x86 -zzstd,level=3 -C1048576 "$stage/rootfs.erofs" "$root"
sudo -n chown "$(id -u):$(id -g)" "$stage/rootfs.erofs" "$stage/binaries.tsv"
python3 -B "$here/provenance.py" --inputs "$inputs" --root "$root" --output "$stage/provenance.json"
checksum=$(sha256sum "$stage/rootfs.erofs" | awk '{print $1}')
printf '%s  %s\n' "$checksum" "$(basename "$output")" > "$stage/rootfs.erofs.sha256"
producer_publish "$stage/rootfs.erofs" "$output"
producer_publish "$stage/packages.tsv" "$output.packages.tsv"
producer_publish "$stage/binaries.tsv" "$output.binaries.tsv"
producer_publish "$stage/provenance.json" "$output.provenance.json"
producer_publish "$stage/rootfs.erofs.sha256" "$output.sha256"
producer_finish
printf '%s\n' "Built Steam x86-64 gaming rootfs: $output"
