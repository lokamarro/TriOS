#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Resolve and validate the required, Git-ignored kernel source configuration.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/mainline-6.18"
CONFIG_PATH=${UML_KERNEL_SOURCE_CONFIG:-"$LINE_ROOT/source-location.conf"}
REQUIRE_SOURCE=0

fail()
{
	printf '%s\n' "kernel/mainline-6.18/scripts/source-location.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/mainline-6.18/scripts/source-location.sh \
       [--config PATH] [--require-source]

Print the configured canonical Linux source path after verifying that its
location is case-sensitive. The configuration must contain exactly one active
line:

  UML_KERNEL_SOURCE_DIR=/absolute/case-sensitive/path/linux

The default configuration is kernel/mainline-6.18/source-location.conf, which
is ignored by Git. UML_KERNEL_SOURCE_CONFIG or --config may select another
configuration file for an isolated replay or tooling fixture.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--config)
		[ "$#" -ge 2 ] || fail "--config requires a path"
		CONFIG_PATH=$2
		shift 2
		;;
	--require-source)
		REQUIRE_SOURCE=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -f "$CONFIG_PATH" ] ||
	fail "required configuration is missing: $CONFIG_PATH (copy kernel/mainline-6.18/source-location.conf.example and set UML_KERNEL_SOURCE_DIR)"

active_count=$(awk '
	/^[[:space:]]*#/ || /^[[:space:]]*$/ { next }
	{ count++ }
	END { print count + 0 }
' "$CONFIG_PATH")
[ "$active_count" -eq 1 ] ||
	fail "$CONFIG_PATH must contain exactly one active UML_KERNEL_SOURCE_DIR assignment"
assignment=$(awk '
	/^[[:space:]]*#/ || /^[[:space:]]*$/ { next }
	{ print }
' "$CONFIG_PATH")
case "$assignment" in
UML_KERNEL_SOURCE_DIR=/*) SOURCE_DIR=${assignment#UML_KERNEL_SOURCE_DIR=} ;;
*) fail "$CONFIG_PATH must set UML_KERNEL_SOURCE_DIR to an unquoted absolute path" ;;
esac
case "$SOURCE_DIR" in
/|*/../*|*/..|*/./*|*/.)
	fail "UML_KERNEL_SOURCE_DIR must name a normalized checkout path"
	;;
esac

PARENT_DIR=$(dirname -- "$SOURCE_DIR")
SOURCE_NAME=$(basename -- "$SOURCE_DIR")
[ -d "$PARENT_DIR" ] ||
	fail "configured source parent does not exist: $PARENT_DIR"
PARENT_DIR=$(CDPATH= cd -- "$PARENT_DIR" && pwd -P)
SOURCE_DIR="$PARENT_DIR/$SOURCE_NAME"

probe_root=$PARENT_DIR
if [ "$REQUIRE_SOURCE" -eq 1 ]; then
	[ -d "$SOURCE_DIR" ] || fail "configured Linux source is missing: $SOURCE_DIR"
	probe_root=$SOURCE_DIR
fi
probe=''
cleanup_probe()
{
	[ -n "$probe" ] || return 0
	rm -f -- "$probe/probe"
	rmdir -- "$probe" 2>/dev/null || true
}
trap cleanup_probe EXIT
trap 'exit 1' HUP INT TERM
probe=$(mktemp -d "$probe_root/.uml-case-sensitive.XXXXXX") ||
	fail "cannot create a case-sensitivity probe below $probe_root"
if ! : > "$probe/probe"; then
	fail "cannot write a case-sensitivity probe below $probe_root"
fi
if [ -e "$probe/PROBE" ]; then
	fail "$probe_root is case-insensitive; configure a case-sensitive APFS volume or disk image"
fi
rm -f -- "$probe/probe"
rmdir -- "$probe" || fail "cannot remove case-sensitivity probe below $probe_root"
probe=''
trap - EXIT HUP INT TERM

printf '%s\n' "$SOURCE_DIR"
