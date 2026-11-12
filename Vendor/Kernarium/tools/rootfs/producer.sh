# SPDX-License-Identifier: GPL-2.0-or-later
#
# Helpers the rootfs producers share. Sourcing this file only defines names.
# It is POSIX sh, because starter/build-all.sh runs under /bin/sh. A caller
# defines fail(), and usage() before it parses EROFS producer options. Every
# helper variable is global, so internal ones carry a producer_ prefix.

# producer_require_host MACHINE COMMAND...: require native Linux on MACHINE,
# as uname -m names it, an ordinary user with passwordless sudo, and every
# COMMAND.
producer_require_host()
{
	producer_machine=$1
	shift
	[ "$(uname -s)" = Linux ] || fail "the producer requires Linux"
	[ "$(uname -m)" = "$producer_machine" ] || \
		fail "the producer requires native $producer_machine"
	[ "$(id -u)" -ne 0 ] || fail "run as an ordinary user; sudo is used narrowly"
	sudo -n true 2>/dev/null || fail "passwordless sudo is required"
	for producer_command in "$@"; do
		command -v "$producer_command" >/dev/null 2>&1 || \
			fail "required command is unavailable: $producer_command"
	done
}

# producer_require_absent WHAT PATH...: refuse to replace any PATH, which must
# not exist even as a dangling symbolic link.
producer_require_absent()
{
	producer_what=$1
	shift
	for producer_path in "$@"; do
		[ ! -e "$producer_path" ] && [ ! -L "$producer_path" ] || \
			fail "refusing to replace $producer_what: $producer_path"
	done
}

# write_root_file DESTINATION MODE: install standard input as the root-owned
# file DESTINATION with MODE, creating its parents. The input is staged in
# $work_dir, which this user owns.
write_root_file()
{
	producer_temporary=$(mktemp "$work_dir/.root-file.XXXXXX")
	cat > "$producer_temporary"
	sudo -n install -D -m "$2" "$producer_temporary" "$1"
	rm -f -- "$producer_temporary"
}

# producer_run_captured COMMAND...: run COMMAND with its output captured
# through a pipe, then copy that output to standard error. BusyBox passwd
# lowers its own RLIMIT_FSIZE to about 15 MB, so writing straight to a
# caller's standard error that is a log file already past that size would
# kill it with SIGXFSZ.
producer_run_captured()
{
	producer_status=0
	producer_output=$("$@" 2>&1) || producer_status=$?
	[ -z "$producer_output" ] || printf '%s\n' "$producer_output" >&2
	return "$producer_status"
}

# producer_reset_machine_id ROOT: leave the systemd root ROOT, which carries
# D-Bus, without a machine ID, so that every guest generates its own at first
# boot. D-Bus reads /var/lib/dbus/machine-id before /etc/machine-id, and
# systemd adopts a regular D-Bus copy, so the copy the package installs wrote
# becomes a link to /etc/machine-id.
producer_reset_machine_id()
{
	sudo -n truncate -s 0 "$1/etc/machine-id"
	sudo -n ln -sfn /etc/machine-id "$1/var/lib/dbus/machine-id"
}

# producer_install_uml_va_driver ROOT SOURCE: build the VA-API driver tree
# SOURCE (tools/video/va-v4l2) inside the Debian root ROOT, against the libva
# and libdrm that root ships, run its unit test and its probe self-test there,
# and install the driver, the gbm backend module that lets libgbm allocate the
# planar buffers a video client needs, and the Chromium configuration that
# selects the driver. The build tools are installed for the build and removed
# again, so the image keeps only the libraries the driver runs on. ROOT needs a working apt and a mounted
# /proc, and the caller defines fail().
producer_install_uml_va_driver()
{
	producer_va_root=$1
	producer_va_source=$2
	[ -f "$producer_va_source/CMakeLists.txt" ] || \
		fail "no VA-API driver source at $producer_va_source"
	sudo -n rm -rf -- "$producer_va_root/tmp/va-v4l2" \
		"$producer_va_root/tmp/va-v4l2-build"
	sudo -n cp -a -- "$producer_va_source" "$producer_va_root/tmp/va-v4l2"
	sudo -n rm -rf -- "$producer_va_root/tmp/va-v4l2/build"
	sudo -n chroot "$producer_va_root" /usr/bin/env DEBIAN_FRONTEND=noninteractive \
		apt-get install -y --no-install-recommends \
		build-essential cmake ninja-build pkg-config libva-dev libdrm-dev
	# What the driver runs on stays when the build tools go.
	sudo -n chroot "$producer_va_root" apt-mark manual libva2 libva-drm2 libdrm2
	# The prefix is /usr so that GNUInstallDirs picks the multiarch library
	# directory and /etc for the Chromium configuration.
	sudo -n chroot "$producer_va_root" /bin/sh -ec '
		cd /tmp/va-v4l2
		cmake --preset guest -B /tmp/va-v4l2-build -DCMAKE_INSTALL_PREFIX=/usr
		cmake --build /tmp/va-v4l2-build
		/tmp/va-v4l2-build/uml_va_v4l2_tests
		tests/probe_selftest.sh /tmp/va-v4l2-build
		cmake --install /tmp/va-v4l2-build
		for module in uml_drv_video.so virtio_gpu_gbm.so; do
			path=$(find /usr/lib -name "$module")
			test -n "$path"
			if ldd "$path" | grep -q "not found"; then
				ldd "$path" >&2
				exit 1
			fi
		done'
	sudo -n chroot "$producer_va_root" apt-get purge -y --auto-remove \
		build-essential cmake ninja-build pkg-config libva-dev libdrm-dev
	sudo -n rm -rf -- "$producer_va_root/tmp/va-v4l2" \
		"$producer_va_root/tmp/va-v4l2-build"
	sudo -n test -f "$producer_va_root/etc/chromium.d/kernarium-video" || \
		fail "the Chromium configuration was not installed"
}

# producer_stage PARENT NAME: create $staging_dir, a directory this user owns
# beside the output NAME in PARENT, so that the name a root-run tool writes
# stays reserved and publication is a same-directory rename. Until
# producer_finish runs, any exit runs the command named by
# $producer_exit_hook, when the caller set one, and removes the directory.
# A producer may publish the directory itself, so it takes the mode mkdir
# would give it rather than the owner-only mode of mktemp -d.
producer_stage()
{
	staging_dir=$(mktemp -d "$1/.$2.partial.XXXXXX")
	trap producer_cleanup EXIT
	trap 'exit 1' HUP INT TERM
	chmod "$(umask -S)" "$staging_dir"
}

producer_cleanup()
{
	if [ -n "${producer_exit_hook-}" ]; then
		"$producer_exit_hook" || true
	fi
	[ -n "$staging_dir" ] || return 0
	sudo -n rm -rf -- "$staging_dir"
}

# producer_publish STAGED FINAL: rename STAGED to the absent name FINAL. GNU
# mv -n skips an existing destination with status 0, so a staged name that is
# still present means another run took FINAL first.
producer_publish()
{
	mv -T -n -- "$1" "$2"
	[ ! -e "$1" ] || fail "output appeared during publication: $2"
}

# producer_finish: end staging once every output is published. A staging
# directory that was not itself published must be empty by then.
producer_finish()
{
	if [ -e "$staging_dir" ]; then
		rmdir -- "$staging_dir"
	fi
	staging_dir=
	trap - EXIT HUP INT TERM
}

# producer_parse_erofs_options ARG...: set $work_dir and $output from an
# EROFS producer's --work and --output options, and refuse a relative,
# shared, unsafe or existing path, or an existing sidecar, before anything
# is created.
producer_parse_erofs_options()
{
	work_dir=
	output=
	while [ "$#" -gt 0 ]; do
		case "$1" in
		--work)
			[ "$#" -ge 2 ] || fail "--work requires a path"
			work_dir=$2
			shift 2
			;;
		--output)
			[ "$#" -ge 2 ] || fail "--output requires a path"
			output=$2
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		*) fail "unknown option: $1" ;;
		esac
	done

	[ -n "$work_dir" ] && [ -n "$output" ] || {
		usage >&2
		exit 2
	}
	case "$work_dir:$output" in
	/*:/*) ;;
	*) fail "--work and --output must be absolute" ;;
	esac
	[ "$work_dir" != "$output" ] || fail "work and output paths must differ"
	case "$work_dir" in /|/home|/tmp|/usr|/var) fail "unsafe work path: $work_dir" ;; esac
	case "$output" in /|/home|/tmp|/usr|/var) fail "unsafe output path: $output" ;; esac
	case "$output" in *.erofs) ;; *) fail "--output must end in .erofs" ;; esac
	producer_require_absent "work path" "$work_dir"
	producer_require_absent output "$output" "$output.packages.tsv" \
		"$output.elf.tsv" "$output.sha256"
}

# producer_stage_erofs_output: canonicalise $work_dir and $output, create
# $root in the work directory, and stage the image as $staging, with its
# sidecars beside it, in a directory beside the output.
producer_stage_erofs_output()
{
	output_parent=$(dirname -- "$output")
	output_name=$(basename -- "$output")
	case "$output_name" in ''|.|..) fail "unsafe output name: $output_name" ;; esac
	mkdir -p -- "$output_parent"
	output_parent=$(CDPATH= cd -- "$output_parent" && pwd -P)
	output=$output_parent/$output_name
	mkdir -p -- "$work_dir"
	work_dir=$(CDPATH= cd -- "$work_dir" && pwd -P)
	root=$work_dir/root
	mkdir -p -- "$root"
	owner_uid=$(id -u)
	owner_gid=$(id -g)
	producer_stage "$output_parent" "$output_name"
	staging=$staging_dir/$output_name
}

# producer_publish_erofs: checksum the staged image, which root's mkfs.erofs
# wrote, and publish it before its package inventory, ELF inventory and
# checksum.
producer_publish_erofs()
{
	sudo -n chown "$owner_uid:$owner_gid" "$staging"
	producer_checksum=$(sha256sum "$staging" | awk '{print $1}')
	printf '%s  %s\n' "$producer_checksum" "$output_name" > "$staging.sha256"
	producer_publish "$staging" "$output"
	producer_publish "$staging.packages.tsv" "$output.packages.tsv"
	producer_publish "$staging.elf.tsv" "$output.elf.tsv"
	producer_publish "$staging.sha256" "$output.sha256"
	producer_finish
}
