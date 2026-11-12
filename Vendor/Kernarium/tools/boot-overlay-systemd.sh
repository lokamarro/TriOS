#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(CDPATH= cd -- "${script_dir}/.." && pwd -P)"
ramdisk="${repo_root}/tools/rootfs/boot-ramdisk"

emulator="${UML_EMULATOR:-${repo_root}/.build/presets/macos-integration/bin/uml-darwin}"
rootfs="${UML_ROOTFS:-${repo_root}/rootfs.erofs}"
memory="${UML_MEMORY:-8G}"
systemd_unit="${UML_SYSTEMD_UNIT:-multi-user.target}"
dry_run=false
extra_kernel_arguments=()

usage()
{
	cat <<EOF
Usage: $(basename "$0") [options] [-- kernel-argument ...]

Boot Darwin UML with an EROFS image, a tmpfs-backed OverlayFS root,
and systemd as PID 1.

Options:
  --emulator PATH     Emulator executable
                      (default: .build/presets/macos-integration/bin/uml-darwin)
  --rootfs PATH       EROFS root filesystem image (default: rootfs.erofs)
  --memory SIZE       Guest memory (default: 8G)
  --unit UNIT         Initial systemd unit (default: multi-user.target)
  --dry-run           Print the command without booting
  -h, --help          Show this help

Environment overrides:
  UML_EMULATOR, UML_ROOTFS, UML_MEMORY, UML_SYSTEMD_UNIT

The emulator's per-run initramfs mounts the image read-only beneath a tmpfs
overlay and starts the harness session, which points the resolver at the
user-mode network, keeps Docker and containerd state on tmpfs and execs the
image's systemd. Host standard input is the guest console throughout.
EOF
}

validate_kernel_argument()
{
	argument=$1
	case "$argument" in
	ubd0*=*|root=*|rootfstype=*|init=*|initrd=*|--guest-*|mem=*|console=*|systemd.unit=*)
		echo "$0: kernel argument is controlled by the launcher: $argument" >&2
		exit 2
		;;
	esac
}

while (($# != 0)); do
	case "$1" in
	--emulator)
		(($# >= 2)) || {
			echo "$0: --emulator requires a path" >&2
			exit 2
		}
		emulator="$2"
		shift 2
		;;
	--rootfs)
		(($# >= 2)) || {
			echo "$0: --rootfs requires a path" >&2
			exit 2
		}
		rootfs="$2"
		shift 2
		;;
	--memory)
		(($# >= 2)) || {
			echo "$0: --memory requires a size" >&2
			exit 2
		}
		memory="$2"
		shift 2
		;;
	--unit)
		(($# >= 2)) || {
			echo "$0: --unit requires a systemd unit" >&2
			exit 2
		}
		systemd_unit="$2"
		shift 2
		;;
	--dry-run)
		dry_run=true
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	--)
		shift
		extra_kernel_arguments+=("$@")
		break
		;;
	-*)
		echo "$0: unknown option: $1" >&2
		usage >&2
		exit 2
		;;
	*)
		extra_kernel_arguments+=("$1")
		shift
		;;
	esac
done

[[ -x "$emulator" ]] || {
	echo "$0: emulator is not executable: $emulator" >&2
	exit 1
}
[[ -f "$rootfs" ]] || {
	echo "$0: rootfs image does not exist: $rootfs" >&2
	exit 1
}
[[ -n "$memory" && -n "$systemd_unit" ]] || {
	echo "$0: memory and systemd unit must be non-empty" >&2
	exit 2
}
# Bash 3.2 treats an empty array as unset under set -u.
for kernel_argument in ${extra_kernel_arguments[@]+"${extra_kernel_arguments[@]}"}; do
	validate_kernel_argument "$kernel_argument"
done

command=(
	"$emulator"
	"--guest-file=/boot.env=${ramdisk}/overlay-systemd/boot.env"
	"--guest-file=/payload/usr/local/libexec/uml-session=${ramdisk}/session"
	"--guest-file=/payload/etc/uml-session.env=${ramdisk}/overlay-systemd/uml-session.env"
	"ubd0c=$rootfs"
	"mem=$memory"
	"console=tty0"
	"console=ttyS0"
	"systemd.unit=$systemd_unit"
	${extra_kernel_arguments[@]+"${extra_kernel_arguments[@]}"}
)

if "$dry_run"; then
	printf '%q ' "${command[@]}"
	printf '\n'
	exit 0
fi

echo "Booting $rootfs as EROFS beneath a tmpfs overlay"
echo "Init: systemd ($systemd_unit)"
exec "${command[@]}"
