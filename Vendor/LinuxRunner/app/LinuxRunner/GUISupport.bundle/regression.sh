#!/bin/sh
set -eu
cd "$(dirname "$0")"
echo 'REGRESSION: Linux ARM64 CLI, persistent filesystem, guest compiler, DNS/HTTPS/Python'
uname -m
[ "$(uname -m)" = aarch64 ]
apk --version
mkdir -p /root/linuxrunner-regression
printf 'persistent-storage\n' > /root/linuxrunner-regression/persistent.txt
cat > /root/linuxrunner-regression/hello.c <<'C'
#include <stdio.h>
int main(void){puts("new-arm64-elf-ok");return 0;}
C
gcc /root/linuxrunner-regression/hello.c -O2 -o /root/linuxrunner-regression/hello
readelf -h /root/linuxrunner-regression/hello | grep AArch64
readelf -l /root/linuxrunner-regression/hello | grep ld-musl-aarch64
[ "$(/root/linuxrunner-regression/hello)" = new-arm64-elf-ok ]
gcc network-probe.c -O2 -Wall -Wextra -o network-probe
./network-probe
command -v python3 >/dev/null || { echo 'REGRESSION: Python not installed; install python3 normally using apk'; exit 2; }
python3 - <<'PY'
import socket,urllib.request,pathlib
print('REGRESSION: Python DNS',bool(socket.getaddrinfo('dl-cdn.alpinelinux.org',443)))
with urllib.request.urlopen('https://dl-cdn.alpinelinux.org/alpine/v3.24/main/aarch64/APKINDEX.tar.gz',timeout=30) as r:
    assert r.status == 200 and r.read(2) == b'\x1f\x8b'
p=pathlib.Path('/root/linuxrunner-regression/python.txt');p.write_text('python-filesystem-ok');assert p.read_text() == 'python-filesystem-ok'
for family,address in [(socket.AF_INET,('127.0.0.1',0)),(socket.AF_INET6,('::1',0))]:
    server=socket.socket(family);server.bind(address);server.listen(1)
    peer=socket.socket(family);peer.connect(server.getsockname());connection,_=server.accept()
    peer.sendall(b'loopback-ok');assert connection.recv(11)==b'loopback-ok';connection.close();peer.close();server.close()
print('REGRESSION: Python HTTPS/filesystem, IPv4/IPv6 localhost passed')
PY
echo 'REGRESSION: PASS (verify persistent.txt after app restart separately)'
