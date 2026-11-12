#!/usr/bin/env python3
"""Prove poll/select readiness under an unchanged, full native FD budget."""
import argparse,fcntl,hashlib,json,os,pathlib,resource,shutil,subprocess
p=argparse.ArgumentParser()
for key in ('runtime','rootfs','exchange','output'):p.add_argument('--'+key,required=True)
p.add_argument('--expect-failure',action='store_true');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[2];runtime=pathlib.Path(a.runtime).resolve();out=pathlib.Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True);exchange=pathlib.Path(a.exchange).resolve();exchange.mkdir(parents=True,exist_ok=True)
with (pathlib.Path(a.rootfs)/'.host-gui-test.lock').open('a') as lock:
 fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
 shutil.copy(root/'tests/linuxrunner/poll_full_host_contract.c',exchange)
 env=dict(os.environ,ISH_BIND_MOUNTS='/home/mobile/Downloads='+str(exchange),ISH_ARM64_MEMORY_PC_FUSION='0')
 with (out/'compile.log').open('w') as log:subprocess.run([str(runtime),'-f',a.rootfs,'/bin/sh','-ec','cd /home/mobile/Downloads; gcc -O2 -Wall -Wextra poll_full_host_contract.c -o poll-full-host'],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=30,check=True)
 def cap():resource.setrlimit(resource.RLIMIT_NOFILE,(96,resource.getrlimit(resource.RLIMIT_NOFILE)[1]))
 with (out/'pressure.log').open('w') as log:
  r=subprocess.run([str(runtime),'-f',a.rootfs,'/home/mobile/Downloads/poll-full-host'],env=env,stdout=log,stderr=subprocess.STDOUT,preexec_fn=cap,timeout=20)
 report=dict(platform='Mac ARM64 interpreter; not iPhone',runtime_sha256=hashlib.file_digest(runtime.open('rb'),'sha256').hexdigest(),host_soft_limit=96,exit=r.returncode,expected_baseline_failure=a.expect_failure)
 (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
 assert r.returncode==(1 if a.expect_failure else 0),report
