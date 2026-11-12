#!/usr/bin/env python3
import pathlib,json,hashlib,urllib.request,concurrent.futures,time
base=pathlib.Path(__file__).resolve().parents[2];m=json.loads((base/'app/LinuxRunner/DebianSupport.bundle/manifest.json').read_text());cache=base/'build/debian/packages';cache.mkdir(exist_ok=True)
def fetch(p):
    dest=cache/(p['Package']+'.deb')
    def valid():return dest.exists() and dest.stat().st_size==int(p['Size']) and hashlib.sha256(dest.read_bytes()).hexdigest()==p['SHA256']
    if not valid():
        for attempt in range(3):
            try:
                with urllib.request.urlopen(m['repository']+p['Filename'],timeout=60) as r: data=r.read(int(p['Size'])+1)
                if len(data)!=int(p['Size']) or hashlib.sha256(data).hexdigest()!=p['SHA256']: raise ValueError('integrity mismatch')
                temporary=dest.with_suffix('.partial');temporary.write_bytes(data);temporary.replace(dest);break
            except Exception:
                if attempt==2:raise
                time.sleep(attempt+1)
    return p['Package']
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
    for name in pool.map(fetch,m['packages']):print(name,flush=True)
print('PASS: verified',len(m['packages']),'official Debian packages')
