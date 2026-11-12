#!/usr/bin/env python3
"""Exercise actual fatal-versus-handled Linux dispositions and additive hooks."""
import argparse,fcntl,json,os,pathlib,re,shutil,subprocess
p=argparse.ArgumentParser()
for key in ('runtime','rootfs','exchange','output'):p.add_argument('--'+key,required=True)
a=p.parse_args();root=pathlib.Path(__file__).resolve().parents[2];out=pathlib.Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True);exchange=pathlib.Path(a.exchange).resolve();env=dict(os.environ,ISH_BIND_MOUNTS='/home/mobile/Downloads='+str(exchange),ISH_ARM64_MEMORY_PC_FUSION='0')
with (pathlib.Path(a.rootfs)/'.host-gui-test.lock').open('a') as lock:
 fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB);shutil.copy(root/'tests/linuxrunner/guest_crash_contract.c',exchange)
 with (out/'compile.log').open('w') as log:
  subprocess.run([a.runtime,'-f',a.rootfs,'/bin/sh','-ec','cd /home/mobile/Downloads; gcc -O2 -pthread -Wall -Wextra guest_crash_contract.c -o guest-crash'],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=30,check=True)
  subprocess.run(['clang','-dynamiclib',str(root/'tests/linuxrunner/guest_crash_hook_audit.c'),'-o',str(out/'audit.dylib')],stdout=log,stderr=subprocess.STDOUT,timeout=30,check=True)
 rows=[]
 for mode,sig,code in [('handled',None,None),('trap',5,-1),('reserved',4,-1),('abort',6,-1),('segv',11,-1),('exit',0,7),('worker191',0,191),('output',0,19),('crashpad',5,-1)]:
  with (out/(mode+'.log')).open('w') as log:
   result=subprocess.run([a.runtime,'-f',a.rootfs,'/home/mobile/Downloads/guest-crash',mode],env=dict(env,DYLD_INSERT_LIBRARIES=str(out/'audit.dylib'),LR_CRASH_AUDIT_ALTSTACK='1'),stdout=log,stderr=subprocess.STDOUT,timeout=15)
  text=(out/(mode+'.log')).read_text();terminations=re.findall(r'CRASH_AUDIT_TERMINATED tid=\d+ signal=(\d+) exit=(-?\d+)',text)
  expected=[] if sig is None else [(str(sig),str(code))]
  assert terminations==expected,(mode,result.returncode,terminations,expected)
  assert 'CRASH_AUDIT_HOOK_MISSING' not in text
  assert 'CRASH_AUDIT_HOST_ALTSTACK_PRESERVED' in text
  assert 'CRASH_AUDIT_HOST_ALTSTACK_REPLACED' not in text
  if sig is None:assert result.returncode==0
  else:assert 'FD_SNAPSHOT' in text and 'FAULT_REG' in text and 'FAULT_CALL' in text and 'FAULT_END' in text
  if mode=='worker191':assert result.returncode==191 and 'name=exit_group status_raw=191 status_linux=191 origin=guest_syscall' in text and 'FATAL test worker exit 191' in text
  if mode=='output':assert 'FATAL partial message' in text and 'WARNING stdout tail' in text
  if mode=='reserved':assert 'instruction=0xee07800' in text or 'instruction=0x0ee07800' in text
  rows.append(dict(mode=mode,exit=result.returncode,termination_hooks=terminations));print(rows[-1],flush=True)
 (out/'results.json').write_text(json.dumps(dict(platform='Mac ARM64 interpreter; UIKit/device NOT tested by this contract',results=rows),indent=2)+'\n')
