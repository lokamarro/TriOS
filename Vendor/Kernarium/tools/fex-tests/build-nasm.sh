#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

fail()
{
	printf '%s\n' "tools/fex-tests/build-nasm.sh: $*" >&2
	exit 1
}

[ "$#" -eq 2 ] || fail "usage: build-nasm.sh NASM_SOURCE OUTPUT_DIRECTORY"
case "$1:$2" in
/*:/*) source=$1; output=$2 ;;
*) fail "source and output paths must be absolute" ;;
esac
[ -d "$source/.git" ] || fail "source is not a prepared Git checkout"
[ ! -e "$output" ] || fail "output already exists: $output"

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
series=$(awk -F '\t' '$1 == "nasm" && $3 == "x86-conformance" { print $7 }' \
	"$root/dependencies/manifest.tsv")
[ -n "$series" ] && [ "$series" != - ] ||
	fail "dependency manifest has no maintained NASM series"
# A prepared source is identified by the tree its maintained series records.
expected=$(awk -F '\t' '$1 == "tree" { print $2 }' "$root/dependencies/$series")
[ -n "$expected" ] || fail "NASM patch series records no tree"
[ "$(git -C "$source" rev-parse 'HEAD^{tree}')" = "$expected" ] ||
	fail "NASM source is not the dependency-manifest commit with its series"
[ -z "$(git -C "$source" status --porcelain --untracked-files=all)" ] ||
	fail "NASM source checkout is dirty"

for tool in autoconf autoheader make perl tar; do
	command -v "$tool" >/dev/null 2>&1 || fail "missing build tool: $tool"
done

parent=$(dirname -- "$output")
mkdir -p -- "$parent"
work=$(mktemp -d "$parent/.nasm-build.XXXXXX")
stage=$(mktemp -d "$parent/.nasm-prefix.XXXXXX")
cleanup()
{
	rm -rf -- "$work"
	[ -z "$stage" ] || rm -rf -- "$stage"
}
trap cleanup EXIT HUP INT TERM

build_source="$work/source"
mkdir -p "$build_source"
git -C "$source" archive HEAD | tar -xf - -C "$build_source"
(CDPATH= cd -- "$build_source" && sh autogen.sh)
(CDPATH= cd -- "$build_source" && ./configure --prefix="$stage")
make -C "$build_source" \
	-j"$(sysctl -n hw.logicalcpu 2>/dev/null || printf 4)" nasm
mkdir -p "$stage/bin"
install -m 0755 "$build_source/nasm" "$stage/bin/nasm"
verify_nasm()
{
	"$1/bin/nasm" -v | grep -q '^NASM version 3\.02'
}
verify_nasm "$stage" || fail "built NASM has the wrong version"
# The exclusive rename never replaces, or moves below, a prefix that a
# concurrent build published first; that winner is verified and reused.
status=0
python3 -B "$root/dependencies/publish-prefix.py" "$stage" "$output" ||
	status=$?
case $status in
0)
	stage=
	printf '%s\n' "built pinned NASM at $output/bin/nasm"
	;;
1)
	verify_nasm "$output" ||
		fail "concurrently published NASM is invalid: $output"
	printf '%s\n' "reusing concurrently published NASM at $output/bin/nasm"
	;;
*) fail "could not atomically publish NASM: $output" ;;
esac
