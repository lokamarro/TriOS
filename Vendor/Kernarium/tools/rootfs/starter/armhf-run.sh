#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Run a command in a chroot of an armhf root tree on an arm64 Linux host,
# executing the tree's ARM programs under qemu-arm user emulation.
set -eu

PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH

# Debian's qemu-user ships this link to its static qemu-arm. QEMU recognises
# the "-binfmt-P" suffix and takes the program's own argv[0] from the kernel,
# which multi-call programs such as kmod need.
INTERPRETER=/usr/libexec/qemu-binfmt/arm-binfmt-P
# ET_EXEC and ET_DYN ARM EABI ELF files, as QEMU's own binfmt configuration
# registers them. The kernel reads the \xNN escapes itself.
MAGIC='\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x28\x00'
MASK='\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff'

fail()
{
	printf '%s\n' "tools/rootfs/starter/armhf-run.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: tools/rootfs/starter/armhf-run.sh ROOT COMMAND [ARGUMENT...]
       tools/rootfs/starter/armhf-run.sh --check

Run COMMAND in a chroot of the armhf tree ROOT, as root on arm64 Linux with
Debian's qemu-user installed. COMMAND is a path inside ROOT; its ARM programs
run under qemu-arm. --check only proves that the host can do this.

The host's binfmt_misc is not touched, because a container often forbids
registering a format in it. The command runs in new user, mount, PID and
network namespaces that map every ID to itself, and the mount namespace has
a private binfmt_misc instance holding the one qemu-arm format. The format is
fix-binary, so the chroot needs no copy of the emulator. Nothing outlives the
command: the namespaces and the format go with it, and the network is empty.
EOF
}

mode=run
case "${1-}" in
-h|--help)
	usage
	exit 0
	;;
--check)
	mode=check
	[ "$#" -eq 1 ] || {
		usage >&2
		exit 2
	}
	;;
*)
	[ "$#" -ge 2 ] || {
		usage >&2
		exit 2
	}
	;;
esac
[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$INTERPRETER" ] || fail "$INTERPRETER is missing; install qemu-user"
command -v unshare >/dev/null 2>&1 || fail "unshare is unavailable"
root=
if [ "$mode" = run ]; then
	root=$1
	shift
	[ -d "$root" ] && [ ! -L "$root" ] || fail "not a real directory: $root"
fi

# The private binfmt_misc instance mounts here. The mount lives only in the
# command's namespace, so the empty directory is the sole trace.
mountpoint=$(mktemp -d "${TMPDIR:-/tmp}/armhf-binfmt.XXXXXX")
trap 'rmdir -- "$mountpoint"' EXIT
trap 'exit 1' HUP INT TERM

status=0
unshare --user --map-users=all --map-groups=all --mount --pid --net --fork \
	--kill-child -- sh -eu -c '
	mode=$1 mountpoint=$2 root=$3 interpreter=$4 magic=$5 mask=$6
	shift 6
	mount -t binfmt_misc binfmt_misc "$mountpoint"
	printf "%s" ":qemu-arm:M::$magic:$mask:$interpreter:OPF" \
		> "$mountpoint/register"
	[ "$mode" = run ] || exit 0
	exec chroot "$root" "$@"
' armhf-run "$mode" "$mountpoint" "$root" "$INTERPRETER" "$MAGIC" "$MASK" \
	"$@" || status=$?
[ "$status" -eq 0 ] || [ "$mode" != check ] || \
	printf '%s\n' "tools/rootfs/starter/armhf-run.sh: the host cannot run a private qemu-arm format (needs unshare --map-users and Linux 6.7 or newer)" >&2
exit "$status"
