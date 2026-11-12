#!/usr/bin/env python3
"""Verify checksums as non-root with read-only managed support resources."""
import argparse,fcntl,hashlib,json,os,pathlib,subprocess,uuid
p=argparse.ArgumentParser()
for n in ('runtime','rootfs','output'):p.add_argument('--'+n,required=True)
a=p.parse_args();root=pathlib.Path(__file__).resolve().parents[2];out=pathlib.Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True)
lock=(pathlib.Path(a.rootfs)/'.host-gui-test.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
results=[]
for label in ('old','fixed','corrupt'):
 fixture=out/label;fixture.mkdir(mode=0o755)
 helper=(subprocess.check_output(['git','show','checkpoint/875-unsigned:app/LinuxRunner/GUISupport.bundle/support-common.sh'],cwd=root) if label=='old' else (root/'app/LinuxRunner/GUISupport.bundle/support-common.sh').read_bytes())
 (fixture/'support-common.sh').write_bytes(helper);(fixture/'support-version').write_text('1\n');(fixture/'resource.txt').write_text('verified content\n')
 (fixture/'check.sh').write_text('#!/bin/sh\nset -eu\n[ "$(id -u)" != 0 ]\n. "$(dirname "$0")/support-common.sh"\nverify_resources\n')
 (fixture/'check.sh').chmod(0o755);(fixture/'support-common.sh').chmod(0o755)
 (fixture/'support.sha256').write_text(''.join(hashlib.sha256((fixture/n).read_bytes()).hexdigest()+'  '+n+'\n' for n in ('support-common.sh','support-version','resource.txt','check.sh')))
 if label=='corrupt':(fixture/'resource.txt').write_text('corrupted content\n')
 guest='/opt/lr-support-'+uuid.uuid4().hex[:12]
 env=dict(os.environ,ISH_BIND_MOUNTS=guest+'='+str(fixture)+':ro',ISH_ARM64_MEMORY_PC_FUSION='0')
 with (out/(label+'.log')).open('w') as log:
  rc=subprocess.run([a.runtime,'-f',a.rootfs,'/bin/su','-s','/bin/sh','nobody','-c',guest+'/check.sh'],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=30).returncode
 results.append(dict(case=label,exit=rc));print(label,rc,flush=True)
 assert (rc==0)==(label=='fixed'),results
 assert not (fixture/'resource-check.log').exists()
(out/'results.json').write_text(json.dumps(dict(platform='Mac ARM64 interpreter, not iPhone',results=results),indent=2)+'\n')
print('SUPPORT_VERIFICATION_PASS readonly nonroot bad_checksum_rejected')
