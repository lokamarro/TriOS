#!/usr/bin/env python3
"""Constrained host resource reproduction; never changes guest/host limits globally."""
import argparse,fcntl,hashlib,json,os,pathlib,resource,shutil,subprocess,threading
p=argparse.ArgumentParser()
for n in ('runtime','rootfs','exchange','output'):p.add_argument('--'+n,required=True)
p.add_argument('--baseline')
a=p.parse_args();root=pathlib.Path(__file__).resolve().parents[2];out=pathlib.Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True)
exchange=pathlib.Path(a.exchange).resolve();exchange.mkdir(parents=True,exist_ok=True)
lock=(pathlib.Path(a.rootfs)/'.host-gui-test.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
env=dict(os.environ,ISH_BIND_MOUNTS='/home/mobile/Downloads='+str(exchange),ISH_ARM64_MEMORY_PC_FUSION='0')
shutil.copy(root/'tests/linuxrunner/futex_hostfd_contract.c',exchange)
with (out/'compile.log').open('w') as log:
 subprocess.run([a.runtime,'-f',a.rootfs,'/bin/sh','-ec','cd /home/mobile/Downloads; gcc -O2 -pthread futex_hostfd_contract.c -o futex-hostfd'],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=30,check=True)
results=[]
for label,runtime in [('baseline',a.baseline),('candidate',a.runtime)]:
 if not runtime:continue
 for limit in (96,256):
  for mode in ('threads','epoll'):
   def lower():resource.setrlimit(resource.RLIMIT_NOFILE,(limit,resource.getrlimit(resource.RLIMIT_NOFILE)[1]))
   proc=subprocess.Popen([runtime,'-f',a.rootfs,'/home/mobile/Downloads/futex-hostfd',mode],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,preexec_fn=lower)
   timer=threading.Timer(25,proc.kill);timer.start();samples={}
   try:
    with (out/f'{label}-{limit}-{mode}.log').open('w') as log:
     for line in proc.stdout:
      log.write(line)
      if line.startswith('FUTEX_HOSTFD_') and line.strip().endswith(('BEFORE','DURING','AFTER')):
       listing=subprocess.run(['/usr/sbin/lsof','-nP','-a','-p',str(proc.pid),'-Ff'],stdout=subprocess.PIPE,text=True,timeout=5)
       samples[line.strip()]=sum(s.startswith('f') and s[1:2].isdigit() for s in listing.stdout.splitlines())
    code=proc.wait(timeout=5)
   finally:
    timer.cancel()
    if proc.poll() is None:proc.kill();proc.wait()
   row=dict(label=label,host_limit=limit,mode=mode,exit=code,host_fd_counts=samples,runtime_sha256=hashlib.sha256(pathlib.Path(runtime).read_bytes()).hexdigest());results.append(row)
   (out/'results.json').write_text(json.dumps(dict(platform='Mac ARM64 interpreter, not physical iPhone',results=results),indent=2)+'\n');print(row,flush=True)
   if label=='candidate':assert code==0,row
print('FUTEX_HOSTFD_SUITE_PASS')
