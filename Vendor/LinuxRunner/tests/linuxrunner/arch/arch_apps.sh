#!/bin/sh
# Arch Linux ARM application compatibility inside LinuxRunner (run as root on an
# image that passed arch_firstboot.sh): installs development tools, editors and
# desktop applications with pacman, then exercises the command-line ones.
# GUI applications are launched by arch_apps_gui.sh inside a desktop session.
# Prints ARCH_APP <name> PASS|FAIL lines and ARCH_APPS_TEST PASS|FAIL.
fails=0
check() { name=$1; shift; if "$@" >/tmp/arch-app.out 2>&1; then echo "ARCH_APP $name PASS"; else echo "ARCH_APP $name FAIL"; sed 's/^/    /' /tmp/arch-app.out | tail -15; fails=$((fails + 1)); fi; }
packages="git python gcc clang cmake make vim nano mousepad feh thunar firefox chromium xorg-xwininfo"
check pacman-install /bin/sh -c "pacman -Sy --noconfirm >/dev/null && pacman -S --needed --noconfirm $packages"
work=$(mktemp -d /tmp/arch-apps.XXXXXX)
check git /bin/sh -c "cd $work && git init -q repo && cd repo && git config user.email t@example.invalid && git config user.name t && echo hi > f && git add f && git commit -qm one && git log --oneline | grep -q one"
check python /usr/bin/python3 -c 'import json, sqlite3, ssl, hashlib; c = sqlite3.connect(":memory:"); c.execute("create table t(x)"); c.execute("insert into t values (41)"); assert c.execute("select x + 1 from t").fetchone()[0] == 42; print(ssl.OPENSSL_VERSION)'
printf '#include <stdio.h>\nint main(void){printf("ok %%d\\n", 6 * 7); return 0;}\n' > "$work/hello.c"
check gcc /bin/sh -c "gcc -O2 $work/hello.c -o $work/hello-gcc && $work/hello-gcc | grep -q 'ok 42'"
check clang /bin/sh -c "clang -O2 $work/hello.c -o $work/hello-clang && $work/hello-clang | grep -q 'ok 42'"
mkdir -p "$work/cm"; cp "$work/hello.c" "$work/cm/"; printf 'cmake_minimum_required(VERSION 3.20)\nproject(h C)\nadd_executable(h hello.c)\n' > "$work/cm/CMakeLists.txt"
check cmake /bin/sh -c "cmake -S $work/cm -B $work/cm/build >/dev/null && cmake --build $work/cm/build >/dev/null && $work/cm/build/h | grep -q 'ok 42'"
check vim /bin/sh -c "printf 'one\n' > $work/v.txt && vim -Es -c '%s/one/two/' -c wq $work/v.txt; grep -qx two $work/v.txt"
check nano nano --version
for b in mousepad feh thunar firefox chromium xwininfo; do check "$b-installed" command -v $b; done
echo "ARCH_APPS_TEST $([ $fails = 0 ] && echo PASS || echo FAIL) failures=$fails"
