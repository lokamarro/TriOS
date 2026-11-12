#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Loader-like guest corpus: real rootfs programs whose work resembles an
# application's load phase (decompression, dynamic linking, interpreter and
# class-loader start-up, parsing, allocator churn). Runs inside the Debian
# arm64 rootfs guest, prepares its inputs from rootfs files, runs every
# workload once to warm caches, then times CORPUS_ROUNDS rounds against
# /proc/uptime and prints one line per workload and round:
#
#   CORPUS:<name>:<round>:<runs>:<centiseconds>
#
# A failing run prints CORPUS_FAIL:<name>:<status> and skips the rest of that
# workload's round. A workload the rootfs cannot run, such as the C front end
# on an image without cc1, prints CORPUS_SKIP:<name> once and is left out.
# CORPUS_ONLY=name,name selects workloads.

set +e
rounds=${CORPUS_ROUNDS:-3}
only=${CORPUS_ONLY:-}
work=${CORPUS_DIR:-/tmp/loader-corpus}
lib=/usr/lib/aarch64-linux-gnu

mkdir -p "$work" && CDPATH= cd -- "$work" || exit 1
now() { cut -d' ' -f1 /proc/uptime | tr -d . | sed 's/^0*//; s/^$/0/'; }
wanted() {
	[ -z "$only" ] && return 0
	case ",$only," in *",$1,"*) return 0 ;; esac
	return 1
}

if [ ! -s words ]; then
	head -c 8388608 "$lib/libLLVM.so.19.1" > llvm8
	gzip -6 -c llvm8 > llvm8.gz
	bzip2 -9 -c llvm8 > llvm8.bz2
	xz -2 -c llvm8 > llvm8.xz
	strings -n 6 "$lib/libLLVM.so.19.1" | head -200000 > words
	head -c 1000000 words > words1m
	printf '#include <%s.h>\n' stdio stdlib string pthread signal math \
		unistd fcntl time locale wchar regex netdb arpa/inet dirent \
		sys/socket sys/mman sys/stat netinet/in > headers.c
	rm -f llvm8
fi

# name runs command...
gunzip() { gzip -dc llvm8.gz; }
unxz() { xz -dc llvm8.xz; }
bunzip2() { bzip2 -dc llvm8.bz2; }
ldso() {
	LD_TRACE_LOADED_OBJECTS=1 LD_WARN=yes LD_BIND_NOW=yes \
		/lib/ld-linux-aarch64.so.1 "$lib/libQt6WebEngineCore.so.6"
}
pyimport() {
	python3 -c 'import json, email.parser, http.client, xml.dom.minidom, decimal, argparse, unittest, asyncio'
}
sorting() { sort -o /dev/null words; }
regex() { sed -E 's/([A-Za-z]+)_([A-Za-z0-9]+)/\2_\1/g; s/[0-9]+/N/g' words1m; }
compile() { gcc -O1 -fsyntax-only headers.c; }
perlhash() {
	perl -e 'my %h; for my $i (1..150000) { my $s = "k" . ($i * 7919 % 100003);
		$h{$s} .= "x" x ($i % 97); delete $h{"k" . ($i % 5000)} if $i % 3 == 0 }'
}
javaint() { java -Xint -Xshare:off -version; }
nodejs() {
	node -e "require('zlib').inflateSync(require('zlib').deflateSync(Buffer.alloc(1<<22,7)))"
}

available() {
	case $1 in
	compile) [ -x "$(gcc -print-prog-name=cc1 2>/dev/null)" ] ;;
	*) return 0 ;;
	esac
}

workloads="gunzip:10 unxz:5 bunzip2:2 ldso:8 pyimport:3 sorting:10 regex:2 compile:5 perlhash:2 javaint:4 nodejs:4"

active=
for item in $workloads; do
	name=${item%%:*}
	wanted "$name" || continue
	if available "$name"; then
		active="$active $item"
	else
		echo "CORPUS_SKIP:$name"
	fi
done
for item in $active; do
	"${item%%:*}" > /dev/null 2>&1
done
round=0
while [ "$round" -lt "$rounds" ]; do
	for item in $active; do
		name=${item%%:*} runs=${item#*:}
		start=$(now) i=0 status=0
		while [ "$i" -lt "$runs" ]; do
			"$name" > /dev/null 2>&1
			status=$?
			[ "$status" -eq 0 ] || break
			i=$((i + 1))
		done
		if [ "$status" -ne 0 ]; then
			echo "CORPUS_FAIL:$name:$status"
			continue
		fi
		echo "CORPUS:$name:$round:$runs:$(($(now) - start))"
	done
	round=$((round + 1))
done
