#!/usr/bin/env python3
"""Real guest tests; host CLI results are not physical-iOS certification."""
import argparse, json, os, pathlib, subprocess, time
p = argparse.ArgumentParser()
p.add_argument('--runtime', required=True); p.add_argument('--rootfs', required=True)
p.add_argument('--report', default='build/tests/runtime-smoke.json')
a = p.parse_args(); runtime = str(pathlib.Path(a.runtime).resolve()); root = str(pathlib.Path(a.rootfs).resolve())
report = []; env = os.environ.copy()
def run(name, args, expected=0, contains=None):
    started = time.monotonic()
    try:
        result = subprocess.run([runtime, '-f', root, *args], env=env, capture_output=True, text=True, timeout=60)
        ok = (result.returncode == expected if expected is not None else result.returncode != 0)
        if contains is not None: ok = ok and contains in result.stdout
        row = dict(name=name, passed=ok, status=result.returncode, seconds=time.monotonic()-started, stdout=result.stdout[-3000:], stderr=result.stderr[-2000:])
    except subprocess.TimeoutExpired:
        row = dict(name=name, passed=False, error='timeout', seconds=time.monotonic()-started)
    report.append(row); print(('PASS' if row['passed'] else 'FAIL') + ': ' + name)
    return row
run('uname', ['/bin/uname', '-m'], contains='aarch64')
run('echo', ['/bin/echo', 'LinuxRunner'], contains='LinuxRunner')
run('basic-files-and-shell', ['/bin/sh', '-ec', '''d=/tmp/linuxrunner-smoke; mkdir -p "$d"; cd "$d"; pwd; touch empty; echo persisted > source; cat source; cp source copy; mv copy moved; ls; test "$(cat moved)" = persisted; rm source moved empty; cd /; rmdir "$d"; sh -c 'echo shell-ok' ''' ], contains='shell-ok')
run('persistence-write', ['/bin/sh', '-ec', 'echo runtime-restart > /root/linuxrunner-smoke-persistence'])
run('persistence-after-restart', ['/bin/cat', '/root/linuxrunner-smoke-persistence'], contains='runtime-restart')
run('apk-version', ['/sbin/apk', '--version'], contains='apk-tools')
probe = subprocess.run([runtime, '-f', root, '/bin/sh', '-c', 'command -v python3'], capture_output=True, text=True, timeout=60)
if probe.returncode == 0: run('python-version', ['/usr/bin/python3', '--version'], contains='Python')
else: report.append(dict(name='python-version', skipped=True, reason='not in bundled minirootfs'))
run('missing-command', ['/bin/sh', '-c', 'linuxrunner-command-that-does-not-exist'], expected=127)
run('invalid-executable-create', ['/bin/sh', '-ec', 'echo invalid > /tmp/linuxrunner-invalid; chmod +x /tmp/linuxrunner-invalid'])
run('invalid-executable', ['/tmp/linuxrunner-invalid'], expected=None)
run('moderate-memory-and-process-churn', ['/bin/sh', '-ec', 'dd if=/dev/zero of=/tmp/linuxrunner-memory bs=1048576 count=8; i=0; while [ "$i" -lt 20 ]; do /bin/echo churn >/dev/null; i=$((i+1)); done; rm /tmp/linuxrunner-memory'])
exchange = pathlib.Path(a.report).resolve().parent / 'exchange'; exchange.mkdir(parents=True, exist_ok=True)
(exchange / 'import.txt').write_text('host-import\n')
env['ISH_BIND_MOUNTS'] = '/mnt/linuxrunner-exchange=' + str(exchange)
run('bind-import', ['/bin/cat', '/mnt/linuxrunner-exchange/import.txt'], contains='host-import')
run('guest-write-for-export', ['/bin/sh', '-ec', 'echo guest-export > /mnt/linuxrunner-exchange/export.txt'])
report.append(dict(name='host-export-content', passed=(exchange/'export.txt').read_text() == 'guest-export\n'))
run('cleanup', ['/bin/rm', '-f', '/root/linuxrunner-smoke-persistence', '/tmp/linuxrunner-invalid'])
path = pathlib.Path(a.report); path.parent.mkdir(parents=True, exist_ok=True)
path.write_text(json.dumps(dict(platform='Darwin ARM64 host CLI', results=report), indent=2))
raise SystemExit(any(not item.get('passed', item.get('skipped', False)) for item in report))
