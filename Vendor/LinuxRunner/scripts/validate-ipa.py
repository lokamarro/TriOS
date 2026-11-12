#!/usr/bin/env python3
"""Release gate for LinuxRunner IPAs. Exits non-zero if any check fails; a failed
validation must stop publishing.

  scripts/validate-ipa.py LinuxRunner.ipa [--expect-sha256 HEX] [--expect-size N]
                          [--report report.json] [--allow-unsigned]

Checks the archive the way installers read it (signature, EOCD, central directory,
local headers), the bundle layout, Info.plist, the Mach-O main executable, the code
signature and entitlements, file hygiene, and LinuxRunner-specific requirements
(real guest vDSO, GitHub issue #3). Works on any IPA, including a downloaded release
asset (pass --expect-sha256/--expect-size to compare with the published values).
Static validation only: it does not prove that a given installer or device accepts
the IPA.
"""
import argparse, hashlib, json, plistlib, posixpath, stat, struct, subprocess, sys, tempfile, zipfile, os

checks = []
def check(name, ok, detail=''):
    checks.append({'check': name, 'result': 'PASS' if ok else 'FAIL', 'detail': detail})
    return ok

def macho_info(data):
    """Minimal 64-bit Mach-O parser: header, platform, encryption, code signature."""
    if len(data) < 32: return None
    magic, cputype, _, filetype, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', data, 0)
    if magic != 0xfeedfacf: return {'magic': hex(magic)}
    info = {'magic': hex(magic), 'cputype': cputype, 'filetype': filetype, 'commands': []}
    off = 32
    for _ in range(ncmds):
        if off + 8 > len(data): break
        cmd, size = struct.unpack_from('<II', data, off)
        info['commands'].append(cmd)
        if cmd == 0x32:   # LC_BUILD_VERSION
            platform, minos = struct.unpack_from('<II', data, off + 8)
            info['platform'] = platform; info['minos'] = f'{minos >> 16}.{(minos >> 8) & 0xff}'
        elif cmd == 0x2c:  # LC_ENCRYPTION_INFO_64
            info['cryptid'] = struct.unpack_from('<I', data, off + 16)[0]
        elif cmd == 0x1d:  # LC_CODE_SIGNATURE
            info['codesig'] = struct.unpack_from('<II', data, off + 8)
        off += size
    return info

def entitlements_from_signature(data, codesig):
    """Entitlements XML from the embedded signature SuperBlob (0xfade7171 blob)."""
    off, size = codesig
    blob = data[off:off + size]
    if len(blob) < 12 or struct.unpack_from('>I', blob, 0)[0] != 0xfade0cc0: return None
    count = struct.unpack_from('>I', blob, 8)[0]
    for i in range(count):
        _, boff = struct.unpack_from('>II', blob, 12 + 8 * i)
        magic, length = struct.unpack_from('>II', blob, boff)
        if magic == 0xfade7171: return blob[boff + 8: boff + length]
    return b''

def has_guest_vdso(binary):
    """An embedded aarch64 ELF shared object (the guest vDSO) whose symbol strings
    include __kernel_rt_sigreturn. A placeholder build embeds no ELF at all."""
    at = binary.find(b'\x7fELF\x02\x01\x01')
    while at >= 0:
        e_type, e_machine = struct.unpack_from('<HH', binary, at + 16)
        if e_type == 3 and e_machine == 183 and b'__kernel_rt_sigreturn' in binary[at:at + 8192]:
            return True
        at = binary.find(b'\x7fELF\x02\x01\x01', at + 1)
    return False

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ipa'); ap.add_argument('--expect-sha256'); ap.add_argument('--expect-size', type=int)
    ap.add_argument('--report'); ap.add_argument('--allow-unsigned', action='store_true')
    a = ap.parse_args()
    raw = open(a.ipa, 'rb').read()
    sha = hashlib.sha256(raw).hexdigest()
    check('size', a.expect_size is None or len(raw) == a.expect_size, f'{len(raw)} bytes')
    check('sha256', a.expect_sha256 is None or sha == a.expect_sha256.lower(), sha)
    # --- archive, as installers parse it -------------------------------------
    check('zip-local-signature', raw[:4] == b'PK\x03\x04', 'first bytes ' + raw[:4].hex() + (' (HTML page?)' if raw[:1] == b'<' else ''))
    eocd = raw.rfind(b'PK\x05\x06', max(0, len(raw) - 65557))
    if not check('zip-eocd', eocd >= 0, 'End of Central Directory record'):
        return finish(a, sha, len(raw))
    disk, cd_disk, n_here, n_total, cd_size, cd_off, clen = struct.unpack_from('<HHHHIIH', raw, eocd + 4)
    check('zip-eocd-at-end', eocd + 22 + clen == len(raw), f'eocd={eocd} comment={clen} size={len(raw)}')
    check('zip-single-disk', disk == 0 and cd_disk == 0 and n_here == n_total, '')
    check('zip-central-directory-bounds', cd_off + cd_size == eocd and raw[cd_off:cd_off + 4] == b'PK\x01\x02', f'offset={cd_off} size={cd_size}')
    try:
        z = zipfile.ZipFile(a.ipa)
    except zipfile.BadZipFile as e:
        check('zip-open', False, str(e)); return finish(a, sha, len(raw))
    infos = z.infolist()
    check('zip-entry-count', len(infos) == n_total and len(infos) > 0, f'{len(infos)} entries')
    check('zip-crc', z.testzip() is None, 'all entries decompress with matching CRC')
    check('zip-methods', all(i.compress_type in (0, 8) for i in infos), 'stored/deflate only')
    check('zip-no-zip64', all(i.file_size < 0xffffffff and i.header_offset < 0xffffffff for i in infos), '')
    check('zip-no-data-descriptors', all(not (i.flag_bits & 0x8) for i in infos), 'sizes in local headers (streaming parsers)')
    check('zip-no-encryption', all(not (i.flag_bits & 0x1) for i in infos), '')
    bad_local = []
    for i in infos:   # local header must agree with the central directory
        lh = raw[i.header_offset:i.header_offset + 30]
        if lh[:4] != b'PK\x03\x04': bad_local.append(i.filename); continue
        name_len = struct.unpack_from('<H', lh, 26)[0]
        if raw[i.header_offset + 30:i.header_offset + 30 + name_len].decode('utf-8', 'replace') != i.filename: bad_local.append(i.filename)
    check('zip-local-headers-match', not bad_local, ', '.join(bad_local[:3]))
    names = [i.filename for i in infos]
    check('zip-unique-names', len(set(names)) == len(names), '')
    unsafe = [n for n in names if n.startswith('/') or '\\' in n or '..' in n.split('/')]
    check('zip-safe-paths', not unsafe, ', '.join(unsafe[:3]))
    junk = [n for n in names if n.startswith('__MACOSX') or posixpath.basename(n.rstrip('/')) in ('.DS_Store',) or posixpath.basename(n).startswith('._')]
    check('no-mac-metadata', not junk, ', '.join(junk[:3]))
    # --- bundle --------------------------------------------------------------
    apps = sorted({n.split('/')[1] for n in names if n.startswith('Payload/') and n.count('/') >= 2 and n.split('/')[1].endswith('.app')})
    check('payload-layout', all(n.startswith('Payload/') for n in names) and len(apps) == 1, f'apps={apps}')
    if len(apps) != 1: return finish(a, sha, len(raw))
    app = 'Payload/' + apps[0] + '/'
    try:
        info = plistlib.loads(z.read(app + 'Info.plist'))
    except Exception as e:
        check('info-plist', False, str(e)); return finish(a, sha, len(raw))
    required = ['CFBundleExecutable', 'CFBundleIdentifier', 'CFBundleShortVersionString', 'CFBundleVersion', 'MinimumOSVersion', 'CFBundlePackageType']
    check('info-plist-keys', all(k in info for k in required), 'missing: ' + ', '.join(k for k in required if k not in info))
    check('info-plist-no-unexpanded-variables', '$(' not in json.dumps(info, default=str), '')
    check('info-plist-platform', info.get('CFBundleSupportedPlatforms') == ['iPhoneOS'] and info.get('CFBundlePackageType') == 'APPL', str(info.get('CFBundleSupportedPlatforms')))
    exe = app + info.get('CFBundleExecutable', '')
    if not check('executable-present', exe in names, exe): return finish(a, sha, len(raw))
    mode = z.getinfo(exe).external_attr >> 16
    check('executable-permission', bool(mode & stat.S_IXUSR), oct(mode))
    links = [i.filename for i in infos if stat.S_ISLNK(i.external_attr >> 16)]
    check('no-symlinks', not links, ', '.join(links[:3]))
    binary = z.read(exe)
    mi = macho_info(binary) or {}
    check('macho-arm64-executable', mi.get('magic') == '0xfeedfacf' and mi.get('cputype') == 0x0100000c and mi.get('filetype') == 2, json.dumps({k: mi.get(k) for k in ('magic', 'cputype', 'filetype')}))
    check('macho-ios-platform', mi.get('platform') == 2, f"platform={mi.get('platform')}")
    check('macho-minos-matches-plist', mi.get('minos') == str(info.get('MinimumOSVersion')), f"{mi.get('minos')} vs {info.get('MinimumOSVersion')}")
    check('macho-not-encrypted', mi.get('cryptid', 0) == 0, '')
    signed = 'codesig' in mi
    if not a.allow_unsigned:
        check('code-signature-present', signed, 'LC_CODE_SIGNATURE (ad-hoc is enough; installers re-sign)')
        check('code-resources-present', app + '_CodeSignature/CodeResources' in names, '')
    if signed:
        ents = entitlements_from_signature(binary, mi['codesig'])
        ok = ents is not None
        detail = ''
        if ents:
            try:
                parsed = plistlib.loads(ents); detail = ', '.join(sorted(parsed))
                ok = '$(' not in json.dumps(parsed)
            except Exception as e:
                ok = False; detail = str(e)
        check('entitlements-valid', ok, detail or 'none')
    # --- LinuxRunner requirements -------------------------------------------
    check('guest-vdso-present', has_guest_vdso(binary), 'embedded aarch64 ELF vDSO exporting __kernel_rt_sigreturn (issue #3)')
    check('rootfs-present', app + 'root.tar.gz' in names, '')
    return finish(a, sha, len(raw))

def finish(a, sha, size):
    failed = [c for c in checks if c['result'] == 'FAIL']
    report = {'ipa': os.path.abspath(a.ipa), 'size': size, 'sha256': sha, 'result': 'FAIL' if failed else 'PASS',
              'checks': checks, 'scope': 'static validation; not a device or installer test'}
    if a.report:
        with open(a.report, 'w') as f: json.dump(report, f, indent=2)
    for c in checks: print(f"{c['result']}  {c['check']:36} {c['detail']}")
    print(f"IPA_VALIDATION {report['result']} failed={len(failed)} sha256={sha}")
    return 1 if failed else 0

if __name__ == '__main__':
    sys.exit(main())
