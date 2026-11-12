#!/usr/bin/env python3
"""Inspect a re-signed LinuxRunner IPA without installing/running/importing keys.

Usage: python3 scripts/inspect-signed-ipa.py signed.ipa [report-directory]
Reports local signature integrity and profile metadata; not iOS trust/revocation.
"""
import datetime, hashlib, json, os, pathlib, plistlib, stat, subprocess, sys, tempfile, zipfile
os.environ.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
if len(sys.argv) not in (2,3):
    raise SystemExit(__doc__)
ipa=pathlib.Path(sys.argv[1]).resolve()
out=pathlib.Path(sys.argv[2] if len(sys.argv)==3 else 'build/device-diagnostics/signing-inspection').resolve()
out.mkdir(parents=True,exist_ok=True)
def run(*args):
    p=subprocess.run(args,capture_output=True,text=True)
    return dict(exit_code=p.returncode,stdout=p.stdout,stderr=p.stderr)
result={'ipa_sha256':hashlib.sha256(ipa.read_bytes()).hexdigest(), 'inspected_at_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(), 'physical_installation_verified':False, 'warnings':[]}
with tempfile.TemporaryDirectory(prefix='linuxrunner-resign-') as directory:
    with zipfile.ZipFile(ipa) as z:
        assert z.testzip() is None, 'Corrupt ZIP'
        for item in z.infolist():
            path=pathlib.PurePosixPath(item.filename)
            assert not path.is_absolute() and '..' not in path.parts, 'Unsafe ZIP path'
            assert not stat.S_ISLNK(item.external_attr >> 16), 'Symlink entries are not supported by this inspector'
        z.extractall(directory)
    apps=[p for p in (pathlib.Path(directory)/'Payload').glob('*.app') if p.is_dir() and not p.name.startswith('._')]
    metadata_entries=[n for n in z.namelist() if any(part.startswith('._') or part=='__MACOSX' for part in pathlib.PurePosixPath(n).parts)]
    if metadata_entries: result['warnings'].append('macOS AppleDouble metadata entries present in IPA.')
    assert len(apps)==1,'Expected one main app'
    app=apps[0]; info=plistlib.loads((app/'Info.plist').read_bytes()); binary=app/info['CFBundleExecutable']
    result['bundle_id']=info['CFBundleIdentifier'];result['build']=info['CFBundleVersion']
    result['platforms']=info['CFBundleSupportedPlatforms'];result['diagnostic_launch']=info.get('LinuxRunnerDiagnostic')
    result['main_arch']=run('xcrun','lipo','-archs',str(binary))
    result['main_build_platform']=run('xcrun','vtool','-show-build',str(binary))
    result['binary_uuid']=run('xcrun','dwarfdump','--uuid',str(binary))
    result['codesign_verification']=run('codesign','--verify','--deep','--strict','--verbose=4',str(app))
    result['signature_details']=run('codesign','-dvvv',str(app))
    ent=run('codesign','-d','--entitlements',':-',str(app))
    result['entitlements_read']=ent
    try: signed_entitlements=plistlib.loads(ent['stdout'].encode()); result['signed_entitlements']=signed_entitlements
    except Exception: signed_entitlements={};result['warnings'].append('Could not decode signed XML entitlements; inspect raw command output.')
    profile=app/'embedded.mobileprovision'
    if profile.exists():
        decoded=run('security','cms','-D','-i',str(profile))
        if decoded['exit_code']==0:
            p=plistlib.loads(decoded['stdout'].encode()); pe=p.get('Entitlements',{})
            expiration=p.get('ExpirationDate'); utc_now=datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
            result['profile']={k:p.get(k) for k in ('UUID','Name','TeamIdentifier','ApplicationIdentifierPrefix','ProvisionsAllDevices')}
            result['profile']['expires_utc']=expiration.isoformat() if expiration else None
            result['profile']['expired']=bool(expiration and expiration<utc_now)
            result['profile']['registered_device_count']=len(p.get('ProvisionedDevices',[]))
            result['profile']['entitlements']=pe
            if result['profile']['expired']:result['warnings'].append('Provisioning profile is expired.')
            permitted=pe.get('application-identifier',''); signed=signed_entitlements.get('application-identifier','')
            if permitted and signed and not (signed==permitted or permitted.endswith('*') and signed.startswith(permitted[:-1])):
                result['warnings'].append('Signed application-identifier does not match profile allowance.')
            extra=set(signed_entitlements.get('com.apple.security.application-groups',[]))-set(pe.get('com.apple.security.application-groups',[]))
            if extra:result['warnings'].append('Signed app groups are not allowed by the embedded profile.')
        else:result['profile_decode']=decoded;result['warnings'].append('Could not decode embedded profile.')
    else:result['warnings'].append('No embedded provisioning profile.')
    if result['codesign_verification']['exit_code']!=0:result['warnings'].append('Local code signature integrity verification failed.')
    if result['platforms']!=['iPhoneOS'] or 'SIMULATOR' in result['main_build_platform']['stdout']:
        result['warnings'].append('Main binary is not a physical-iOS artifact.')
    result['limitations']='Local signature/profile checks cannot prove certificate trust, revocation, profile bans, device eligibility, installation or runtime success. Capture the installer error on the intended phone.'
(out/'signed-ipa-inspection.json').write_text(json.dumps(result,indent=2,default=str)+'\n')
print(out/'signed-ipa-inspection.json')
print('Warnings:', '; '.join(result['warnings']) or 'none detected; physical installation still unverified')
