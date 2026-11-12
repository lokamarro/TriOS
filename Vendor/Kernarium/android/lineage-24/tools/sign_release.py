# SPDX-License-Identifier: GPL-2.0-or-later
"""Sign a target-files archive with a private key directory.

`publish-release.py --signing-keys DIR` uses this module. DIR must be a private
(0700) directory of the invoking user, which the caller deletes; nothing here
copies its keys. A private (0700) work directory links it as `keys/`, the AOSP
releasetools sign the archive with those keys from that directory, the signing
log is checked so no APK or APEX stays signed with a key outside it, the OTA
certificates are replaced and checked so no public AOSP or LineageOS
certificate stays trusted, and the work directory is deleted again, with the
deletion verified, whether or not signing succeeded.

The signer names the certificate in each `otacerts.zip` after the path it was
given for the release key. It runs in the work directory, which also links the
top level of the source tree so its own relative reads still resolve, and it is
given `keys` as the default key directory, so the member is
`keys/releasekey.x509.pem` in every release and the image holds neither the
build host's directories nor a temporary name.

The key directory holds the Android platform set (`releasekey`, `platform`,
`media`, `shared`, `networkstack`, ... as `<name>.pk8` and `<name>.x509.pem`)
and one APEX key per container name (`<name>.pk8`, `<name>.x509.pem` and the
payload key `<name>.pem`). Nothing here prints key material or stores a key
path in a release.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import base64
import hashlib
import io
import os
import re
import shutil
import stat
import subprocess
import tempfile
import zipfile
from pathlib import Path

from release_common import OTA_CERTIFICATE_ARCHIVES, fail, signed_ota_certificate_member

SECURITY_DIR = "build/make/target/product/security/"
# Source directories of public certificates (AOSP's test keys and LineageOS's
# release certificate) that no signed release may trust.
PUBLIC_CERTIFICATE_DIRS = (SECURITY_DIR, "vendor/lineage/build/target/product/security/")
PLATFORM_KEYS = ("releasekey", "platform", "media", "shared")
# The link to the key directory, as the signer is given it for the default key
# mappings (the platform keys and the OTA certificate): relative to the work
# directory it runs in. The other keys keep the link's absolute path.
KEYS_DIRECTORY = "keys"
# The one member every signed otacerts.zip holds is the release certificate, named after that
# relative path (release_common.OTA_CERTIFICATE_MEMBERS, by build variant).
# `signing: NAME [container] (KEY)` and the continuation `: NAME payload (KEY)`;
# the tool may print the next record on the same line, so search the text.
SIGNING_RECORD = re.compile(
    r"(?:(?<!NOT )\bsigning:|(?<=\s):)\s+(\S+)\s+(?:container\s+|payload\s+)?\(([^)]+)\)"
)
FIELD = re.compile(r'(\w+)="([^"]*)"')


def check_private_directory(keys: Path) -> None:
    """Refuse a key directory other users could read."""
    if not keys.is_absolute() or any(part in (".", "..") for part in keys.parts):
        fail("signing key directory must be an absolute normalized path")
    if keys.is_symlink() or not keys.is_dir():
        fail("signing key directory must be a regular directory")
    info = keys.lstat()
    if info.st_uid != os.getuid():
        fail("signing key directory is not owned by the invoking user")
    if info.st_mode & 0o077:
        fail("signing key directory must not be accessible to group or others")
    for entry in sorted(keys.iterdir()):
        entry_info = entry.lstat()
        if stat.S_ISLNK(entry_info.st_mode):
            target = os.readlink(entry)
            if os.path.isabs(target) or "/" in target:
                fail(f"signing key link must name a sibling file: {entry.name}")
            continue
        if not stat.S_ISREG(entry_info.st_mode):
            fail(f"signing key directory holds a non-regular entry: {entry.name}")
        if entry_info.st_mode & 0o077:
            fail(f"signing key file is accessible to group or others: {entry.name}")


def read_member(archive: zipfile.ZipFile, member: str) -> str:
    try:
        return archive.read(member).decode("utf-8")
    except (KeyError, UnicodeError) as error:
        fail(f"target files omit {member}: {error}")


def key_arguments(target_files: Path, keys: Path) -> list[str]:
    """The sign_target_files_apks flags that route every non-platform key."""
    with zipfile.ZipFile(target_files) as archive:
        apexkeys = read_member(archive, "META/apexkeys.txt")
        apkcerts = read_member(archive, "META/apkcerts.txt")
    flags: list[str] = []
    required = {f"{base}{suffix}" for base in PLATFORM_KEYS for suffix in (".pk8", ".x509.pem")}
    for line in apexkeys.splitlines():
        fields = dict(FIELD.findall(line))
        if "name" not in fields:
            continue
        name, container = fields["name"], fields.get("container_private_key", "")
        if container == "PRESIGNED":
            continue
        # A container the tree signs with an AOSP public key gets the key that
        # carries the APEX's own name; the others keep the tree's key name.
        if container.startswith(SECURITY_DIR):
            base = re.sub(r"\.c?apex$", "", name)
        else:
            base = container.rsplit("/", 1)[-1].removesuffix(".pk8")
        flags += ["--extra_apks", f"{name}={keys}/{base}"]
        flags += ["--extra_apex_payload_key", f"{name}={keys}/{base}.pem"]
        required.update(f"{base}{suffix}" for suffix in (".pk8", ".x509.pem", ".pem"))
    for line in apkcerts.splitlines():
        fields = dict(FIELD.findall(line))
        if "name" not in fields or fields.get("partition") == "data":
            continue
        certificate = fields.get("certificate", "")
        if certificate == "PRESIGNED" or certificate.startswith(SECURITY_DIR):
            continue
        # An APK inside an APEX, or a module resource package, that the tree
        # signs with its own key: the release key signs it, as the LineageOS
        # signing guide does.
        flags += ["--extra_apks", f"{fields['name']}={keys}/releasekey"]
    missing = sorted(name for name in required if not (keys / name).is_file())
    if missing:
        fail("signing key directory lacks: " + ", ".join(missing[:12]))
    return flags


def check_signing_log(log: str, prefixes: tuple[str, ...]) -> tuple[int, list[str]]:
    """Every signed file must use a key from the key directory, named by
    either of the paths the signer is given for it."""
    signed = 0
    for name, key in SIGNING_RECORD.findall(log):
        signed += 1
        if not key.startswith(tuple(f"{prefix}/" for prefix in prefixes)):
            fail(f"{name} stayed signed with a key outside the release key set")
    unsigned = [
        line.strip().removeprefix("NOT signing:").split()[0]
        for line in log.splitlines()
        if line.strip().startswith("NOT signing:") and line.strip() != "NOT signing:"
    ]
    if signed == 0:
        fail("the signing log records no signed APK or APEX")
    return signed, unsigned


def pem_digest(pem: bytes) -> str:
    """SHA-256 of the DER form of one PEM certificate (public data)."""
    body = "".join(
        line
        for line in pem.decode("ascii").splitlines()
        if line and not line.startswith("-----")
    )
    return hashlib.sha256(base64.b64decode(body)).hexdigest()


def certificate_digests(keys: Path) -> dict[str, str]:
    """SHA-256 of the DER form of each platform certificate (public data)."""
    return {
        name: pem_digest((keys / f"{name}.x509.pem").read_bytes())
        for name in PLATFORM_KEYS
    }


def check_ota_certificates(
    signed_output: Path, keys: Path, aosp_source: Path, build_variant: str
) -> None:
    """Every otacerts.zip, in the system and the recovery, must trust the
    release certificate and no public AOSP or LineageOS certificate.

    Without the OTA key replacement the recovery and the system keep the AOSP
    public test certificate, which would let anyone sideload an update."""
    member = signed_ota_certificate_member(build_variant)
    release = pem_digest((keys / "releasekey.x509.pem").read_bytes())
    public = {
        pem_digest(path.read_bytes())
        for directory in PUBLIC_CERTIFICATE_DIRS
        for path in sorted((aosp_source / directory).glob("*.x509.pem"))
    }
    if not public:
        fail("the source tree holds no public certificates to check against")
    with zipfile.ZipFile(signed_output) as archive:
        names = [name for name in archive.namelist() if name.endswith("/otacerts.zip")]
        for required in OTA_CERTIFICATE_ARCHIVES:
            if required not in names:
                fail(f"the signed target files omit {required}")
        for name in names:
            with zipfile.ZipFile(io.BytesIO(archive.read(name))) as certificates:
                members = sorted(certificates.namelist())
                digests = {
                    pem_digest(certificates.read(member))
                    for member in members
                    if member.endswith(".pem")
                }
            if release not in digests:
                fail(f"{name} omits the release certificate")
            if digests & public:
                fail(f"{name} still trusts a public AOSP or LineageOS certificate")
            if members != [member]:
                fail(
                    f"{name} holds the members {members}, not only {member}: "
                    "the signer named them after a path of the build host"
                )


def run_logged(arguments: list[str], *, cwd: Path, env: dict[str, str]) -> str:
    result = subprocess.run(
        arguments, cwd=cwd, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, check=False,
    )
    if result.returncode != 0:
        fail("sign_target_files_apks failed:\n" + "\n".join(result.stdout.splitlines()[-25:]))
    return result.stdout


def private_work(keys: Path, aosp_source: Path, parent: Path | None) -> Path:
    """The private work directory: `keys/`, a link to the key directory, and a
    link to each top-level entry of the source tree, so the signer, which reads
    old certificates and key mappings by their source-relative paths, runs
    here. No key is copied."""
    check_private_directory(keys)
    if not aosp_source.is_dir():
        fail("the source tree is not a directory")
    if (aosp_source / KEYS_DIRECTORY).exists() or (aosp_source / KEYS_DIRECTORY).is_symlink():
        fail(f"the source tree has a top-level {KEYS_DIRECTORY}, the name of the key link")
    old_umask = os.umask(0o077)
    work = Path(tempfile.mkdtemp(prefix="uml-signing-", dir=parent))
    try:
        os.chmod(work, 0o700)
        os.symlink(os.path.abspath(keys), work / KEYS_DIRECTORY)
        for entry in sorted(aosp_source.iterdir()):
            os.symlink(os.path.abspath(entry), work / entry.name)
    except BaseException:
        delete_private(work)
        raise
    finally:
        os.umask(old_umask)
    return work


def delete_private(work: Path) -> None:
    # The links are removed first, by name, so that no deletion can reach into the source tree
    # or the key directory.
    if work.is_dir() and not work.is_symlink():
        for entry in work.iterdir():
            if entry.is_symlink():
                entry.unlink()
    shutil.rmtree(work, ignore_errors=True)
    if work.exists() or work.is_symlink():
        fail("the private signing work directory could not be deleted")


def sign_target_files(
    *,
    target_files: Path,
    signed_output: Path,
    keys: Path,
    build_variant: str,
    host_bin: Path,
    aosp_source: Path,
    jdk_bin: Path,
    temporary_parent: Path | None = None,
) -> tuple[int, list[str], dict[str, str]]:
    """Write a signed copy of the target files of a user build.

    Returns the signed file count, the presigned files left as published and
    the SHA-256 of each platform certificate."""
    signed_ota_certificate_member(build_variant)    # refuses a variant that is not signed
    if signed_output.exists() or signed_output.is_symlink():
        fail("signed target-files output already exists")
    tool = host_bin / "sign_target_files_apks"
    if not tool.exists():
        fail("sign_target_files_apks is missing from the host tools (build the otatools)")
    work = private_work(keys, aosp_source, temporary_parent)
    copy = work / KEYS_DIRECTORY      # the link; the signer reads the keys through it
    try:
        arguments = [
            str(tool),
            "-o",
            "-d", KEYS_DIRECTORY,
            *key_arguments(target_files, copy),
            str(target_files),
            str(signed_output),
        ]
        env = os.environ.copy()
        env["PATH"] = f"{host_bin}:{jdk_bin}:{env.get('PATH', '/usr/bin:/bin')}"
        log = run_logged(arguments, cwd=work, env=env)
        signed, unsigned = check_signing_log(log, (KEYS_DIRECTORY, str(copy)))
        check_ota_certificates(signed_output, copy, aosp_source, build_variant)
        # Signing rebuilds the partition images but not the data seed.
        with zipfile.ZipFile(target_files) as original, zipfile.ZipFile(
            signed_output, "a", zipfile.ZIP_DEFLATED
        ) as signed_archive:
            if "IMAGES/userdata.img" not in signed_archive.namelist():
                signed_archive.writestr(
                    "IMAGES/userdata.img", original.read("IMAGES/userdata.img")
                )
        return signed, unsigned, certificate_digests(copy)
    finally:
        delete_private(work)
