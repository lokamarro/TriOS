#!/usr/bin/env python3
"""Arch host acceptance on an explicitly supplied DISPOSABLE fakefs fixture.
Does not claim iOS UI/device coverage. --network permits signed pacman writes.
"""
import argparse, fcntl, json, os, pathlib, shutil, subprocess
p = argparse.ArgumentParser(description=__doc__)
for key in ('runtime', 'rootfs', 'exchange', 'output'): p.add_argument('--' + key, required=True)
p.add_argument('--network', action='store_true')
a = p.parse_args()
root = pathlib.Path(__file__).resolve().parents[3]
out = pathlib.Path(a.output).resolve(); out.mkdir(parents=True, exist_ok=True)
ex = pathlib.Path(a.exchange).resolve(); ex.mkdir(parents=True, exist_ok=True)
lease = (pathlib.Path(a.rootfs) / '.host-gui-test.lock').open('a')
fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
shutil.copytree(root / 'app/LinuxRunner/GUISupport.bundle', ex / '.linuxrunner/gui', dirs_exist_ok=True)
for src in (root / 'tests/linuxrunner/arch').glob('*.sh'): shutil.copy2(src, ex)
shutil.copy2(root / 'tests/linuxrunner/pty_shell_contract.c', ex)
shutil.copy2(root / 'tests/linuxrunner/termios2_contract.c', ex)
for name in ('package-manager.sh', 'package/package_lock_wait.sh'):
    shutil.copy2(root / 'tests/linuxrunner' / name, ex / pathlib.Path(name).name)
env = dict(os.environ, ISH_BIND_MOUNTS='/home/mobile/Downloads=' + str(ex))
base = [str(pathlib.Path(a.runtime).resolve()), '-f', str(pathlib.Path(a.rootfs).resolve())]
results = []
def record(name, rc, passed, stdout='', stderr=''):
    (out / (name + '.stdout')).write_text(stdout)
    (out / (name + '.stderr')).write_text(stderr)
    results.append(dict(name=name, exit=rc, status='PASS' if passed else 'FAIL'))
    (out / 'summary.json').write_text(json.dumps(dict(environment='macOS ARM64 guest interpreter; iOS UI NOT TESTED', stages=results), indent=2) + '\n')
    print(name, results[-1]['status'], 'exit=' + str(rc), flush=True)
def run(name, args, expected=0, contains='', timeout=120, input=''):
    try:
        r = subprocess.run(base + args, env=env, capture_output=True, text=True, input=input, timeout=timeout)
        record(name, r.returncode, r.returncode == expected and contains in r.stdout, r.stdout, r.stderr)
    except subprocess.TimeoutExpired as e:
        record(name, 124, False, (e.stdout or b'').decode(errors='replace'), (e.stderr or b'').decode(errors='replace'))
run('echo', ['/bin/echo', 'hello'], contains='hello')
run('shell', ['/bin/sh', '-c', 'echo shell-ok; echo stderr-ok >&2; exit 23'], expected=23, contains='shell-ok')
run('bash', ['/bin/bash', '-lc', 'echo bash-ok'], contains='bash-ok')
run('identity', ['/usr/bin/id'], contains='uid=0')
run('architecture', ['/usr/bin/uname', '-m'], contains='aarch64')
run('pacman-version', ['/usr/bin/pacman', '--version'], contains='Pacman')
run('package-recovery', ['/bin/sh', '/home/mobile/Downloads/arch_package_recovery.sh'], contains='ARCH_PACKAGE_RECOVERY_TEST PASS')
run('managed-package-tests', ['/bin/sh', '/home/mobile/Downloads/package-manager.sh'], contains='PACKAGE_MANAGER_PASS', timeout=180)
run('installer-serialization', ['/bin/sh', '/home/mobile/Downloads/package_lock_wait.sh'], contains='PACKAGE_LOCK_WAIT_TEST PASS', timeout=90)
if a.network:
    run('pacman-sync', ['/bin/sh', '/home/mobile/Downloads/.linuxrunner/gui/package-manager.sh', 'update'], timeout=240)
    run('pacman-install', ['/usr/bin/pacman', '-Syu', '--noconfirm', '--needed', 'bc'], timeout=600)
    run('pacman-signature', ['/bin/sh', '-ec', 'pacman -Qi bc | grep "Validated By.*Signature"; bc --version'], contains='Signature')
    run('pacman-repeat', ['/usr/bin/pacman', '-S', '--noconfirm', 'bc'], timeout=180)
    run('database-check', ['/usr/bin/pacman', '-Dk'], contains='No database errors')
    run('desktop-profile-repair', ['/bin/sh', '/home/mobile/Downloads/.linuxrunner/gui/installer-run.sh', 'desktop-profile.sh', 'repair'], contains='PROFILE: result=PASS', timeout=900)
run('guest-pty', ['/bin/sh', '-ec', 'cd /home/mobile/Downloads; gcc -O2 pty_shell_contract.c -o pty-shell; ./pty-shell'], contains='pty_shell_contract PASS')
run('termios2', ['/bin/sh', '-ec', 'cd /home/mobile/Downloads; gcc -O2 -Wall -Wextra termios2_contract.c -o termios2-contract; ./termios2-contract'], contains='TERMIOS2_CONTRACT PASS')
for attempt in (1, 2):
    run('terminal-roundtrip-' + str(attempt), ['/home/mobile/Downloads/pty-shell', '/bin/bash', '-l'], contains='pty_shell_contract PASS')
raise SystemExit(any(r['status'] == 'FAIL' for r in results))
