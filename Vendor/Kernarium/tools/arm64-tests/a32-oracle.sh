#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the AArch32 corpus oracle (a32_oracle.cpp) for armhf with the pinned
# clang and lld, and run it on a host that executes AArch32 programs: a
# machine with AArch32 EL0 and a 64-bit kernel with compat support, or one
# that runs `qemu-arm` (decision D2: the oracle there is labelled "qemu").
# The oracle is copied, with the request, into a fresh private directory
# $HOME/[DIRECTORY/]NAME.XXXXXX on the host, run there, its response fetched
# and the directory removed, so nothing stays on the host.
#
#   a32-oracle.sh [--prefix NAME] [--directory DIRECTORY]
#                 [--runner 'qemu-arm -cpu max'] HOST REQUEST OUTPUT
#
# --directory names an existing directory under the host's home to work in,
# for a host whose owner confines visitors to one. --runner is the command
# the oracle runs under on the host ("" runs it natively). Every ssh and
# transfer is time-bounded.

set -euo pipefail

prefix=a32-oracle
parent=
runner=
while [[ $# -gt 0 ]]; do
	case "$1" in
	--prefix) prefix=$2; shift 2 ;;
	--directory) parent=$2/; shift 2 ;;
	--runner) runner=$2; shift 2 ;;
	--) shift; break ;;
	-*) echo "unknown option $1" >&2; exit 64 ;;
	*) break ;;
	esac
done
if [[ $# -ne 3 || ! $prefix =~ ^[A-Za-z0-9._-]+$ || ! $parent =~ ^([A-Za-z0-9._-]+/)*$
	|| $parent == *../* ]]; then
	echo "usage: $0 [--prefix NAME] [--directory DIRECTORY] [--runner COMMAND] HOST REQUEST OUTPUT" >&2
	exit 64
fi
host=$1
request=$2
output=$3
here="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
llvm=${A32_ORACLE_LLVM:-/opt/homebrew/opt/llvm@22}
[[ -f $request ]] || { echo "missing request" >&2; exit 66; }

work=$(mktemp -d "${TMPDIR:-/tmp}/a32-oracle-build.XXXXXX")
directory=
cleanup() {
	rm -rf -- "$work"
	if [[ -n $directory ]]; then
		timeout 60 ssh -o ConnectTimeout=15 -o BatchMode=yes "$host" \
			"case '$directory' in \"\$HOME\"/$parent$prefix.*) rm -rf -- '$directory' ;; esac" \
			</dev/null || echo "could not remove $host:$directory" >&2
	fi
}
trap cleanup EXIT

flags=(--target=armv7a-linux-gnueabihf -marm -mfloat-abi=soft -O2 -ffreestanding
	-fno-builtin -fno-stack-protector -fno-exceptions -fno-rtti -fno-unwind-tables
	-fno-asynchronous-unwind-tables -nostdlib)
"$llvm/bin/clang++" "${flags[@]}" -std=c++20 -c "$here/a32_oracle.cpp" -o "$work/a32_oracle.o"
"$llvm/bin/clang" "${flags[@]}" -c "$here/a32_oracle_entry.S" -o "$work/a32_oracle_entry.o"
"$llvm/bin/clang" --target=armv7a-linux-gnueabihf -nostdlib -static -fuse-ld=lld \
	-Wl,-T,"$here/a32_oracle.ld" -Wl,-z,max-page-size=4096 -Wl,-z,nognustack \
	-Wl,--defsym=A32_LAUNCH=0x4f003f80 "$work/a32_oracle_entry.o" "$work/a32_oracle.o" \
	-o "$work/a32_oracle"
chmod +x "$work/a32_oracle"

ssh_options=(-o ConnectTimeout=15 -o BatchMode=yes)
remote() {
	timeout "$1" ssh "${ssh_options[@]}" "$host" "${@:2}" </dev/null
}
directory=$(remote 60 "mktemp -d \"\$HOME/$parent$prefix.XXXXXX\"")
[[ $directory == /* ]] || { echo "bad remote directory" >&2; exit 70; }
timeout 600 scp -q "${ssh_options[@]}" "$work/a32_oracle" "$request" "$host:$directory/" </dev/null
remote 3000 "CDPATH= cd -- '$directory' && chmod +x a32_oracle && $runner ./a32_oracle '$(basename "$request")' response"
timeout 600 scp -q "${ssh_options[@]}" "$host:$directory/response" "$output" </dev/null
