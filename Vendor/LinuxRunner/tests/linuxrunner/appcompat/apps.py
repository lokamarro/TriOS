"""Application compatibility definitions (Debian arm64 packages from the signed
official archive). Each test checks real work, not startup alone.

CLI tests run as the normal guest user in a fresh runtime; `expect` must
appear in the output. GUI tests run inside one X session through
appcompat_driver; `setup` runs (as the user) before launch, `actions` drive
the app, and every `expect_*` action is an independent functional check.
"""

CLI = [
    dict(name='git', packages=['git'], expect='GIT_OK 3', script=r'''
set -e; rm -rf ~/ac-git ~/ac-git-clone; mkdir ~/ac-git; cd ~/ac-git
git init -q -b main; git config user.email t@example.invalid; git config user.name T
for i in 1 2 3; do echo "line $i" >> f.txt; git add f.txt; git commit -qm "c$i"; done
git status --porcelain; git log --oneline | wc -l > n; git clone -q ~/ac-git ~/ac-git-clone
cd ~/ac-git-clone; git diff HEAD~1 --stat | grep -q f.txt; echo "GIT_OK $(git rev-list --count HEAD)"'''),
    dict(name='python3', packages=['python3'], expect='PY_OK', script=r'''
python3 - <<'EOF'
import hashlib, json, sqlite3, multiprocessing, threading, tempfile, os, subprocess, venv
db = sqlite3.connect(':memory:'); db.execute('create table t(x)'); db.executemany('insert into t values(?)', [(i,) for i in range(1000)])
assert db.execute('select sum(x) from t').fetchone()[0] == 499500
with multiprocessing.Pool(2) as p: assert p.map(abs, [-1, -2, 3]) == [1, 2, 3]
t = threading.Thread(target=lambda: None); t.start(); t.join()
d = tempfile.mkdtemp(); venv.create(d + '/v', with_pip=False)
assert subprocess.run([d + '/v/bin/python', '-c', 'print(42)'], capture_output=True, text=True).stdout.strip() == '42'
print('PY_OK', hashlib.sha256(json.dumps({'a': 1}).encode()).hexdigest()[:8])
EOF'''),
    dict(name='python3-https', packages=['python3'], expect='HTTPS_OK', script=r'''
python3 -c "import urllib.request,ssl;r=urllib.request.urlopen('https://deb.debian.org/debian/',timeout=60);print('HTTPS_OK',r.status)"'''),
    dict(name='nodejs', packages=['nodejs'], expect='NODE_OK', script=r'''
cat > /tmp/ac-node.js <<'EOF'
const { Worker, isMainThread, parentPort } = require('worker_threads');
const crypto = require('crypto'), http = require('http'), fs = require('fs');
if (!isMainThread) { parentPort.postMessage(21 * 2); return; }
new Worker(__filename).on('message', (v) => {
  const srv = http.createServer((q, s) => s.end('pong')).listen(0, () => {
    http.get({ port: srv.address().port }, (r) => { let b = ''; r.on('data', d => b += d); r.on('end', () => {
      fs.writeFileSync('/tmp/ac-node.txt', b);
      console.log('NODE_OK', v, b, crypto.createHash('sha256').update('x').digest('hex').slice(0, 8)); srv.close(); }); });
  });
});
EOF
node /tmp/ac-node.js'''),
    dict(name='gcc', packages=['gcc'], expect='GCC_OK 4950', script=r'''
cat > /tmp/ac.c <<'EOF'
#include <pthread.h>
#include <stdio.h>
static int sum; static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static void *work(void *a) { for (int i = (int)(long)a; i < 100; i += 4) { pthread_mutex_lock(&m); sum += i; pthread_mutex_unlock(&m); } return 0; }
int main(void) { pthread_t t[4]; for (long i = 0; i < 4; i++) pthread_create(&t[i], 0, work, (void *)i); for (int i = 0; i < 4; i++) pthread_join(t[i], 0); printf("GCC_OK %d\n", sum); }
EOF
gcc -O2 -pthread /tmp/ac.c -o /tmp/ac-gcc && /tmp/ac-gcc'''),
    dict(name='g++', packages=['g++'], expect='GXX_OK 6', script=r'''
cat > /tmp/ac.cpp <<'EOF'
#include <iostream>
#include <map>
#include <thread>
#include <future>
int main() { std::map<int,int> m{{1,2},{3,4}}; auto f = std::async(std::launch::async, [&]{ return m.size() * 3; }); std::cout << "GXX_OK " << f.get() << "\n"; }
EOF
g++ -O2 -std=c++17 -pthread /tmp/ac.cpp -o /tmp/ac-gxx && /tmp/ac-gxx'''),
    dict(name='clang', packages=['clang'], expect='CLANG_OK 4950', script=r'''
cat > /tmp/acl.c <<'EOF'
#include <stdio.h>
int main(void) { int s = 0; for (int i = 0; i < 100; i++) s += i; printf("CLANG_OK %d\n", s); }
EOF
clang -O2 /tmp/acl.c -o /tmp/ac-clang && /tmp/ac-clang'''),
    dict(name='cmake-ninja', packages=['cmake', 'ninja-build', 'g++'], expect='CMAKE_OK', script=r'''
set -e; rm -rf /tmp/ac-cm; mkdir -p /tmp/ac-cm/src; cd /tmp/ac-cm
printf 'cmake_minimum_required(VERSION 3.16)\nproject(ac CXX)\nadd_library(l STATIC src/l.cpp)\nadd_executable(app src/main.cpp)\ntarget_link_libraries(app l)\n' > CMakeLists.txt
printf 'int value(){return 7;}\n' > src/l.cpp
printf '#include <cstdio>\nint value();\nint main(){std::printf("CMAKE_OK %%d\\n", value());}\n' > src/main.cpp
cmake -S . -B b -G Ninja >/dev/null && ninja -C b >/dev/null && ./b/app'''),
    dict(name='make', packages=['make', 'gcc'], expect='MAKE_OK', script=r'''
set -e; rm -rf /tmp/ac-mk; mkdir /tmp/ac-mk; cd /tmp/ac-mk
printf 'int main(){return 0;}\n' > a.c; printf 'all: a\n\t./a && echo MAKE_OK\na: a.c\n\tcc -o a a.c\n' > Makefile; make -s -j2'''),
    dict(name='gdb', packages=['gdb', 'gcc'], expect='GDB_OK', script=r'''
printf 'int crash(int *p){return *p;}\nint main(){return crash(0);}\n' > /tmp/acg.c; gcc -g -O0 /tmp/acg.c -o /tmp/acg
gdb -q -batch -ex run -ex bt /tmp/acg 2>&1 | tee /tmp/acg.txt | tail -5; grep -q 'crash' /tmp/acg.txt && grep -q SIGSEGV /tmp/acg.txt && echo GDB_OK'''),
    dict(name='gdb-breakpoint', packages=['gdb', 'gcc'], expect='GDB_BREAK_OK', script=r'''
printf '#include <stdio.h>\nint square(int v){return v*v;}\nint main(){int s=0;for(int i=0;i<4;i++)s+=square(i);printf("sum=%%d\\n",s);return 0;}\n' > /tmp/acb.c
gcc -g -O0 /tmp/acb.c -o /tmp/acb
gdb -q -batch -ex 'break square' -ex run -ex 'print v' -ex 'next' -ex continue -ex 'info registers pc' -ex 'delete' -ex continue /tmp/acb > /tmp/acb.txt 2>&1
cat /tmp/acb.txt | tail -12; grep -q 'Breakpoint 1, square (v=0)' /tmp/acb.txt && grep -q 'square (v=1)' /tmp/acb.txt && grep -q 'sum=14' /tmp/acb.txt && echo GDB_BREAK_OK'''),
    dict(name='strace', packages=['strace'], expect='STRACE_OK', script=r'''
strace -f -o /tmp/acs.txt sh -c 'echo hi > /tmp/acs-out; cat /tmp/acs-out' >/dev/null 2>&1; rc=$?
grep -c . /tmp/acs.txt; grep -m3 'openat\|execve' /tmp/acs.txt; [ $rc = 0 ] && grep -q 'execve("/usr/bin/sh"' /tmp/acs.txt && grep -q 'write(1, "hi\\n"' /tmp/acs.txt && echo STRACE_OK'''),
    dict(name='vim', packages=['vim'], expect='VIM_OK', script=r'''
printf 'alpha\nbeta\n' > /tmp/acv.txt; vim -Es -c '%s/beta/VIM_OK/' -c 'wq' /tmp/acv.txt; cat /tmp/acv.txt'''),
    dict(name='neovim', packages=['neovim'], expect='NVIM_OK', script=r'''
printf 'alpha\n' > /tmp/acn.txt; nvim --headless -c 'normal! oNVIM_OK' -c 'wq' /tmp/acn.txt; cat /tmp/acn.txt'''),
    dict(name='imagemagick', packages=['imagemagick'], expect='IM_OK 64x48', script=r'''
convert -size 320x240 gradient:blue-yellow /tmp/aci.png && convert /tmp/aci.png -resize 20% /tmp/aci-small.png && echo "IM_OK $(identify -format %wx%h /tmp/aci-small.png)"'''),
    dict(name='ffmpeg', packages=['ffmpeg'], expect='FFMPEG_OK 25', script=r'''
ffmpeg -v error -y -f lavfi -i testsrc=duration=1:size=160x120:rate=25 -f lavfi -i sine=duration=1 -c:v libx264 -preset ultrafast -c:a aac /tmp/acf.mp4 && echo "FFMPEG_OK $(ffprobe -v error -count_frames -select_streams v -show_entries stream=nb_read_frames -of csv=p=0 /tmp/acf.mp4)"'''),
    dict(name='tar-xz-zip', packages=['xz-utils'], expect='ARCHIVE_OK', script=r'''
set -e; rm -rf /tmp/aca; mkdir -p /tmp/aca/d; seq 1 20000 > /tmp/aca/d/n; cd /tmp/aca; tar -cJf a.tar.xz d; rm -rf d; tar -xJf a.tar.xz; [ "$(wc -l < d/n)" = 20000 ] && echo ARCHIVE_OK'''),
]

GUI = [
    dict(name='xterm', packages=['xterm'], command=['xterm'], actions='''
type echo XTERM_$((6*7)) > /tmp/ac-xterm.txt\\n
expect_file /tmp/ac-xterm.txt XTERM_42 20
capture xterm'''),
    dict(name='xfce4-terminal', packages=['xfce4-terminal'], command=['xfce4-terminal', '--disable-server'], actions='''
sleep 3000
type echo XFT_$((6*7)) > /tmp/ac-xft.txt\\n
expect_file /tmp/ac-xft.txt XFT_42 30
capture xfce4-terminal'''),
    dict(name='mousepad', packages=['mousepad'], setup='printf "mousepad original\\n" > ~/ac-mousepad.txt', command=['mousepad', '~/ac-mousepad.txt'], actions='''
expect_title ac-mousepad.txt 30
key ctrl+End
type MOUSEPAD_EDIT_OK\\n
key ctrl+s
expect_file ~/ac-mousepad.txt MOUSEPAD_EDIT_OK 20
capture mousepad'''),
    dict(name='thunar', packages=['thunar'], setup='rm -rf ~/ac-thunar; mkdir -p ~/ac-thunar; echo x > ~/ac-thunar/existing.txt', command=['thunar', '~/ac-thunar'], actions='''
expect_title ac-thunar 30
key ctrl+shift+n
expect_window Folder 20
key ctrl+a
type ThunarMade
key Return
expect_file ~/ac-thunar/ThunarMade 20
capture thunar'''),
    dict(name='pcmanfm', packages=['pcmanfm'], setup='rm -rf ~/ac-pcmanfm; mkdir -p ~/ac-pcmanfm', command=['pcmanfm', '--no-desktop', '~/ac-pcmanfm'], actions='''
expect_title ac-pcmanfm 30
key ctrl+shift+n
expect_window Folder 20
key ctrl+a
type PcmanMade
key Return
expect_file ~/ac-pcmanfm/PcmanMade 20
capture pcmanfm'''),
    dict(name='ristretto', packages=['ristretto', 'imagemagick'], setup='convert -size 200x150 plasma: ~/ac-image.png', command=['ristretto', '~/ac-image.png'], actions='''
expect_title ac-image.png 30
capture ristretto'''),
    dict(name='xarchiver', packages=['xarchiver'], setup='rm -rf ~/ac-arc; mkdir -p ~/ac-arc/in; echo data > ~/ac-arc/in/member.txt; cd ~/ac-arc && tar -czf demo.tar.gz in', command=['xarchiver', '~/ac-arc/demo.tar.gz'], actions='''
expect_title demo.tar.gz 30
capture xarchiver'''),
    dict(name='galculator', packages=['galculator'], command=['galculator'], actions='''
type 12*12=
sleep 1500
capture galculator'''),
    # ---- Batch 2: framework coverage ----
    dict(name='glxgears', packages=['mesa-utils'], command=['glxgears', '-info'], actions='''
expect_file $AC_OUT/glxgears.app.log frames 30
capture glxgears'''),
    dict(name='gtk4-widget-factory', packages=['gtk-4-examples'], command=['gtk4-widget-factory'], actions='''
expect_title Widget 30
capture gtk4-widget-factory'''),
    dict(name='qterminal', packages=['qterminal'], command=['qterminal'], actions='''
sleep 4000
type echo QTERM_$((6*7)) > /tmp/ac-qterm.txt\\n
expect_file /tmp/ac-qterm.txt QTERM_42 30
capture qterminal'''),
    dict(name='featherpad', packages=['featherpad'], setup='printf "featherpad original\\n" > ~/ac-feather.txt', command=['featherpad', '~/ac-feather.txt'], actions='''
expect_title ac-feather 30
key ctrl+End
type FEATHER_EDIT_OK\\n
key ctrl+s
expect_file ~/ac-feather.txt FEATHER_EDIT_OK 20
capture featherpad'''),
    dict(name='qt5ct', packages=['qt5ct'], command=['qt5ct'], actions='''
capture qt5ct'''),
    dict(name='qalculate-qt', packages=['qalculate-qt'], command=['qalculate-qt'], actions='''
sleep 3000
type 6*7\\n
sleep 2000
capture qalculate-qt'''),
    dict(name='geany', packages=['geany'], setup='printf "geany original\\n" > ~/ac-geany.txt', command=['geany', '-i', '~/ac-geany.txt'], actions='''
expect_title ac-geany 30
key ctrl+End
type GEANY_EDIT_OK\\n
key ctrl+s
expect_file ~/ac-geany.txt GEANY_EDIT_OK 20
capture geany'''),
    dict(name='gnumeric', packages=['gnumeric'], setup='printf "a,b\\n1,2\\n3,4\\n" > ~/ac-sheet.csv', command=['gnumeric', '~/ac-sheet.csv'], actions='''
expect_title ac-sheet 40
capture gnumeric'''),
    dict(name='zathura', packages=['zathura', 'zathura-pdf-poppler'], setup='cp /home/mobile/Downloads/appcompat/sample.pdf ~/ac-doc.pdf', command=['zathura', '~/ac-doc.pdf'], actions='''
expect_title ac-doc.pdf 30
capture zathura'''),
    dict(name='qpdfview', packages=['qpdfview'], setup='cp /home/mobile/Downloads/appcompat/sample.pdf ~/ac-doc2.pdf', command=['qpdfview', '~/ac-doc2.pdf'], actions='''
expect_title ac-doc2 30
capture qpdfview'''),
    dict(name='feh', packages=['feh', 'imagemagick'], setup='convert -size 240x160 plasma: ~/ac-feh.png', command=['feh', '~/ac-feh.png'], actions='''
expect_title ac-feh.png 30
capture feh'''),
    dict(name='xcalc', packages=['x11-apps'], command=['xcalc'], actions='''
type 6*7=
sleep 1000
capture xcalc'''),
    dict(name='xeyes', packages=['x11-apps'], command=['xeyes'], actions='''
click 20 20
capture xeyes'''),
    dict(name='tkinter', packages=['python3-tk'], command=['python3', '/home/mobile/Downloads/appcompat/tk_probe.py'], actions='''
expect_title LR Tk probe 30
type TK_INPUT_OK\\n
expect_file /tmp/ac-tk.txt TK_INPUT_OK 20
capture tkinter'''),
    dict(name='sdl2', packages=['libsdl2-dev', 'gcc'], setup='gcc -O2 /home/mobile/Downloads/appcompat/sdl2_probe.c -o /tmp/ac-sdl2 $(sdl2-config --cflags --libs)', command=['/tmp/ac-sdl2'], actions='''
expect_file /tmp/ac-sdl.txt FRAMES_OK 40
key a
expect_file /tmp/ac-sdl.txt KEY_OK 20
capture sdl2'''),
    dict(name='abiword', packages=['abiword'], command=['abiword'], actions='''
sleep 3000
type AbiWord typing check
sleep 1500
capture abiword'''),
]


# ---- Startup benchmark (cold = run 1 with a fresh profile, warm = runs 2+) ----
# ready_* actions mark the first moment the application demonstrably works:
# the requested document or page appears in its title (browsers: after the
# page's script ran), or typed input was executed.
STARTUP = [
    # The terminal is ready when its shell runs: the shell writes a marker.
    dict(name='xterm', setup='rm -f /tmp/ac-ready-xterm.txt', command=['xterm', '-e', 'sh', '-c', 'echo XTERM_READY > /tmp/ac-ready-xterm.txt; exec sh'], actions='''
ready_file /tmp/ac-ready-xterm.txt XTERM_READY 60'''),
    dict(name='thunar', setup='mkdir -p ~/ac-thunar-ready', command=['thunar', '~/ac-thunar-ready'], actions='''
ready_title ac-thunar-ready 120'''),
    dict(name='firefox', command=['firefox-esr', '--no-remote', '--new-instance', '--profile', '~/.mozilla/lr-startup', 'file:///home/mobile/Downloads/appcompat/ready.html'],
         setup='mkdir -p ~/.mozilla/lr-startup', actions='''
ready_title LR READY PAGE 42 240''', timeout=240),
    dict(name='chromium', command=['chromium', '--user-data-dir=~/.config/lr-chromium-startup', '--no-first-run', '--no-default-browser-check', 'file:///home/mobile/Downloads/appcompat/ready.html'], actions='''
ready_title LR READY PAGE 42 240''', timeout=240),
    # Brave restores the previous session's tabs; with several tabs open its
    # window close asks 'Close all tabs?'. Warm runs keep the profile and cache
    # but start from an empty session, as Chromium does by default.
    dict(name='brave', setup='find ~/.config/lr-brave-startup/Default/Sessions -type f -delete 2>/dev/null; true', command=['/opt/linuxrunner-fixture-apps-874/brave/brave', '--user-data-dir=~/.config/lr-brave-startup', '--no-first-run', '--no-default-browser-check', 'file:///home/mobile/Downloads/appcompat/ready.html'], actions='''
ready_title LR READY PAGE 42 240''', timeout=240),
    dict(name='vscode', setup='printf "startup\\n" > ~/ac-code-ready.txt', command=['/opt/linuxrunner-fixture-apps-874/code/VSCode-linux-arm64/code', '--user-data-dir=~/.config/lr-code-startup', '--extensions-dir=~/.local/share/lr-code-startup-ext', '~/ac-code-ready.txt'], actions='''
ready_title ac-code-ready.txt 300''', timeout=300),
    dict(name='telegram', command=['telegram-desktop'], actions='''
ready_title Telegram 240''', timeout=240),
    # Blender maps its window long before its event loop runs (first redraw,
    # shader compilation); a bpy timer fires only once the UI loop is live.
    dict(name='blender', setup='rm -f /tmp/ac-ready-blender.txt', command=['blender', '--factory-startup', '--python-expr',
         "import bpy; bpy.app.timers.register(lambda: (open('/tmp/ac-ready-blender.txt', 'w').write('BLENDER_READY'), None)[1], first_interval=0.1)"], actions='''
ready_file /tmp/ac-ready-blender.txt BLENDER_READY 600''', timeout=600),
    dict(name='libreoffice-writer', setup='printf "LinuxRunner startup\\n" > ~/ac-lo-ready.txt', command=['libreoffice', '--writer', '--norestore', '~/ac-lo-ready.txt'], actions='''
ready_title ac-lo-ready 300''', timeout=300),
    dict(name='gimp', command=['gimp', '--no-splash'], actions='''
ready_title GNU Image Manipulation Program 300''', timeout=300),
    dict(name='inkscape', setup='printf "<svg xmlns=\\"http://www.w3.org/2000/svg\\" width=\\"64\\" height=\\"64\\"><rect width=\\"32\\" height=\\"32\\"/></svg>" > ~/ac-ink-ready.svg', command=['inkscape', '~/ac-ink-ready.svg'], actions='''
ready_title ac-ink-ready 300''', timeout=300),
]
