#!/bin/sh
# macro.sh — real-program workloads inside the guest; prints "MACRO <name> ms=<t> check=<c>"
cd /tmp
t() { name=$1; shift; s=$(date +%s%N); out=$("$@" 2>&1 | sha256sum | cut -c1-12); e=$(date +%s%N); echo "MACRO $name ms=$(( (e - s) / 1000000 )) check=$out"; }
[ -f /tmp/lrbench-data ] || head -c 4000000 /usr/bin/gcc-14 > /tmp/lrbench-data 2>/dev/null || head -c 4000000 /usr/lib/x86_64-linux-gnu/libc.so.6 > /tmp/lrbench-data
t python_start python3 -c 'pass'
t python_compute python3 -c 'print(sum(i*i % 7 for i in range(300000)))'
t python_json python3 -c 'import json; d=[{"k":i,"v":str(i)*3} for i in range(20000)]; print(len(json.dumps(json.loads(json.dumps(d)))))'
t gcc_compile gcc -O2 -c /mnt/bench/interp_bench.c -o /tmp/lrbench.o
t perl_regex perl -e 'my $s = "abc123def456" x 20000; my $n = () = $s =~ /\d+/g; print "$n\n"'
t gzip gzip -6 -c /tmp/lrbench-data
t xz xz -1 -c /tmp/lrbench-data
t sort sh -c 'seq 1 200000 | sort -r | sha256sum'
t sha256 sha256sum /tmp/lrbench-data
t git_hash sh -c 'git hash-object /tmp/lrbench-data'
t node_start node -e 'console.log(1+1)'
