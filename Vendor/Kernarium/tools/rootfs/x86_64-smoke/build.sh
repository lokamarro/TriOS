#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

fail()
{
	printf '%s\n' "tools/rootfs/x86_64-smoke/build.sh: $*" >&2
	exit 1
}

[ "$#" -eq 1 ] || fail "usage: build.sh OUTPUT.erofs"
case "$1" in
/*) output=$1 ;;
*) fail "output path must be absolute" ;;
esac
[ ! -e "$output" ] || fail "output already exists: $output"
[ "$(uname -m)" = x86_64 ] || fail "the producer requires an x86-64 builder"
for tool in as ld readelf mkfs.erofs fsck.erofs busybox; do
	command -v "$tool" >/dev/null 2>&1 || fail "missing build tool: $tool"
done

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output_parent=$(dirname -- "$output")
mkdir -p -- "$output_parent"
# The image is written and audited beside the output, then renamed into place,
# so a rejected image is never published.
work=$(mktemp -d "$output_parent/.$(basename -- "$output").partial.XXXXXX")
cleanup()
{
	rm -rf -- "$work"
}
trap cleanup EXIT HUP INT TERM

as --64 "$script_dir/init.S" -o "$work/init.o"
ld -m elf_x86_64 -static -z noexecstack -e _start \
	-o "$work/init" "$work/init.o"
readelf -h "$work/init" | grep -q 'Machine:.*Advanced Micro Devices X86-64' ||
	fail "PID 1 is not x86-64"
if readelf -l "$work/init" | grep -q 'INTERP'; then
	fail "PID 1 must not have a program interpreter"
fi

root="$work/root"
mkdir -p "$root/bin" "$root/dev" "$root/etc" "$root/proc" "$root/sys" \
	"$root/tmp"
install -m 0755 "$work/init" "$root/init"
install -m 0755 "$(command -v busybox)" "$root/bin/busybox"
ln -s busybox "$root/bin/sh"
install -m 0644 "$script_dir/overlay/etc/passwd" "$root/etc/passwd"
install -m 0644 "$script_dir/overlay/etc/group" "$root/etc/group"

image="$work/rootfs.erofs"
mkfs.erofs -L uml-x86-smoke "$image" "$root"

extracted="$work/extracted"
mkdir "$extracted"
fsck.erofs --extract="$extracted" "$image" >/dev/null
for executable in "$extracted/init" "$extracted/bin/busybox"; do
	readelf -h "$executable" |
		grep -q 'Machine:.*Advanced Micro Devices X86-64' ||
		fail "rootfs contains a non-x86-64 executable: $executable"
done
mv -T -n -- "$image" "$output"
[ ! -e "$image" ] || fail "output appeared during publication: $output"
sha256sum "$output"
