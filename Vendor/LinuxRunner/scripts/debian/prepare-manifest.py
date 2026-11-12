#!/usr/bin/env python3
"""Build an immutable install manifest from authenticated Debian metadata.
Requires gpg/gpgv. No package or maintainer script executes on the host.
"""
import pathlib,subprocess,hashlib,lzma,re,json,datetime,os
base=pathlib.Path(__file__).resolve().parents[2]
audit=base/'build/debian/audit'; out=base/'app/LinuxRunner/DebianSupport.bundle'; out.mkdir(exist_ok=True)
trusted={'archive-key-12.asc':'B8B80B5B623EAB6AD8775C45B7C5D7D6350947F8','archive-key-13.asc':'04B54C3CDCA79751B16BC6B5225629DF75B188BD','release-13.asc':'41587F7DB8C774BCCF131416762F67A0B2C39DE4'}
keyring=audit/'trusted.gpg'; keyring.unlink(missing_ok=True)
for name,fingerprint in trusted.items():
    details=subprocess.check_output(['gpg','--batch','--with-colons','--show-keys',str(audit/name)],text=True)
    primary=next(line.split(':')[9] for line in details.splitlines() if line.startswith('fpr:'))
    if primary!=fingerprint: raise RuntimeError('Debian signing fingerprint mismatch')
    with keyring.open('ab') as f: f.write(subprocess.check_output(['gpg','--batch','--yes','--output','-','--dearmor',str(audit/name)]))
verify=subprocess.run(['gpgv','--status-fd','1','--keyring',str(keyring),str(audit/'InRelease')],capture_output=True,text=True)
(audit/'signature-verification.txt').write_text(verify.stdout+verify.stderr)
if '[GNUPG:] BADSIG' in verify.stdout or not any(line.startswith('[GNUPG:] VALIDSIG ') and any(f in line for f in trusted.values()) for line in verify.stdout.splitlines()): raise RuntimeError('Release signature invalid')
release=(audit/'InRelease').read_text(); index=audit/'Packages.xz'; indexhash=hashlib.sha256(index.read_bytes()).hexdigest()
expected=re.search(r'^ ([0-9a-f]{64})\s+(\d+)\s+main/binary-arm64/Packages.xz$',release,re.M)
if not expected or indexhash!=expected[1] or index.stat().st_size!=int(expected[2]): raise RuntimeError('Signed index integrity mismatch')
if not re.search(r'^Codename: trixie$',release,re.M): raise RuntimeError('Wrong Debian release')
packages={}; providers={}
for block in lzma.decompress(index.read_bytes()).decode().split('\n\n'):
    d={}; field=None
    for line in block.splitlines():
        if line.startswith(' ') and field: d[field]+='\n'+line
        elif ': ' in line: field,value=line.split(': ',1);d[field]=value
    if 'Package' not in d:continue
    packages[d['Package']]=d
    for provider in d.get('Provides','').split(','):
        name=provider.strip().split(' ')[0]
        if name:providers.setdefault(name,[]).append(d['Package'])
selected=set();pending=[n for n,d in packages.items() if d.get('Essential')=='yes']+['apt','bash','gpgv','debian-archive-keyring','ca-certificates','curl','wget','netbase']
while pending:
    name=pending.pop()
    if name in selected:continue
    d=packages[name]; selected.add(name)
    for dependency in (d.get('Pre-Depends','')+','+d.get('Depends','')).split(','):
        if not dependency.strip():continue
        alternatives=[re.split(r'[\s:(\[]',a.strip())[0] for a in dependency.split('|')]
        chosen=next((a for a in alternatives if a in selected),None) or next((a for a in alternatives if a in packages),None)
        if not chosen:
            chosen=next((p for a in alternatives for p in providers.get(a,[]) if p in selected),None) or next((p for a in alternatives for p in providers.get(a,[])),None)
        if not chosen:raise RuntimeError('Unresolved dependency '+dependency)
        pending.append(chosen)
rows=[]
for name in sorted(selected):
    d=packages[name]
    if d['Architecture'] not in ('arm64','all'):raise RuntimeError('Wrong architecture')
    if not d['Filename'].startswith('pool/') or '..' in d['Filename'].split('/'):raise RuntimeError('Unsafe package URL')
    rows.append({k:d[k] for k in ['Package','Version','Architecture','Filename','Size','SHA256','Installed-Size']})
manifest={'schema':1,'id':'debian-trixie-arm64','name':'Debian GNU/Linux','version':re.search(r'^Version: (.+)$',release,re.M)[1],'codename':'trixie','architecture':'arm64','packageManager':'apt','defaultShell':'/bin/bash','minimumRuntimeVersion':'3.0.0','repository':'https://deb.debian.org/debian/','releaseSHA256':hashlib.sha256((audit/'InRelease').read_bytes()).hexdigest(),'indexSHA256':indexhash,'signingFingerprints':list(trusted.values()),'packages':rows,'downloadBytes':sum(int(r['Size']) for r in rows),'installedBytes':sum(int(r['Installed-Size'])*1024 for r in rows)}
data=(json.dumps(manifest,sort_keys=True,indent=2)+'\n').encode();(out/'manifest.json').write_bytes(data)
(out/'InRelease').write_bytes((audit/'InRelease').read_bytes());(out/'signature-verification.txt').write_bytes((audit/'signature-verification.txt').read_bytes())
(out/'manifest.sha256').write_text(hashlib.sha256(data).hexdigest()+'\n')
print(json.dumps({k:v for k,v in manifest.items() if k!='packages'},indent=2));print('packages',len(rows))
