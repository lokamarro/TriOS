#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Exercise the rootfs producers' shared helpers against stub host tools, under
# every POSIX shell and bash that is available.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
HELPERS=$ROOT_DIR/tools/rootfs/producer.sh
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-rootfs-producer.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM
TEST_ROOT=$(CDPATH= cd -- "$TEST_ROOT" && pwd -P)
STUBS=$TEST_ROOT/bin
LOG=$TEST_ROOT/stub.log
REAL_ID=$(command -v id)
REAL_MV=$(command -v mv)

fail()
{
	printf '%s\n' "test_producer.sh: $*" >&2
	exit 1
}

# The producers run these as root on native Linux. The stubs report the host
# the test selects through STUB_* variables, record every sudo command, and
# accept only the argument forms the helpers are meant to use.
mkdir -p "$STUBS"
cat > "$STUBS/uname" <<'EOF'
#!/bin/sh
case "$1" in
-s) printf '%s\n' "${STUB_KERNEL:-Linux}" ;;
-m) printf '%s\n' "${STUB_MACHINE:-x86_64}" ;;
*) exit 64 ;;
esac
EOF
cat > "$STUBS/id" <<EOF
#!/bin/sh
if [ "\$1" = -u ] && [ -n "\${STUB_UID-}" ]; then
	printf '%s\n' "\$STUB_UID"
	exit 0
fi
exec "$REAL_ID" "\$@"
EOF
cat > "$STUBS/sudo" <<'EOF'
#!/bin/sh
printf 'sudo %s\n' "$*" >> "$STUB_LOG"
[ -z "${STUB_SUDO_FAILS-}" ] || exit 1
# The helpers must never let sudo prompt.
[ "$1" = -n ] || exit 64
shift
if [ "$1" = install ]; then
	# install -D -m MODE SOURCE DESTINATION, as write_root_file uses it.
	[ "$#" -eq 6 ] && [ "$2" = -D ] && [ "$3" = -m ] || exit 64
	mkdir -p -- "$(dirname -- "$6")"
	cp -- "$5" "$6"
	chmod "$4" "$6"
	exit 0
fi
exec "$@"
EOF
# GNU mv 9.x -n skips an existing destination and still exits 0, which is
# what producer_publish must detect.
cat > "$STUBS/mv" <<EOF
#!/bin/sh
[ "\$#" -eq 5 ] && [ "\$1" = -T ] && [ "\$2" = -n ] && [ "\$3" = -- ] || exit 64
if [ -e "\$5" ] || [ -L "\$5" ]; then
	exit 0
fi
exec "$REAL_MV" -- "\$4" "\$5"
EOF
chmod 0755 "$STUBS/uname" "$STUBS/id" "$STUBS/sudo" "$STUBS/mv"

PRELUDE='set -eu
fail()
{
	printf "fixture: %s\n" "$*" >&2
	exit 1
}
usage()
{
	printf "%s\n" "fixture usage"
}
. "$HELPERS"
'

# run_fixture BODY: run BODY under $shell after the prelude, with the stubs
# first on PATH. The status is left in $status and the output in
# $TEST_ROOT/out and $TEST_ROOT/err.
run_fixture()
{
	printf '%s%s\n' "$PRELUDE" "$1" > "$TEST_ROOT/case.sh"
	: > "$LOG"
	status=0
	PATH=$STUBS:$PATH HELPERS=$HELPERS P=$P STUB_LOG=$LOG \
		"$shell" "$TEST_ROOT/case.sh" > "$TEST_ROOT/out" \
		2> "$TEST_ROOT/err" || status=$?
}

# expect_success NAME BODY
expect_success()
{
	run_fixture "$2"
	[ "$status" -eq 0 ] ||
		fail "$shell: $1 failed ($status): $(cat "$TEST_ROOT/err")"
}

# expect_failure NAME MESSAGE BODY
expect_failure()
{
	run_fixture "$3"
	[ "$status" -ne 0 ] || fail "$shell: accepted $1"
	grep -qF -- "$2" "$TEST_ROOT/err" ||
		fail "$shell: $1 printed '$(cat "$TEST_ROOT/err")', not '$2'"
}

mode_of()
{
	ls -lnd -- "$1" | cut -c1-10
}

run_cases()
{
	P=$TEST_ROOT/$(basename -- "$shell")
	mkdir -p "$P"

	# Sourcing only defines names.
	expect_success "sourcing the helpers" ''
	[ ! -s "$TEST_ROOT/out" ] && [ ! -s "$TEST_ROOT/err" ] ||
		fail "$shell: sourcing the helpers printed output"

	# EROFS options: usage, then every refusal before anything is created.
	run_fixture 'producer_parse_erofs_options'
	[ "$status" -eq 2 ] && grep -qx 'fixture usage' "$TEST_ROOT/err" ||
		fail "$shell: missing options did not print usage with status 2"
	expect_success "--help" 'producer_parse_erofs_options --help'
	grep -qx 'fixture usage' "$TEST_ROOT/out" || fail "$shell: --help printed no usage"
	expect_failure "--work without a path" "--work requires a path" \
		'producer_parse_erofs_options --work'
	expect_failure "--output without a path" "--output requires a path" \
		'producer_parse_erofs_options --work "$P/work" --output'
	expect_failure "an unknown option" "unknown option: --bogus" \
		'producer_parse_erofs_options --bogus'
	expect_failure "a relative work path" "--work and --output must be absolute" \
		'producer_parse_erofs_options --work work --output "$P/o.erofs"'
	expect_failure "one path for both" "work and output paths must differ" \
		'producer_parse_erofs_options --work "$P/o.erofs" --output "$P/o.erofs"'
	expect_failure "/tmp as the work path" "unsafe work path: /tmp" \
		'producer_parse_erofs_options --work /tmp --output "$P/o.erofs"'
	expect_failure "/var as the output" "unsafe output path: /var" \
		'producer_parse_erofs_options --work "$P/work" --output /var'
	expect_failure "a non-EROFS output" "--output must end in .erofs" \
		'producer_parse_erofs_options --work "$P/work" --output "$P/o.img"'
	mkdir "$P/work-exists"
	expect_failure "an existing work path" "refusing to replace work path: $P/work-exists" \
		'producer_parse_erofs_options --work "$P/work-exists" --output "$P/o.erofs"'
	ln -s "$P/absent" "$P/work-link"
	expect_failure "a dangling work link" "refusing to replace work path: $P/work-link" \
		'producer_parse_erofs_options --work "$P/work-link" --output "$P/o.erofs"'
	for suffix in '' .packages.tsv .elf.tsv .sha256; do
		: > "$P/o.erofs$suffix"
		expect_failure "an existing o.erofs$suffix" \
			"refusing to replace output: $P/o.erofs$suffix" \
			'producer_parse_erofs_options --work "$P/work" --output "$P/o.erofs"'
		rm -f -- "$P/o.erofs$suffix"
	done
	expect_success "valid options" '
producer_parse_erofs_options --work "$P/work" --output "$P/o.erofs"
printf "%s\n" "$work_dir" "$output"'
	[ "$(cat "$TEST_ROOT/out")" = "$(printf '%s\n' "$P/work" "$P/o.erofs")" ] ||
		fail "$shell: valid options were not kept"
	[ ! -e "$P/work" ] && [ ! -e "$P/o.erofs" ] ||
		fail "$shell: option parsing created a path"

	# Host requirements.
	expect_failure "a non-Linux host" "the producer requires Linux" '
export STUB_KERNEL=Darwin
producer_require_host x86_64'
	expect_failure "another machine" "the producer requires native x86_64" '
export STUB_MACHINE=aarch64
producer_require_host x86_64'
	expect_failure "root" "run as an ordinary user; sudo is used narrowly" '
export STUB_UID=0
producer_require_host x86_64'
	expect_failure "no passwordless sudo" "passwordless sudo is required" '
export STUB_SUDO_FAILS=1
producer_require_host x86_64'
	expect_failure "a missing command" \
		"required command is unavailable: uml-absent-command" \
		'producer_require_host x86_64 cat uml-absent-command'
	expect_success "a matching host" '
export STUB_MACHINE=aarch64
producer_require_host aarch64 cat sh'
	grep -qx 'sudo -n true' "$LOG" || fail "$shell: sudo was not probed with -n"

	# The root-file writer installs its input with the mode, creates the
	# parents, removes its temporary file and keeps the caller's variables.
	expect_success "write_root_file" '
work_dir=$P/writer
mkdir -p "$work_dir"
printf "one\ntwo\n" > "$P/writer-input"
destination=kept mode=kept temporary=kept
write_root_file "$P/writer-root/etc/deep/file" 0640 < "$P/writer-input"
[ "$destination:$mode:$temporary" = kept:kept:kept ]'
	[ "$(cat "$P/writer-root/etc/deep/file")" = "$(printf 'one\ntwo')" ] ||
		fail "$shell: write_root_file changed its input"
	[ "$(mode_of "$P/writer-root/etc/deep/file")" = -rw-r----- ] ||
		fail "$shell: write_root_file did not apply the mode"
	[ -z "$(ls -A "$P/writer")" ] || fail "$shell: write_root_file left its temporary file"
	grep -q "^sudo -n install -D -m 0640 $P/writer/\\.root-file\\.[^ ]* $P/writer-root/etc/deep/file\$" \
		"$LOG" || fail "$shell: write_root_file did not install through sudo -n"

	# A root leaves no machine ID behind: the package installs' IDs go, and
	# D-Bus's copy follows the ID systemd generates at first boot.
	mkdir -p "$P/machine-root/etc" "$P/machine-root/var/lib/dbus"
	printf '%s\n' 0123456789abcdef0123456789abcdef > "$P/machine-root/etc/machine-id"
	printf '%s\n' fedcba9876543210fedcba9876543210 > \
		"$P/machine-root/var/lib/dbus/machine-id"
	expect_success "producer_reset_machine_id" \
		'producer_reset_machine_id "$P/machine-root"'
	[ -f "$P/machine-root/etc/machine-id" ] && [ ! -L "$P/machine-root/etc/machine-id" ] &&
		[ ! -s "$P/machine-root/etc/machine-id" ] ||
		fail "$shell: /etc/machine-id is not an empty file"
	[ -L "$P/machine-root/var/lib/dbus/machine-id" ] &&
		[ "$(readlink "$P/machine-root/var/lib/dbus/machine-id")" = /etc/machine-id ] ||
		fail "$shell: /var/lib/dbus/machine-id is not a link to /etc/machine-id"
	grep -qx "sudo -n truncate -s 0 $P/machine-root/etc/machine-id" "$LOG" &&
		grep -qx "sudo -n ln -sfn /etc/machine-id $P/machine-root/var/lib/dbus/machine-id" \
			"$LOG" || fail "$shell: the machine ID was not reset through sudo -n"

	# A command that lowers its own file-size limit, as BusyBox passwd does,
	# dies writing to a standard error that is a log file past that limit.
	# Captured, it runs, and its output and status reach the caller. The log
	# grows through its own descriptor, as a producer's log does, because
	# XNU checks the limit against the descriptor's offset before appending.
	run_fixture '
exec 2> "$P/direct-log"
head -c 65536 /dev/zero >&2
sh -c "ulimit -f 1; printf %s%s direct- output >&2"'
	[ "$status" -ne 0 ] && ! grep -q direct-output "$P/direct-log" ||
		fail "$shell: a command wrote past its file-size limit ($status)"
	run_fixture '
exec 2> "$P/captured-log"
head -c 65536 /dev/zero >&2
producer_run_captured sh -c "ulimit -f 1; echo captured >&2"
producer_run_captured sh -c "echo failed; exit 3" || printf "%s\n" "$?"'
	[ "$status" -eq 0 ] && [ "$(cat "$TEST_ROOT/out")" = 3 ] ||
		fail "$shell: a captured command failed ($status) or lost its status"
	[ "$(tail -c 16 "$P/captured-log")" = "$(printf 'captured\nfailed')" ] ||
		fail "$shell: a captured command's output did not reach standard error"

	# Publication renames onto an absent name and fails when a concurrent
	# run took the name first, leaving that run's output alone.
	expect_success "publication" '
printf staged > "$P/staged"
producer_publish "$P/staged" "$P/published"'
	[ "$(cat "$P/published")" = staged ] && [ ! -e "$P/staged" ] ||
		fail "$shell: producer_publish did not rename its input"
	printf winner > "$P/taken"
	expect_failure "a lost race" "output appeared during publication: $P/taken" '
printf loser > "$P/loser"
producer_publish "$P/loser" "$P/taken"'
	[ "$(cat "$P/taken")" = winner ] && [ "$(cat "$P/loser")" = loser ] ||
		fail "$shell: a lost race changed either output"

	# A failed run releases the producer's resources through its hook, even
	# when the hook fails, and removes the staging directory.
	mkdir -p "$P/stage-fail"
	expect_failure "a failing producer" "fixture: build failed" '
hook()
{
	: > "$P/hook-ran"
	return 1
}
producer_exit_hook=hook
producer_stage "$P/stage-fail" out.erofs
printf "%s\n" "$staging_dir"
fail "build failed"'
	case "$(cat "$TEST_ROOT/out")" in
	"$P/stage-fail/.out.erofs.partial."*) ;;
	*) fail "$shell: staging was not beside the output" ;;
	esac
	[ -e "$P/hook-ran" ] || fail "$shell: a failing run skipped the exit hook"
	[ -z "$(ls -A "$P/stage-fail")" ] || fail "$shell: a failing run left its staging"
	mkdir -p "$P/stage-signal"
	run_fixture '
producer_stage "$P/stage-signal" out.erofs
kill -TERM $$
sleep 5'
	[ "$status" -eq 1 ] && [ -z "$(ls -A "$P/stage-signal")" ] ||
		fail "$shell: a terminated run left its staging or status $status"

	# A finished run publishes files out of its staging directory, or the
	# directory itself, and neither leaves staging nor runs the hook.
	mkdir -p "$P/stage-ok" "$P/release-parent" "$P/private-parent"
	expect_success "a finished run" '
hook()
{
	: > "$P/hook-ran-on-success"
}
producer_exit_hook=hook
producer_stage "$P/stage-ok" out.erofs
printf image > "$staging_dir/out.erofs"
producer_publish "$staging_dir/out.erofs" "$P/stage-ok/out.erofs"
producer_finish
umask 022
producer_stage "$P/release-parent" release
printf manifest > "$staging_dir/manifest"
producer_publish "$staging_dir" "$P/release-parent/release"
producer_finish
umask 027
producer_stage "$P/private-parent" release
producer_publish "$staging_dir" "$P/private-parent/release"
producer_finish'
	[ ! -e "$P/hook-ran-on-success" ] || fail "$shell: a finished run ran the exit hook"
	[ "$(ls -A "$P/stage-ok")" = out.erofs ] ||
		fail "$shell: a finished file publication left staging"
	[ "$(ls -A "$P/release-parent")" = release ] &&
		[ "$(cat "$P/release-parent/release/manifest")" = manifest ] ||
		fail "$shell: a finished directory publication left staging"
	# A published directory has the mode mkdir gives under the caller's
	# umask, not the owner-only mode of mktemp -d.
	[ "$(mode_of "$P/release-parent/release")" = drwxr-xr-x ] &&
		[ "$(mode_of "$P/private-parent/release")" = drwxr-x--- ] ||
		fail "$shell: published directories have modes $(mode_of "$P/release-parent/release") and $(mode_of "$P/private-parent/release")"

	# The EROFS prologue canonicalises both paths and stages beside the
	# output; the tail checksums the image and publishes all four files.
	mkdir -p "$P/e2e-real"
	ln -s e2e-real "$P/e2e-link"
	expect_success "an EROFS producer" '
producer_parse_erofs_options --work "$P/e2e-work" --output "$P/e2e-link/image.erofs"
producer_stage_erofs_output
printf image > "$staging"
printf packages > "$staging.packages.tsv"
printf elf > "$staging.elf.tsv"
producer_publish_erofs
printf "%s\n" "$root" "$output"'
	[ "$(cat "$TEST_ROOT/out")" = "$(printf '%s\n' "$P/e2e-work/root" "$P/e2e-real/image.erofs")" ] ||
		fail "$shell: the EROFS prologue did not canonicalise its paths"
	[ -d "$P/e2e-work/root" ] || fail "$shell: the EROFS prologue created no root"
	[ "$(ls -A "$P/e2e-real" | tr '\n' ' ')" = \
		'image.erofs image.erofs.elf.tsv image.erofs.packages.tsv image.erofs.sha256 ' ] ||
		fail "$shell: the EROFS tail published $(ls -A "$P/e2e-real")"
	[ "$(cat "$P/e2e-real/image.erofs.packages.tsv"):$(cat "$P/e2e-real/image.erofs.elf.tsv")" = \
		packages:elf ] || fail "$shell: the EROFS tail changed an inventory"
	[ "$(cat "$P/e2e-real/image.erofs.sha256")" = \
		"$(printf image | sha256sum | awk '{print $1}')  image.erofs" ] ||
		fail "$shell: the EROFS checksum does not describe the image"
	grep -q "^sudo -n chown $("$REAL_ID" -u):$("$REAL_ID" -g) $P/e2e-real/\\.image\\.erofs\\.partial\\.[^ ]*/image\\.erofs\$" \
		"$LOG" || fail "$shell: the EROFS tail did not take the image from root"
}

# Each producer of a systemd root that carries D-Bus resets its machine ID.
# The Steam image instead links /etc/machine-id to its persistent disk.
for producer in starter/build-all.sh x86_64-desktop/build.sh; do
	grep -qx '[[:space:]]*producer_reset_machine_id "$root"' \
		"$ROOT_DIR/tools/rootfs/$producer" ||
		fail "$producer does not reset its machine ID"
done
# The starter's BusyBox passwd writes nothing to the producer's log itself.
grep -qx '[[:space:]]*producer_run_captured as_root chroot "$root" /usr/bin/passwd -d root' \
	"$ROOT_DIR/tools/rootfs/starter/build-all.sh" ||
	fail "the starter's BusyBox passwd writes to the caller's standard error"

tested=
for shell in sh dash bash; do
	command -v "$shell" > /dev/null 2>&1 || continue
	run_cases
	tested="$tested $shell"
done
printf '%s\n' "rootfs producer helper tests passed under:$tested"
