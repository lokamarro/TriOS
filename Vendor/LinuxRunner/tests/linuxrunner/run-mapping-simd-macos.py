#!/usr/bin/env python3
"""Focused 879 contracts; native ARM64 oracle and test-only mmap failure injection.

Use a disposable rootfs and the shared heavy-test lease. The interposer is built
inside the evidence directory and is never linked into the application.
"""
import argparse
import fcntl
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess

p = argparse.ArgumentParser()
for arg in ('runtime', 'rootfs', 'exchange', 'output'):
    p.add_argument('--' + arg, required=True)
a = p.parse_args()
repo = pathlib.Path(__file__).resolve().parents[2]
runtime, rootfs, exchange, out = [pathlib.Path(v).resolve() for v in
                                (a.runtime, a.rootfs, a.exchange, a.output)]
out.mkdir(parents=True, exist_ok=True)
exchange.mkdir(parents=True, exist_ok=True)
report = {'environment': 'Mac ARM64 interpreter, not physical iPhone',
          'runtime_sha256': hashlib.sha256(runtime.read_bytes()).hexdigest(), 'stages': []}

def run(name, args, env=None):
    with (out / (name + '.log')).open('w') as log:
        r = subprocess.run(args, cwd=repo, env=env, stdout=log,
                           stderr=subprocess.STDOUT, timeout=180)
    report['stages'].append({'name': name, 'exit': r.returncode})
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    r.check_returncode()
    return (out / (name + '.log')).read_text()

with (rootfs / '.host-gui-test.lock').open('a') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    env = dict(os.environ, ISH_BIND_MOUNTS='/home/mobile/Downloads=' + str(exchange),
               ISH_ARM64_MEMORY_PC_FUSION='0')
    for name in ('mmap_native_error_contract', 'high_mmap_reuse_contract',
                 'shared_mapping_lifecycle_contract'):
        shutil.copy(repo / 'tests/linuxrunner' / (name + '.c'), exchange)
    shutil.copy(repo / 'tests/regress/regress_mixed_saturating_accum.c', exchange)
    run('compile-guest', [str(runtime), '-f', str(rootfs), '/bin/sh', '-ec',
        'cd /home/mobile/Downloads; for n in mmap_native_error_contract '
        'high_mmap_reuse_contract shared_mapping_lifecycle_contract '
        'regress_mixed_saturating_accum; do gcc -O2 -Wall -Wextra "$n.c" -o "$n"; done'], env)
    for name in ('high_mmap_reuse_contract', 'shared_mapping_lifecycle_contract'):
        run(name, [str(runtime), '-f', str(rootfs), '/home/mobile/Downloads/' + name], env)
    native = out / 'mixed-sat-native'
    run('compile-native-oracle', ['clang', '-O2', str(repo / 'tests/regress/regress_mixed_saturating_accum.c'), '-o', str(native)])
    oracle = run('native-oracle', [str(native)])
    guest = run('guest-oracle', [str(runtime), '-f', str(rootfs),
                               '/home/mobile/Downloads/regress_mixed_saturating_accum'], env)
    pattern = r'MIXED_SAT_ACCUM_PASS cases=\d+ digest=[0-9a-f]+'
    assert re.search(pattern, oracle).group() == re.search(pattern, guest).group()
    dylib = out / 'mmap-native-error.dylib'
    run('compile-interposer', ['clang', '-dynamiclib', '-O2',
        str(repo / 'tests/linuxrunner/mmap_native_error_inject.c'), '-o', str(dylib)])
    injected = run('native-errno', [str(runtime), '-f', str(rootfs),
                   '/home/mobile/Downloads/mmap_native_error_contract'],
                   dict(env, DYLD_INSERT_LIBRARIES=str(dylib)))
    assert 'native_errno=13 linux_result=-13' in injected
    assert 'stage=file_backend guest_fd=3' in injected
    report['oracle_match'] = True
    report['status'] = 'PASS'
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print('MAPPING_SIMD_PASS; device and browser acceptance are separate')
