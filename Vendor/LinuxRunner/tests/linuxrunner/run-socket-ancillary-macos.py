#!/usr/bin/env python3
"""Bounded original ARM64 guest SCM contracts; Mac fixture only."""
import argparse,fcntl,hashlib,json,os,pathlib,shutil,subprocess,threading
p=argparse.ArgumentParser()
for flag in ['runtime','rootfs','exchange','output']:p.add_argument('--'+flag,required=True)
p.add_argument('--host-fds',action='store_true');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[2];out=pathlib.Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True)
exchange=pathlib.Path(a.exchange).resolve();exchange.mkdir(parents=True,exist_ok=True)
lock=(pathlib.Path(a.rootfs)/'.host-gui-test.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
runtime=pathlib.Path(a.runtime).resolve();env=dict(os.environ,ISH_BIND_MOUNTS='/home/mobile/Downloads='+str(exchange),ISH_ARM64_MEMORY_PC_FUSION='0')
identity={'environment':'Mac ARM64 interpreter; not physical iPhone','runtime':str(runtime),'runtime_sha256':hashlib.sha256(runtime.read_bytes()).hexdigest(),'driver_revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'driver_dirty':bool(subprocess.check_output(['git','status','--porcelain'],cwd=root,text=True).strip())}
shutil.copy(root/'tests/linuxrunner/socket_ancillary_contract.c',exchange)
with (out/'guest.log').open('w') as log:
 result=subprocess.run([str(runtime),'-f',a.rootfs,'/bin/sh','-ec','cd /home/mobile/Downloads; gcc -O2 -Wall -Wextra -pthread socket_ancillary_contract.c -o scm-contract; ./scm-contract'],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=90)
identity['exit']=result.returncode
text=(out/'guest.log').read_text(errors='replace')
print('\n'.join(x for x in text.splitlines() if x.startswith(('SCM_CONTRACT','SCM_CASE','SCM_FAULT_RETENTION')) or x.startswith('SCM_CHECK') and 'FAIL' in x))
if a.host_fds and result.returncode==0:
 proc=subprocess.Popen([str(runtime),'-f',a.rootfs,'/home/mobile/Downloads/scm-contract','--host-fds'],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 samples={};timer=threading.Timer(60,proc.kill);timer.start()
 try:
  with (out/'host-fds.log').open('w') as log:
   for line in proc.stdout:
    log.write(line)
    if line.startswith('HOST_FD_'):
     data=subprocess.check_output(['/usr/sbin/lsof','-nP','-a','-p',str(proc.pid),'-Ff'],text=True,timeout=5)
     samples[line.strip()]=sum(x.startswith('f') and x[1:2].isdigit() for x in data.splitlines())
  code=proc.wait(timeout=5)
 finally:
  timer.cancel()
  if proc.poll() is None:proc.kill();proc.wait()
 identity['host_fds']={'exit':code,'iterations':256,**samples}
 if code!=0 or samples.get('HOST_FD_BEFORE')!=samples.get('HOST_FD_AFTER'):identity['exit']=1
 print('SCM_HOST_FDS',identity['host_fds'])
(out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
raise SystemExit(identity['exit'])
