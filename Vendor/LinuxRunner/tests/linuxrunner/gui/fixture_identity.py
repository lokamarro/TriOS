"""Test-only binary identity checks; no runtime/application special cases."""
import hashlib
import json
import pathlib
import struct


def check_identity(rootfs, profile):
    data = (pathlib.Path(rootfs) / 'data').resolve()
    if profile['executable'] not in profile['files']:
        raise ValueError('Fixture executable must have a pinned hash')
    observed = {}
    for name, expected in profile['files'].items():
        guest = pathlib.PurePosixPath(name)
        if not guest.is_absolute() or '..' in guest.parts:
            raise ValueError('Invalid guest identity path')
        path = (data / str(guest).lstrip('/')).resolve()
        if not path.is_relative_to(data):
            raise ValueError('Identity resource escapes fixture')
        content = path.read_bytes()
        actual = hashlib.sha256(content).hexdigest()
        if actual != expected:
            raise ValueError(f'STALE_TRACE_REJECTED: {name} expected={expected} actual={actual}')
        observed[name] = {'sha256': actual, 'bytes': len(content)}
    executable = (data / profile['executable'].lstrip('/')).read_bytes()
    if executable[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', executable, 18)[0] != 183:
        raise ValueError('Fixture executable is not ARM64 little-endian ELF64')
    phoff = struct.unpack_from('<Q', executable, 32)[0]
    phsize, phnum = struct.unpack_from('<HH', executable, 54)
    interpreter = None
    for i in range(phnum):
        kind, _, offset, _, _, size, _, _ = struct.unpack_from('<IIQQQQQQ', executable, phoff + i * phsize)
        if kind == 3:
            interpreter = executable[offset:offset + size].rstrip(b'\0').decode()
    if interpreter != profile['interpreter']:
        raise ValueError('STALE_TRACE_REJECTED: ELF interpreter changed')
    return {'label': profile['label'], 'executable': profile['executable'],
            'interpreter': interpreter, 'files': observed}


def load_profile(path):
    return json.loads(pathlib.Path(path).read_text())


def check_load_bases(maps, profile):
    bases = dict(profile.get('load_bases', {}))
    if profile.get('load_base'):
        bases[profile['executable']] = profile['load_base']
    for name, base in bases.items():
        prefix = f'{int(base, 0):012x}-'
        if not any(line.startswith(prefix) and line.split()[-1] == name
                   for line in maps.splitlines() if line.split()):
            raise ValueError(f'STALE_TRACE_REJECTED: mapping changed for {name}; reject absolute PCs')
