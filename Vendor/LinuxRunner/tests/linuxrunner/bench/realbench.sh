#!/bin/sh
# Real-workload timings (ms, 5 runs each) for startup-heavy operations.
cd /tmp; printf 'int main(){return 0;}\n' > rb.c
[ -d rbrepo ] || { git init -q rbrepo; cd rbrepo; for i in $(seq 200); do echo $i > f$i; done; git add .; git -c user.email=x@x -c user.name=x commit -qm init; cd /tmp; }
t() { s=$(date +%s%N); sh -c "$1" >/dev/null 2>&1; e=$(date +%s%N); echo $(( (e-s)/1000000 )); }
[ -f rb.bin ] || head -c 4000000 /usr/lib/aarch64-linux-gnu/libc.so.6 /usr/bin/python3* 2>/dev/null | head -c 4000000 > rb.bin
for name in ${REALBENCH_ONLY:-py_import node_start gcc_hello git_status ls_lR_py py_loop gzip_4mb sha256_4mb py_sqlite}; do
  case $name in
    py_import) cmd='python3 -c "import json,sqlite3,asyncio,email,http.client"';;
    node_start) cmd='node -e 0';;
    gcc_hello) cmd='gcc -O2 rb.c -o rb';;
    git_status) cmd='git -C rbrepo status';;
    ls_lR_py) cmd='ls -lR /usr/lib/python3.13';;
    # CPU-bound interpreter work: bytecode loop, compression, hashing, SQLite.
    py_loop) cmd='python3 -c "s=0
for i in range(300000): s+=i*i%7"';;
    gzip_4mb) cmd='gzip -c rb.bin';;
    sha256_4mb) cmd='sha256sum rb.bin';;
    py_sqlite) cmd='python3 -c "import sqlite3; c=sqlite3.connect(\":memory:\"); c.execute(\"create table t(a,b)\"); c.executemany(\"insert into t values(?,?)\", ((i,str(i)) for i in range(20000))); c.execute(\"select count(*),sum(a) from t where b like \x27%7%\x27\").fetchone()"';;
  esac
  r=""; for i in 1 2 3 4 5; do r="$r $(t "$cmd")"; done; echo "REAL $name ms:$r"
done
