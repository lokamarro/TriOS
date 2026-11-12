#!/usr/bin/env python3
"""Test fresh imported execute bits, readonly enforcement and guest metadata."""
import argparse,fcntl,hashlib,json,os,pathlib,subprocess,uuid
p=argparse.ArgumentParser()
for key in ['runtime','rootfs','output']:p.add_argument('--'+key,required=True)
a=p.parse_args();out=pathlib.Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True)
lock=(pathlib.Path(a.rootfs)/'.host-gui-test.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
bundle=out/'public-imports';bundle.mkdir(mode=0o755)
for name,mode in [('executable',0o755),('nonexecutable',0o644)]:
 f=bundle/name;f.write_text('#!/bin/sh\nprintf "IMPORTED_EXEC_PASS\\n"\n');f.chmod(mode)
guest='/opt/lr-bind-exec-'+uuid.uuid4().hex[:12]
env=dict(os.environ,ISH_BIND_MOUNTS=guest+'='+str(bundle)+':ro',ISH_ARM64_MEMORY_PC_FUSION='0')
command='''set -eu
test "$(stat -c %a "$1/executable")" = 755
test "$(stat -c %a "$1/nonexecutable")" = 644
"$1/executable"
if "$1/nonexecutable"; then echo UNEXPECTED_NONEXEC_SUCCESS; exit 1; fi
if chmod 777 "$1/executable"; then echo UNEXPECTED_RO_CHMOD; exit 1; fi
if printf changed > "$1/executable"; then echo UNEXPECTED_RO_WRITE; exit 1; fi
echo IMPORTED_READONLY_PASS
'''
runtime=pathlib.Path(a.runtime).resolve()
with (out/'guest.log').open('w') as log:
 r=subprocess.run([str(runtime),'-f',a.rootfs,'/bin/sh','-ec',command,'bind-exec-contract',guest],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=30)
assert (bundle/'executable').read_text().endswith('"IMPORTED_EXEC_PASS\\n"\n')
report={'environment':'Mac ARM64 interpreter, not physical iPhone','runtime_sha256':hashlib.file_digest(runtime.open('rb'),'sha256').hexdigest(),'exit':r.returncode,'fresh_guest_prefix':guest,'readonly':True}
(out/'identity.json').write_text(json.dumps(report,indent=2)+'\n')
print('BIND_EXEC_CONTRACT',json.dumps(report));raise SystemExit(r.returncode)
