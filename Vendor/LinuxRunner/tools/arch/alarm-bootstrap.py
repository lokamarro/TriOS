#!/usr/bin/env python3
"""Build a minimal Arch Linux ARM (aarch64) userspace tarball from official packages.

Development and test tool for LinuxRunner's Arch Linux ARM support (the app installer follows the
same trust model). It never trusts the repository database alone: Arch Linux ARM does not sign its
repository databases, so every package is checked twice before use:

  * SHA-256 against the repository database entry, and
  * the package's detached OpenPGP signature (the database's %PGPSIG%) with gpg, using a keyring
    that contains only the Arch Linux ARM Build System key, whose fingerprint must be
    68B3537F39A313B3E574D06777193F152BDBE6A6 (archlinuxarm.org/about/package-signing).

The output tarball holds the package files plus a pacman local database (/var/lib/pacman/local),
so pacman inside LinuxRunner knows what is installed. Package install scripts and ALPM hooks do not
run here; the first boot inside LinuxRunner reinstalls the set with pacman, which runs them (see
tests/linuxrunner/arch/arch_firstboot.sh).

  alarm-bootstrap.py --keyring archlinuxarm.gpg --cache DIR --output rootfs.tar [PACKAGE...]
"""
import argparse, base64, hashlib, io, lzma, os, re, subprocess, sys, tarfile, tempfile, time, urllib.request

ALARM_FPR = '68B3537F39A313B3E574D06777193F152BDBE6A6'
DEFAULT_MIRROR = 'https://fl.us.mirror.archlinuxarm.org/aarch64'
DEFAULT_PACKAGES = ['filesystem', 'bash', 'coreutils', 'glibc', 'pacman', 'archlinuxarm-keyring', 'sed', 'grep',
                    'gawk', 'findutils', 'tar', 'gzip', 'xz', 'zstd', 'which', 'less', 'procps-ng', 'shadow',
                    'util-linux', 'ca-certificates', 'iana-etc', 'sudo', 'nano', 'file', 'diffutils']
METADATA = {'.PKGINFO', '.MTREE', '.BUILDINFO', '.INSTALL', '.CHANGELOG'}


def fetch(url, dest):
    if os.path.exists(dest):
        return dest
    tmp = dest + '.part'
    with urllib.request.urlopen(url, timeout=120) as r, open(tmp, 'wb') as f:
        while chunk := r.read(1 << 20):
            f.write(chunk)
    os.replace(tmp, dest)
    return dest


def parse_db(path):
    """Repository database -> {name: fields}. Each entry is NAME-VER/desc, plus NAME-VER/depends in
    the older layout Arch Linux ARM still uses (DEPENDS/PROVIDES/CONFLICTS live there)."""
    entries = {}
    with tarfile.open(path) as t:
        for m in t.getmembers():
            if not m.isfile() or m.name.count('/') != 1 or m.name.split('/')[1] not in ('desc', 'depends'):
                continue
            fields, key = entries.setdefault(m.name.split('/')[0], {}), None
            for line in t.extractfile(m).read().decode().splitlines():
                if re.fullmatch(r'%[A-Z0-9]+%', line):
                    key = line.strip('%'); fields[key] = []
                elif line and key:
                    fields[key].append(line)
    return {f['NAME'][0]: f for f in entries.values() if 'NAME' in f}


def strip_version(dep):
    return re.split(r'[<>=]', dep, 1)[0]


def resolve(pkgs, wanted):
    provides = {}
    for name, f in pkgs.items():
        for p in f.get('PROVIDES', []):
            provides.setdefault(strip_version(p), name)
    order, seen, stack = [], set(), list(wanted)
    while stack:
        dep = strip_version(stack.pop())
        name = dep if dep in pkgs else provides.get(dep)
        if name is None:
            raise SystemExit(f'unresolvable dependency: {dep}')
        if name in seen:
            continue
        seen.add(name); order.append(name)
        stack.extend(pkgs[name].get('DEPENDS', []))
    return order


def verify(path, fields, gnupghome):
    sha = hashlib.sha256(open(path, 'rb').read()).hexdigest()
    if sha != fields['SHA256SUM'][0]:
        raise SystemExit(f'SHA-256 mismatch: {path}')
    with tempfile.NamedTemporaryFile(suffix='.sig') as sig:
        sig.write(base64.b64decode(''.join(fields['PGPSIG']))); sig.flush()
        out = subprocess.run(['gpg', '--homedir', gnupghome, '--status-fd', '1', '--verify', sig.name, path],
                             capture_output=True, text=True).stdout
    if not re.search(rf'^\[GNUPG:\] VALIDSIG {ALARM_FPR} ', out, re.M):
        raise SystemExit(f'OpenPGP signature not valid for the Arch Linux ARM key: {path}\n{out}')


def local_desc(fields, explicit):
    keep = ['NAME', 'VERSION', 'BASE', 'DESC', 'URL', 'ARCH', 'BUILDDATE', 'PACKAGER', 'LICENSE', 'GROUPS',
            'REPLACES', 'DEPENDS', 'OPTDEPENDS', 'CONFLICTS', 'PROVIDES']
    out = []
    for k in keep:
        if k in fields:
            out += [f'%{k}%', *fields[k], '']
        if k == 'BUILDDATE':
            out += ['%INSTALLDATE%', str(int(time.time())), '']
    out += ['%SIZE%', fields.get('ISIZE', ['0'])[0], '', '%REASON%', '0' if explicit else '1', '',
            '%VALIDATION%', 'pgp', '']
    return '\n'.join(out) + '\n'


def add_dir(out, name, made):
    """Directory entry (fakefsify does not create missing parent directories)."""
    if name in made:
        return
    made.add(name)
    ti = tarfile.TarInfo(name); ti.type = tarfile.DIRTYPE; ti.mode = 0o755; ti.mtime = int(time.time())
    out.addfile(ti)


def add_bytes(out, name, data, made, mode=0o644):
    parts = name.split('/')
    for i in range(1, len(parts)):
        add_dir(out, '/'.join(parts[:i]), made)
    ti = tarfile.TarInfo(name); ti.size = len(data); ti.mode = mode; ti.mtime = int(time.time())
    out.addfile(ti, io.BytesIO(data))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--keyring', required=True, help='archlinuxarm.gpg from the archlinuxarm-keyring package')
    ap.add_argument('--cache', required=True)
    ap.add_argument('--output', required=True)
    ap.add_argument('--mirror', default=DEFAULT_MIRROR)
    ap.add_argument('packages', nargs='*', default=DEFAULT_PACKAGES)
    a = ap.parse_args()
    os.makedirs(a.cache, exist_ok=True)
    gnupghome = tempfile.mkdtemp(prefix='alarm-gpg-')
    subprocess.run(['gpg', '--homedir', gnupghome, '--quiet', '--import', a.keyring], check=True, capture_output=True)
    fprs = re.findall(r'^fpr:+([0-9A-F]+):', subprocess.run(['gpg', '--homedir', gnupghome, '--with-colons', '--list-keys'],
                      capture_output=True, text=True).stdout, re.M)
    if ALARM_FPR not in fprs:
        raise SystemExit('keyring does not contain the Arch Linux ARM Build System key')
    pkgs = {}
    for repo in ('core', 'alarm', 'extra'):
        db = fetch(f'{a.mirror}/{repo}/{repo}.db', os.path.join(a.cache, f'{repo}.db'))
        for name, f in parse_db(db).items():
            f['REPO'] = [repo]; pkgs.setdefault(name, f)
    order = resolve(pkgs, a.packages)
    total = sum(int(pkgs[n]['CSIZE'][0]) for n in order)
    print(f'ALARM_BOOTSTRAP packages={len(order)} download_bytes={total}', flush=True)
    seen_paths = set()
    with tarfile.open(a.output, 'w', format=tarfile.GNU_FORMAT) as out:
        for name in order:
            f = pkgs[name]
            path = fetch(f"{a.mirror}/{f['REPO'][0]}/{f['FILENAME'][0]}", os.path.join(a.cache, f['FILENAME'][0]))
            verify(path, f, gnupghome)
            files = []
            with tarfile.open(path) as p:
                for m in p.getmembers():
                    if m.name in METADATA:
                        continue
                    files.append(m.name + ('/' if m.isdir() else ''))
                    if m.name.rstrip('/') in seen_paths and m.isdir():
                        continue
                    seen_paths.add(m.name.rstrip('/'))
                    out.addfile(m, p.extractfile(m) if m.isfile() else None)
            ldir = f"var/lib/pacman/local/{name}-{f['VERSION'][0]}"
            add_bytes(out, ldir + '/desc', local_desc(f, name in a.packages).encode(), seen_paths)
            add_bytes(out, ldir + '/files', ('%FILES%\n' + '\n'.join(sorted(files)) + '\n').encode(), seen_paths)
            print(f'ALARM_PACKAGE {name} {f["VERSION"][0]} sha256+pgp ok', flush=True)
        add_bytes(out, 'var/lib/pacman/local/ALPM_DB_VERSION', b'9\n', seen_paths)
    print(f'ALARM_BOOTSTRAP_OK output={a.output}')


if __name__ == '__main__':
    main()
