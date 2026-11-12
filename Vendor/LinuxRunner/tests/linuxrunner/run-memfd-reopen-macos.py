#!/usr/bin/env python3
"""Actual guest reopen/permission/lifetime tests and constrained native FD capacity."""
import argparse,fcntl,hashlib,json,os,pathlib,re,resource,shutil,subprocess
p=argparse.ArgumentParser()
for name in ('runtime','rootfs','exchange','output'):p.add_argument('--'+name,required=True)
p.add_argument('--baseline-runtime')
a=p.parse_args();repo=pathlib.Path(__file__).resolve().parents[2]
runtime=pathlib.Path(a.runtime).resolve();rootfs=pathlib.Path(a.rootfs).resolve();exchange=pathlib.Path(a.exchange).resolve();out=pathlib.Path(a.output).resolve()
if out.exists() and any(out.iterdir()):raise SystemExit('STALE_TRACE_REJECTED: use a new output directory')
out.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,ISH_BIND_MOUNTS='/home/mobile/Downloads='+str(exchange),ISH_ARM64_MEMORY_PC_FUSION='0')
report={'environment':'Mac ARM64 interpreter, not iPhone','runtime_sha256':hashlib.sha256(runtime.read_bytes()).hexdigest(),'driver_revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),'driver_dirty':bool(subprocess.check_output(['git','status','--porcelain'],cwd=repo,text=True).strip()),'status':'RUNNING','cases':[]}
(out/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
def run(name,args,expected=0,cap=False):
 def limit():resource.setrlimit(resource.RLIMIT_NOFILE,(64,resource.getrlimit(resource.RLIMIT_NOFILE)[1]))
 with (out/(name+'.log')).open('w') as log:
  try:code=subprocess.run(args,env=env,stdout=log,stderr=subprocess.STDOUT,cwd=repo,preexec_fn=limit if cap else None,timeout=120).returncode
  except subprocess.TimeoutExpired:code=124
 report['cases'].append({'name':name,'exit':code,'expected':expected})
 if code!=expected:report['status']='FAIL'
 (out/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
 assert code==expected,(name,code,expected)
 return (out/(name+'.log')).read_text()
with (rootfs/'.host-gui-test.lock').open('a') as lock:
 fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
 for name in ('memfd_proc_reopen_contract','memfd_pressure_contract','memfd_offload_contract','memfd_size_seals_contract','shared_mapping_lifecycle_contract','shared_partial_page_contract'):shutil.copy(repo/'tests/linuxrunner'/(name+'.c'),exchange)
 cmd='cd /home/mobile/Downloads; for n in memfd_proc_reopen_contract memfd_pressure_contract memfd_offload_contract memfd_size_seals_contract shared_mapping_lifecycle_contract shared_partial_page_contract; do gcc -O2 -Wall -Wextra "$n.c" -o "$n"; done'
 run('compile',[str(runtime),'-f',str(rootfs),'/bin/sh','-ec',cmd])
 for name in ('memfd_proc_reopen_contract','memfd_size_seals_contract','shared_mapping_lifecycle_contract','shared_partial_page_contract'):
  run(name,[str(runtime),'-f',str(rootfs),'/home/mobile/Downloads/'+name])
 candidate=run('native-pressure',[str(runtime),'-f',str(rootfs),'/home/mobile/Downloads/memfd_pressure_contract'],cap=True)
 capacity=int(re.search(r'MEMFD_PRESSURE_PASS allocated=(\d+)',candidate).group(1));report['native_fd_capacity']=capacity
 if a.baseline_runtime:
  baseline=pathlib.Path(a.baseline_runtime).resolve()
  text=run('baseline-native-pressure',[str(baseline),'-f',str(rootfs),'/home/mobile/Downloads/memfd_pressure_contract'],cap=True)
  before=int(re.search(r'MEMFD_PRESSURE_PASS allocated=(\d+)',text).group(1));report['baseline_native_fd_capacity']=before
  report['baseline_sha256']=hashlib.sha256(baseline.read_bytes()).hexdigest();assert capacity>=before
 for mode,offload,expected in [('stdin','cat=/usr/bin/false',0),('stdout','echo=/usr/bin/true',1),('ordinary','cat=/usr/bin/false',1)]:
  text=run('offload-'+mode,[str(runtime),'-f',str(rootfs),'-n',offload,'/home/mobile/Downloads/memfd_offload_contract',mode],expected)
  if mode=='stdin':assert 'NATIVE_MEMFD_STDIN_PASS' in text and 'fallback to emulated exec' in text
  if mode=='stdout':assert 'Bad file descriptor' in text and 'fallback to emulated exec' in text
  if mode=='ordinary':assert 'native_offload: fallback' not in text
 report['status']='PASS';(out/'summary.json').write_text(json.dumps(report,indent=2)+'\n');print('MEMFD_REOPEN_SUITE_PASS',report['native_fd_capacity'],flush=True)
