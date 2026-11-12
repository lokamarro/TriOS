#!/usr/bin/env python3
"""Bounded, interleaved macOS guest comparison; never an iPhone speed claim."""
import argparse,hashlib,json,os,pathlib,platform,resource,statistics,subprocess,time
p=argparse.ArgumentParser();p.add_argument('--baseline',required=True);p.add_argument('--candidate',required=True);p.add_argument('--rootfs',required=True);p.add_argument('--output',required=True);p.add_argument('--runs',type=int,default=5);a=p.parse_args()
assert 2<=a.runs<=10
workloads={'startup':['/bin/true'],'python-loop':['/usr/bin/python3','-c','x=0\nfor i in range(150000): x=(x+i*i)&0xffffffff\nprint(x)'],'filesystem':['/bin/sh','-ec','p=$(mktemp -d); dd if=/dev/zero of="$p/a" bs=65536 count=16 2>/dev/null; cp "$p/a" "$p/b"; cmp "$p/a" "$p/b"; rm -r "$p"'],'gcc-compile-execute':['/bin/sh','-ec',"p=$(mktemp -d); printf '%s\\n' '#include <stdio.h>' 'int main(void){unsigned x=1;for(unsigned i=0;i<1000000;i++)x=x*1664525u+1013904223u;printf(\"%u\\n\",x);}' > \"$p/a.c\"; gcc -O2 \"$p/a.c\" -o \"$p/a\"; \"$p/a\"; rm -r \"$p\""]}
out={'environment':platform.platform(),'device':platform.machine(),'physical_iOS':'NOT TESTED','runs':a.runs,'scope':'wall and host user/system CPU seconds, includes guest bootstrap; no GUI/network speed claim','binaries':{},'workloads':{}}
for label,binary in [('baseline',a.baseline),('candidate',a.candidate)]:out['binaries'][label]={'path':str(pathlib.Path(binary).resolve()),'sha256':hashlib.sha256(pathlib.Path(binary).read_bytes()).hexdigest()}
for name,command in workloads.items():
 data={k:{'wall':[],'cpu':[],'stdout':[]} for k in out['binaries']}
 for i in range(a.runs):
  for label in (['baseline','candidate'] if i%2==0 else ['candidate','baseline']):
   env=dict(os.environ);env.pop('ISH_ARM64_BLOCK_STATS',None)
   before=resource.getrusage(resource.RUSAGE_CHILDREN);start=time.perf_counter()
   r=subprocess.run([out['binaries'][label]['path'],'-f',a.rootfs,*command],env=env,capture_output=True,text=True,timeout=90);elapsed=time.perf_counter()-start
   after=resource.getrusage(resource.RUSAGE_CHILDREN)
   if r.returncode:raise SystemExit(f'{name}/{label} failed {r.returncode}: {r.stderr[-2000:]}')
   data[label]['wall'].append(elapsed);data[label]['cpu'].append(after.ru_utime+after.ru_stime-before.ru_utime-before.ru_stime);data[label]['stdout'].append(r.stdout.strip())
 assert len(set(data['baseline']['stdout']+data['candidate']['stdout']))==1,(name,'output mismatch')
 for label,v in data.items():v['median_wall_seconds']=statistics.median(v['wall']);v['median_cpu_seconds']=statistics.median(v['cpu'])
 out['workloads'][name]=data
 path=pathlib.Path(a.output);path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(out,indent=2)+'\n')
 print(name,{k:round(v['median_wall_seconds'],4) for k,v in data.items()},flush=True)
