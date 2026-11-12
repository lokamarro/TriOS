#!/usr/bin/env python3
"""Host RLIMIT_NOFILE startup raise under the iOS default soft limit (2560).

Runs the runtime with an inherited soft limit of 2560 (hard unlimited, as the
phone reports a hard limit above its soft limit) and checks: the raise is
reported in /proc/ish/fds, the guest limit stays 1024/4096, and guest processes
together holding 3200 host fds complete with the raise but exhaust without it
(ISH_HOST_NOFILE=0). A hard limit of 2560 must leave the soft limit unchanged.
Mac fixture only; the phone's actual limits are recorded in its startup.log.
"""
import argparse, json, os, pathlib, re, resource, shutil, subprocess

p = argparse.ArgumentParser()
for key in ['runtime', 'rootfs', 'exchange', 'output']: p.add_argument('--' + key, required=True)
a = p.parse_args()
root = pathlib.Path(__file__).resolve().parents[2]
out = pathlib.Path(a.output).resolve(); out.mkdir(parents=True, exist_ok=True)
exchange = pathlib.Path(a.exchange).resolve(); exchange.mkdir(parents=True, exist_ok=True)
shutil.copy(root / 'tests/linuxrunner/host_nofile_pressure_contract.c', exchange)

def run(name, soft, hard, env_extra, script):
    def limits():
        resource.setrlimit(resource.RLIMIT_NOFILE, (soft, hard))
    env = dict(os.environ, ISH_BIND_MOUNTS='/home/mobile/Downloads=' + str(exchange), **env_extra)
    r = subprocess.run([a.runtime, '-f', a.rootfs, '/bin/sh', '-ec', script], env=env, preexec_fn=limits,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300)
    (out / (name + '.log')).write_text(r.stdout)
    return r.returncode, r.stdout

failures = []
def check(cond, message):
    if not cond: failures.append(message); print('FAIL', message)

build = 'cd /home/mobile/Downloads; gcc -O2 -Wall -Wextra host_nofile_pressure_contract.c -o host_nofile_pressure_contract'
probe = 'cd /home/mobile/Downloads; grep -o "host_nofile_[a-z]*=[^ ]*" /proc/ish/fds | sort -u; ./host_nofile_pressure_contract'
INF = resource.RLIM_INFINITY
code, text = run('build', 2560, INF, {}, build)
check(code == 0, 'contract build exit %d' % code)
results = {}
for name, soft, hard, env, expect in [('raised', 2560, INF, {}, 'COMPLETE'),
                                      ('disabled', 2560, INF, {'ISH_HOST_NOFILE': '0'}, 'EXHAUSTED'),
                                      ('hard-capped', 2560, 2560, {}, 'EXHAUSTED')]:
    code, text = run(name, soft, hard, env, probe)
    raise_state = dict(re.findall(r'host_nofile_(\w+)=(\S+)', text))
    outcome = re.search(r'host_nofile_pressure total_pipes=(\d+) failed_children=(\d+) (\w+)', text)
    results[name] = {'exit': code, 'state': raise_state, 'pressure': outcome.group(0) if outcome else None}
    check(code == 0, '%s exit %d' % (name, code))
    check('guest_nofile soft=1024 hard=4096' in text, '%s guest limit changed' % name)
    check(outcome is not None and outcome.group(3) == expect, '%s expected %s, got %s' % (name, expect, outcome and outcome.group(0)))
    want_raise = {'raised': 'raised', 'disabled': 'disabled', 'hard-capped': 'unchanged'}[name]
    check(raise_state.get('raise') == want_raise, '%s raise state %s' % (name, raise_state.get('raise')))
    if name == 'raised': check(raise_state.get('soft') == '8192', 'raised soft %s' % raise_state.get('soft'))
    else: check(raise_state.get('soft') == '2560', '%s soft %s' % (name, raise_state.get('soft')))
(out / 'summary.json').write_text(json.dumps({'environment': 'Mac ARM64 interpreter, not physical iPhone',
                                              'results': results, 'failures': failures}, indent=2) + '\n')
print('host_nofile', 'FAIL' if failures else 'PASS', json.dumps(results))
raise SystemExit(1 if failures else 0)
