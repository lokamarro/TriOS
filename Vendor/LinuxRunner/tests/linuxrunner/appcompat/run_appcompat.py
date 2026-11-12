#!/usr/bin/env python3
"""Automated ARM64 Linux application compatibility runner (Mac fixture).

  run_appcompat.py --runtime ISH --rootfs ROOTFS --exchange DIR --output DIR
                   [--suite cli|gui|all] [--only name,...] [--repeat N]

CLI tests (apps.CLI): one fresh runtime per test, run as the normal guest user
(lrabi_gui) through su; records exit status, wall time, the expected output
marker, the runtime compatibility census (/proc/ish/compat: missing/stub
syscalls, ENOTTY ioctls, faults) and host descriptor counts.
GUI tests (apps.GUI): one X session (run_guest_session.py with the passive
RFB probe); appcompat_driver launches each app, measures time to first
window, runs its functional actions and closes it (WM_DELETE_WINDOW), then
reports clean/forced shutdown and leftover processes.
Results: OUTPUT/results.json and a summary table. Mac fixture only: never a
physical-iPhone result.
"""
import argparse, json, os, pathlib, re, resource, shlex, shutil, subprocess, sys, time

here = pathlib.Path(__file__).resolve().parent
repo = here.parents[2]
sys.path.insert(0, str(here))
import apps  # noqa: E402

p = argparse.ArgumentParser()
for key in ['runtime', 'rootfs', 'exchange', 'output']: p.add_argument('--' + key, required=True)
p.add_argument('--suite', choices=['cli', 'gui', 'all', 'startup'], default='all')
p.add_argument('--application-bundles', help='Host directory mounted at /opt/linuxrunner-fixture-apps-874 (Brave, VS Code)')
p.add_argument('--only', default='')
p.add_argument('--repeat', type=int, default=1)
p.add_argument('--vnc-timing', default='default', help='x11vnc update policy (run_guest_session --vnc-timing)')
p.add_argument('--host-nofile', type=int, default=2560, help='Inherited host soft limit (phone default 2560)')
p.add_argument('--user', default='lrabi_gui')
p.add_argument('--probe', help='Passive RFB probe binary (built from gui/passive_session_probe.m)')
a = p.parse_args()
runtime = pathlib.Path(a.runtime).resolve(); rootfs = pathlib.Path(a.rootfs).resolve()
exchange = pathlib.Path(a.exchange).resolve(); out = pathlib.Path(a.output).resolve(); out.mkdir(parents=True, exist_ok=True)
only = set(filter(None, a.only.split(',')))
guest_dir = '/home/mobile/Downloads/appcompat'
host_dir = exchange / 'appcompat'; host_dir.mkdir(parents=True, exist_ok=True)

def limits():
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (min(a.host_nofile, hard), hard))

def guest(script, timeout, log):
    env = dict(os.environ, ISH_BIND_MOUNTS='/home/mobile/Downloads=' + str(exchange), ISH_ARM64_MEMORY_PC_FUSION=os.environ.get('LR_MEMORY_PC_FUSION', '0'))
    started = time.monotonic()
    try:
        r = subprocess.run([str(runtime), '-f', str(rootfs), '/bin/sh', '-c', script], env=env, preexec_fn=limits,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        text, code = r.stdout.decode(errors='replace'), r.returncode
    except subprocess.TimeoutExpired as e:
        text, code = (e.stdout or b'').decode(errors='replace'), 124
    log.write_text(text)
    return code, text, time.monotonic() - started

COMPAT = re.compile(r'COMPAT comm=(\S+) kind=(\S+) code=(\S+) aux=(\S+) count=(\d+)')
# ioctls every program probes (isatty and window size on non-terminals).
ROUTINE_IOCTLS = {'0x5401', '0x5413'}

def compat_events(text):
    events = []
    for comm, kind, code, aux, count in COMPAT.findall(text):
        if kind == 'ioctl_enotty' and code in ROUTINE_IOCTLS: continue
        events.append(dict(comm=comm, kind=kind, code=code, aux=aux, count=int(count)))
    return events

def host_fds(text):
    values = [int(v) for v in re.findall(r'FD_TREND .*?host_open=(\d+)', text)]
    return values[-1] if values else None

results = dict(environment='Mac ARM64 interpreter fixture (case-sensitive APFS rootfs); not physical iPhone',
               runtime=str(runtime), rootfs=str(rootfs), host_nofile=a.host_nofile, cli=[], gui=[])

def run_cli():
    for test in apps.CLI:
        if only and test['name'] not in only: continue
        (host_dir / (test['name'] + '.sh')).write_text(test['script'].lstrip('\n') + '\n')
        for rep in range(a.repeat):
            script = ('su - %s -s /bin/sh -c %s; echo "AC_RC=$?"; cat /proc/ish/compat; grep FD_TREND /proc/ish/fds'
                      % (a.user, shlex.quote('sh %s/%s.sh' % (guest_dir, test['name']))))
            log = out / ('cli-%s-%d.log' % (test['name'], rep + 1))
            code, text, seconds = guest(script, test.get('timeout', 600), log)
            rc = re.search(r'AC_RC=(\d+)', text)
            ok = rc is not None and rc.group(1) == '0' and test['expect'] in text
            faults = [l for l in text.splitlines() if l.startswith('GUEST_FAULT')]
            entry = dict(name=test['name'], run=rep + 1, status='PASS' if ok else 'FAIL', seconds=round(seconds, 1),
                         exit=int(rc.group(1)) if rc else None, runtime_exit=code, expect=test['expect'],
                         faults=faults[:5], compat=compat_events(text), host_open_at_end=host_fds(text), log=log.name)
            results['cli'].append(entry)
            print('CLI %-14s %s %6.1fs compat=%d faults=%d' % (test['name'], entry['status'], seconds, len(entry['compat']), len(faults)), flush=True)
            (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')

def run_gui():
    shutil.copy(here / 'appcompat_driver.c', host_dir)
    for fixture in (here / 'fixtures').iterdir(): shutil.copy(fixture, host_dir)
    code, text, _ = guest('cd %s && gcc -O2 -Wall appcompat_driver.c -o appcompat_driver -lX11 '
                          '/usr/lib/aarch64-linux-gnu/libXtst.so.6 && chmod 755 appcompat_driver && echo BUILT' % guest_dir,
                          300, out / 'driver-build.log')
    if 'BUILT' not in text: raise SystemExit('appcompat_driver build failed; see driver-build.log')
    tag = 'ac-%d' % int(time.time())
    # Results go to the session user's home: the exchange is root-owned in the guest.
    lines = ['#!/bin/sh', 'set -u', 'o="$HOME/ac-out/%s"; mkdir -p "$o"' % tag]
    selected = [t for t in (apps.STARTUP if a.suite == 'startup' else apps.GUI) if not only or t['name'] in only]
    for test in selected:
        actions = test.get('actions', '').strip('\n').replace('~/', '$HOME/')
        (host_dir / (test['name'] + '.actions.in')).write_text(actions + '\n')
        # "~/" anywhere in an argument means the session user's home.
        command = ' '.join('"%s"' % c.replace('"', '\\"').replace('~/', '$HOME/') if '~/' in c else shlex.quote(c) for c in test['command'])
        for rep in range(a.repeat):
            lines.append('echo "AC_BEGIN %s %d"' % (test['name'], rep + 1))
            if test.get('setup'): lines.append('( %s ) || echo "AC_SETUP_FAILED %s"' % (test['setup'], test['name']))
            # Expand only $HOME in the actions (typed text keeps its shell syntax).
            lines.append('sed -e "s|\\$HOME|$HOME|g" -e "s|\\$AC_OUT|$o|g" %s/%s.actions.in > "$o/%s.actions"' % (guest_dir, test['name'], test['name']))
            lines.append('%s/appcompat_driver %s %d "$o/%s.actions" "$o" -- %s' % (
                guest_dir, test['name'], test.get('timeout', 120), test['name'], command))
            lines.append('sleep 2')
    lines += ['cat /proc/ish/compat > "$o/compat.txt"', 'sleep 5', 'touch "$HOME/%s-done"' % tag]
    script = host_dir / ('session-%s.sh' % tag)
    script.write_text('\n'.join(lines) + '\n'); script.chmod(0o755)
    probe = a.probe or str(pathlib.Path(a.output).parent / 'passive-probe')
    env = dict(os.environ, LR_PASSIVE_MARKER='home/%s/%s-done' % (a.user, tag), LR_PASSIVE_SECONDS=str(300 + 240 * len(selected) * a.repeat))
    # The runtime default (memory dispatch fusion on since 884) applies unless
    # LR_MEMORY_PC_FUSION selects a setting for comparison.
    if 'LR_MEMORY_PC_FUSION' in os.environ:
        env['ISH_ARM64_MEMORY_PC_FUSION'] = os.environ['LR_MEMORY_PC_FUSION']
    session_out = out / 'gui-session'
    with (out / 'gui-driver.log').open('w') as log:
        subprocess.run(['python3', str(repo / 'tests/linuxrunner/gui/run_guest_session.py'), '--runtime', str(runtime),
                        '--rootfs', str(rootfs), '--exchange', str(exchange), '--probe', probe, '--mode', 'custom',
                        '--script', str(script), '--output', str(session_out), '--guest-user', a.user, '--geometry', '960x540',
                        '--glx', 'software', '--black-screen-diagnostics', '--timeout', str(400 + 240 * len(selected) * a.repeat)]
                       + (['--application-bundles', a.application_bundles] if a.application_bundles else [])
                       + ['--vnc-timing', a.vnc_timing],
                       cwd=repo, env=env, preexec_fn=limits, stdout=log, stderr=subprocess.STDOUT)
    launch = '\n'.join(f.read_text(errors='replace') for f in session_out.glob('*.log'))
    result_dir = rootfs / 'data' / 'home' / a.user / 'ac-out' / tag
    compat = result_dir / 'compat.txt'
    if result_dir.exists(): shutil.copytree(result_dir, out / 'gui-results', dirs_exist_ok=True)
    compat_text = compat.read_text(errors='replace') if compat.exists() else ''
    trends = [int(v) for v in re.findall(r'FD_TREND .*?host_open=(\d+)', (session_out / 'fd-trends.log').read_text(errors='replace'))] if (session_out / 'fd-trends.log').exists() else []
    actions_by_app, current = {}, None
    for line in launch.splitlines():
        m = re.match(r'AC_BEGIN (\S+) (\d+)', line)
        if m: current = (m.group(1), int(m.group(2))); actions_by_app[current] = []
        elif (line.startswith('ACTION') or line.startswith('REMAINING')) and current: actions_by_app[current].append(line)
    for line in launch.splitlines():
        if not line.startswith('APPCOMPAT '): continue
        try: r = json.loads(line[len('APPCOMPAT '):])
        except ValueError: continue
        test = next((t for t in selected if t['name'] == r.get('name')), {})
        runs = [x for x in results['gui'] if x['name'] == r.get('name')]
        key = (r.get('name'), len(runs) + 1)
        acts = actions_by_app.get(key, [])
        mine = [e for e in compat_events(compat_text) if e['comm'].lower().startswith(r.get('name', '')[:6].lower())]
        window = r.get('window_ms', -1) >= 0
        functional = window and r.get('actions_failed', 1) == 0 and r.get('alive_after_actions', 0)
        clean = r.get('close') in ('clean', 'xkillclient')
        status = 'PASS' if functional and clean and r.get('leftover_processes', 1) == 0 else 'PARTIAL' if window else 'FAIL'
        results['gui'].append(dict(r, run=len(runs) + 1, status=status, actions=acts, compat=mine))
        print('GUI %-14s %s window=%sms actions=%d/%d close=%s leftover=%s' % (
            r.get('name'), status, r.get('window_ms'), r.get('actions_ok', 0), r.get('actions_ok', 0) + r.get('actions_failed', 0),
            r.get('close'), r.get('leftover_processes')), flush=True)
    seen = {x['name'] for x in results['gui']}
    for test in selected:
        if test['name'] not in seen:
            results['gui'].append(dict(name=test['name'], status='FAIL', error='no APPCOMPAT result (driver did not finish)'))
            print('GUI %-14s FAIL no result' % test['name'], flush=True)
    results['gui_host_fds'] = dict(samples=len(trends), peak=max(trends) if trends else None, last=trends[-1] if trends else None)
    results['gui_compat_raw'] = compat_text.splitlines()[-200:]

if a.suite in ('cli', 'all'): run_cli()
if a.suite in ('gui', 'all', 'startup'): run_gui()
if a.suite == 'startup':
    import statistics
    summary = {}
    for name in dict.fromkeys(r['name'] for r in results['gui']):
        runs = sorted((r for r in results['gui'] if r['name'] == name), key=lambda r: r.get('run', 0))
        def stat(key, rs):
            v = [r.get(key, -1) for r in rs if r.get(key, -1) is not None and r.get(key, -1) >= 0]
            return dict(n=len(v), median=statistics.median(v) if v else None, min=min(v) if v else None, max=max(v) if v else None)
        summary[name] = dict(cold=dict(window=stat('window_ms', runs[:1]), ready=stat('ready_ms', runs[:1])),
                             warm=dict(window=stat('window_ms', runs[1:]), ready=stat('ready_ms', runs[1:])),
                             statuses=[r.get('status') for r in runs])
        print('STARTUP %-18s cold window=%s ready=%s | warm window=%s ready=%s' % (name,
            summary[name]['cold']['window']['median'], summary[name]['cold']['ready']['median'],
            summary[name]['warm']['window']['median'], summary[name]['warm']['ready']['median']), flush=True)
    results['startup_summary'] = summary
(out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
print('APPCOMPAT_RUN_DONE', out / 'results.json')
