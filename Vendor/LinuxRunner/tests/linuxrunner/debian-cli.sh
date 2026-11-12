#!/bin/bash
set -eu
cat /etc/os-release
uname -a
id
pwd
/bin/bash --version | head -1
dpkg --version | head -1
apt --version
gcc --version | head -1
g++ --version | head -1
make --version | head -1
git --version
cat >/tmp/debian-c.c <<'C'
#include <stdio.h>
#include <gnu/libc-version.h>
#include <pthread.h>
static void *work(void *p){return p;}
int main(void){pthread_t t;void *v; if(pthread_create(&t,0,work,(void*)42)||pthread_join(t,&v)||v!=(void*)42)return 1;printf("DEBIAN_C: glibc=%s threads=PASS\n",gnu_get_libc_version());return 0;}
C
gcc /tmp/debian-c.c -pthread -O2 -o /tmp/debian-c
readelf -l /tmp/debian-c | grep 'ld-linux-aarch64.so.1'
/tmp/debian-c
cat >/tmp/debian-cpp.cpp <<'CPP'
#include <future>
#include <iostream>
#include <vector>
int main(){std::vector<int> v={1,2,3}; auto f=std::async(std::launch::async,[&]{int n=0; for(int i:v)n+=i; return n;});if(f.get()!=6)return 1;std::cout<<"DEBIAN_CPP: C++17 async=PASS\n";}
CPP
g++ /tmp/debian-cpp.cpp -std=c++17 -pthread -O2 -o /tmp/debian-cpp
readelf -l /tmp/debian-cpp | grep 'ld-linux-aarch64.so.1'
/tmp/debian-cpp
python3 - <<'PY'
import os,socket,urllib.request,ssl
p='/root/linuxrunner-debian-persistence-test.txt'
open(p,'w').write('Debian persistent state\n')
assert open(p).read()=='Debian persistent state\n'
for domain in ('example.com','etsbi.edu.ba'):
 assert socket.getaddrinfo(domain,443)
 with urllib.request.urlopen('https://'+domain+'/',timeout=45,context=ssl.create_default_context()) as r:
  data=r.read(512);assert r.status==200 and data;print('DEBIAN_PYTHON: DNS/TLS/HTTPS',domain,'PASS',r.status)
print('DEBIAN_PYTHON: filesystem=PASS')
PY
curl -I --max-time 45 https://example.com/
wget -q --spider --timeout=45 https://example.com/
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends hello
hello
DEBIAN_FRONTEND=noninteractive apt-get remove -y hello
! dpkg-query -W -f='${Status}' hello 2>/dev/null | grep -q 'install ok installed'
[ -z "$(dpkg --audit)" ]
echo 'DEBIAN_CLI: PASS'
