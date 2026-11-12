#!/usr/bin/env python3
"""Native double-close observation and bounded guest socket failure contracts."""
import argparse,fcntl,json,os,pathlib,shutil,subprocess
p=argparse.ArgumentParser()
for n in ('runtime','rootfs','exchange','output'):p.add_argument('--'+n,required=True)
p.add_argument('--expect-failure',action='store_true')
a=p.parse_args();root=pathlib.Path(__file__).resolve().parents[2];out=pathlib.Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True);exchange=pathlib.Path(a.exchange).resolve()
lock=(pathlib.Path(a.rootfs)/'.host-gui-test.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
shutil.copy(root/'tests/linuxrunner/socket_fd_failure_contract.c',exchange)
env=dict(os.environ,ISH_BIND_MOUNTS='/home/mobile/Downloads='+str(exchange),ISH_ARM64_MEMORY_PC_FUSION='0')
with (out/'compile.log').open('w') as log:
 subprocess.run([a.runtime,'-f',a.rootfs,'/bin/sh','-ec','cd /home/mobile/Downloads; gcc -O2 socket_fd_failure_contract.c -o socket-failure'],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=30,check=True)
 audit=out/'close-audit.dylib';subprocess.run(['/usr/bin/clang','-dynamiclib',str(root/'tests/linuxrunner/socket_close_audit.c'),'-o',str(audit)],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=30)
rows=[]
for mode in ('socket','zero-slot','one-slot','efault'):
 with (out/(mode+'.log')).open('w') as log:
  try:rc=subprocess.run([a.runtime,'-f',a.rootfs,'/home/mobile/Downloads/socket-failure',mode],env=dict(env,DYLD_INSERT_LIBRARIES=str(audit)),stdout=log,stderr=subprocess.STDOUT,timeout=6).returncode
  except subprocess.TimeoutExpired:rc=124
 text=(out/(mode+'.log')).read_text();phase=text.split('SOCKET_FD_PHASE_BEGIN\n',1)[-1].split('SOCKET_FD_PHASE_END',1)[0];bad=phase.count('HOST_CLOSE_EBADF')
 row=dict(mode=mode,exit=rc,host_double_close=bad);rows.append(row);print(row,flush=True)
 (out/'results.json').write_text(json.dumps(dict(platform='Mac ARM64 interpreter, not iPhone',runtime=str(pathlib.Path(a.runtime).resolve()),results=rows),indent=2)+'\n')
 if not a.expect_failure:assert rc==0 and bad==0,row
print('SOCKET_FAILURE_REPRODUCED' if a.expect_failure else 'SOCKET_FAILURE_SUITE_PASS')
