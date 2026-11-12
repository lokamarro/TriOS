#!/usr/bin/env python3
"""Real ARM64 guest contracts; disposable fixtures only. Not iPhone evidence."""
import argparse,fcntl,hashlib,json,os,pathlib,shutil,subprocess,threading
p=argparse.ArgumentParser()
p.add_argument('--runtime',required=True);p.add_argument('--rootfs',required=True)
p.add_argument('--exchange',required=True);p.add_argument('--output',required=True)
p.add_argument('--host-fds',action='store_true')
a=p.parse_args();root=pathlib.Path(__file__).resolve().parents[2]
out=pathlib.Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True)
exchange=pathlib.Path(a.exchange).resolve();exchange.mkdir(parents=True,exist_ok=True)
lock=(pathlib.Path(a.rootfs)/'.host-gui-test.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
runtime=pathlib.Path(a.runtime).resolve()
identity={'platform':'macOS ARM64 interpreter fixture, not physical iPhone','runtime':str(runtime),'runtime_sha256':hashlib.sha256(runtime.read_bytes()).hexdigest(),'source_revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'dirty_source':bool(subprocess.check_output(['git','status','--porcelain'],cwd=root,text=True).strip())}
env=dict(os.environ,ISH_BIND_MOUNTS='/home/mobile/Downloads='+str(exchange),ISH_ARM64_MEMORY_PC_FUSION='0')
shutil.copy(root/'tests/linuxrunner/fd_integrity_contract.c',exchange)
shutil.copy(root/'tests/linuxrunner/poll_deadline_contract.c',exchange)
shutil.copy(root/'tests/linuxrunner/fd_unshare_contract.c',exchange)
with (out/'guest.log').open('w') as log:
 result=subprocess.run([str(runtime),'-f',a.rootfs,'/bin/sh','-ec','cd /home/mobile/Downloads; gcc -O2 -Wall -Wextra -pthread fd_integrity_contract.c -o fd-contract; ./fd-contract; gcc -O2 -Wall -Wextra -pthread poll_deadline_contract.c -o poll-deadline; ./poll-deadline; gcc -O2 -Wall -Wextra -pthread fd_unshare_contract.c -o fd-unshare; ./fd-unshare'],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=120)
text=(out/'guest.log').read_text();identity['contract_exit']=result.returncode
print('\n'.join(line for line in text.splitlines() if 'FD_CONTRACT' in line or 'FD_UNSHARE_CONTRACT' in line or 'POLL_DEADLINE_' in line or 'FD_CHECK' in line and 'FAIL' in line))
if a.host_fds and result.returncode==0:
 guest=subprocess.Popen([str(runtime),'-f',a.rootfs,'/home/mobile/Downloads/fd-contract','host-probe'],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 samples={}
 timer=threading.Timer(30,guest.kill);timer.start()
 with (out/'host-fd-probe.log').open('w') as log:
  try:
   for line in guest.stdout:
    log.write(line)
    if line.startswith('HOST_FD_'):
     listing=subprocess.check_output(['/usr/sbin/lsof','-nP','-a','-p',str(guest.pid),'-Ff'],text=True,timeout=5)
     samples[line.strip()]=sum(x.startswith('f') and x[1:2].isdigit() for x in listing.splitlines())
   code=guest.wait(timeout=15)
  finally:
   timer.cancel()
   if guest.poll() is None:guest.kill();guest.wait()
 identity['host_fd_probe']={'exit':code,**samples,'iterations':2000}
 assert code==0 and samples.get('HOST_FD_BEFORE')==samples.get('HOST_FD_AFTER'),identity['host_fd_probe']
 print('HOST_FD_PROBE',identity['host_fd_probe'])
(out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
raise SystemExit(result.returncode)
