#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check release signing: key routing, key-directory hygiene, work-directory deletion and the OTA certificate name."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import io
import os
import stat
import sys
import tempfile
import zipfile
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"release signing fixture: {message}")


APEXKEYS = "\n".join([
    'name="com.android.adbd.apex" public_key="packages/modules/adb/apex/com.android.adbd.avbpubkey" '
    'private_key="packages/modules/adb/apex/com.android.adbd.pem" '
    'container_certificate="packages/modules/adb/apex/com.android.adbd.x509.pem" '
    'container_private_key="packages/modules/adb/apex/com.android.adbd.pk8" partition="system"',
    'name="com.android.tethering.apex" public_key="x" private_key="packages/modules/Connectivity/apex/com.android.tethering.pem" '
    'container_certificate="build/make/target/product/security/testkey.x509.pem" '
    'container_private_key="build/make/target/product/security/testkey.pk8" partition="system"',
    'name="com.android.apex.cts.shim.apex" public_key="PRESIGNED" private_key="PRESIGNED" '
    'container_certificate="PRESIGNED" container_private_key="PRESIGNED" partition="system"',
    "",
])
APKCERTS = "\n".join([
    'name="Settings.apk" certificate="build/make/target/product/security/platform.x509.pem" '
    'private_key="build/make/target/product/security/platform.pk8" partition="system_ext"',
    'name="WifiDialog.apk" certificate="packages/modules/Wifi/WifiDialog/certs/com.android.wifi.dialog.x509.pem" '
    'private_key="packages/modules/Wifi/WifiDialog/certs/com.android.wifi.dialog.pk8" partition="system"',
    'name="Presigned.apk" certificate="PRESIGNED" private_key="" partition="product"',
    'name="CtsHelper.apk" certificate="cts/certs/cts-testkey1.x509.pem" private_key="cts/certs/cts-testkey1.pk8" partition="data"',
    "",
])
PLATFORM_CERT = (
    "-----BEGIN CERTIFICATE-----\n"
    "AAECAwQFBgcICQ==\n"
    "-----END CERTIFICATE-----\n"
)
# The AOSP public test certificate and the LineageOS release certificate.
AOSP_CERT = (
    "-----BEGIN CERTIFICATE-----\n"
    "CgsMDQ4PEBESEw==\n"
    "-----END CERTIFICATE-----\n"
)
LINEAGE_CERT = (
    "-----BEGIN CERTIFICATE-----\n"
    "FBUWFxgZGhscHQ==\n"
    "-----END CERTIFICATE-----\n"
)


def otacerts(*certificates: str) -> bytes:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for index, certificate in enumerate(certificates):
            archive.writestr(f"cert{index}.x509.pem", certificate)
    return buffer.getvalue()

FAKE_SIGNER = r'''#!/usr/bin/env python3
import io, os, sys, zipfile
args = sys.argv[1:]
open(RECORD, "w").write("\n".join(args) + "\n")
open(RECORD + ".cwd", "w").write(os.getcwd())
mode = MODE
if mode == "fail":
    print("boom")
    sys.exit(3)
keys = args[args.index("-d") + 1]
# The key directory is linked, never copied: no key file lies in the work directory itself.
if not os.path.islink(keys):
    print("the key directory is not a link")
    sys.exit(5)
open(RECORD + ".keys", "w").write(os.path.realpath(keys))
for root, directories, files in os.walk("."):
    for name in files:
        if name.endswith((".pk8", ".pem")):
            print("a key file in the work directory: " + os.path.join(root, name))
            sys.exit(6)
# The real tool reads the old certificates by their source-relative paths and the new ones by the path it is given.
for path in ("build/make/target/product/security/testkey.x509.pem", keys + "/releasekey.x509.pem"):
    if not os.path.isfile(path):
        print("cannot read " + path)
        sys.exit(4)
# ZipFile.write names a member after the path it is given, without a leading separator.
key = (os.path.abspath(keys) if mode == "abs-ota" else keys) + "/releasekey.x509.pem"
member = os.path.normpath(os.path.splitdrive(key)[1]).lstrip(os.sep)
def rename(data):
    out = io.BytesIO()
    with zipfile.ZipFile(io.BytesIO(data)) as source, zipfile.ZipFile(out, "w") as target:
        for name in source.namelist():
            target.writestr(member if name == "cert0.x509.pem" else name, source.read(name))
    return out.getvalue()
with zipfile.ZipFile(args[-2]) as source, zipfile.ZipFile(args[-1], "w") as target:
    for item in source.infolist():
        data = source.read(item.filename)
        target.writestr(item.filename, rename(data) if item.filename.endswith("/otacerts.zip") else data)
extra = [a for a in args if a.startswith("com.android.adbd.apex=")][0].split("=", 1)[1]
print("    signing: Settings.apk                                                            (%s/platform)" % keys)
print("    signing: com.android.adbd.apex                                                   container (%s)" % extra)
print("           : com.android.adbd.apex                                                   payload   (%s.pem)" % extra)
if mode == "leak":
    print("    signing: com.android.tethering.apex                                              container (build/make/target/product/security/testkey)")
print("NOT signing: Presigned.apk (skipped due to special cert string)")
'''


def build_target_files(path: Path, ota: str) -> None:
    """The fake signer copies this archive, so it holds the signed OTA certificates."""
    system, recovery = {
        "ok": ((PLATFORM_CERT,), (PLATFORM_CERT,)),
        "ota-aosp": ((PLATFORM_CERT,), (PLATFORM_CERT, AOSP_CERT)),
        "ota-lineage": ((PLATFORM_CERT,), (PLATFORM_CERT, LINEAGE_CERT)),
        "ota-system-aosp": ((AOSP_CERT, PLATFORM_CERT), (PLATFORM_CERT,)),
        "ota-missing": ((PLATFORM_CERT,), (LINEAGE_CERT,)),
    }[ota]
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("META/apexkeys.txt", APEXKEYS)
        archive.writestr("META/apkcerts.txt", APKCERTS)
        archive.writestr("IMAGES/userdata.img", b"seed")
        archive.writestr("SYSTEM/etc/security/otacerts.zip", otacerts(*system))
        archive.writestr(
            "RECOVERY/RAMDISK/system/etc/security/otacerts.zip", otacerts(*recovery)
        )


def build_keys(path: Path, *, skip: str | None = None, loose: bool = False) -> None:
    path.mkdir(mode=0o700)
    names = ["releasekey", "platform", "media", "shared"]
    files = []
    for base in names:
        files += [f"{base}.pk8", f"{base}.x509.pem"]
    for base in ("com.android.adbd", "com.android.tethering"):
        files += [f"{base}.pk8", f"{base}.x509.pem", f"{base}.pem"]
    for name in files:
        if name == skip:
            continue
        target = path / name
        target.write_text(PLATFORM_CERT if name.endswith(".x509.pem") else "key-material\n")
        os.chmod(target, 0o640 if loose and name == "media.pk8" else 0o600)
    os.symlink("releasekey.pk8", path / "testkey.pk8")


OTA_ZIPS = ("SYSTEM/etc/security/otacerts.zip", "RECOVERY/RAMDISK/system/etc/security/otacerts.zip")


def ota_members(signed: Path) -> dict[str, list[str]]:
    """The member names of each otacerts.zip of a signed archive."""
    with zipfile.ZipFile(signed) as archive:
        return {
            name: sorted(zipfile.ZipFile(io.BytesIO(archive.read(name))).namelist())
            for name in OTA_ZIPS
        }


def entries(path: Path) -> list[str]:
    return sorted(entry.name for entry in path.iterdir())


def run_case(
    sign_release, work: Path, name: str, mode: str, *, skip: str | None = None, loose: bool = False,
    key_mode: int = 0o700, ota: str = "ok", collide: bool = False, build_variant: str = "user",
):
    case = work / name
    case.mkdir()
    target_files = case / "target-files.zip"
    build_target_files(target_files, ota)
    security = case / "build/make/target/product/security"
    security.mkdir(parents=True)
    (security / "testkey.x509.pem").write_text(AOSP_CERT)
    lineage = case / "vendor/lineage/build/target/product/security"
    lineage.mkdir(parents=True)
    (lineage / "lineage.x509.pem").write_text(LINEAGE_CERT)
    keys = case / "source-keys"
    build_keys(keys, skip=skip, loose=loose)
    if collide:
        (case / "keys").mkdir()
    os.chmod(keys, key_mode)
    host_bin = case / "bin"
    host_bin.mkdir()
    record = case / "record.txt"
    signer = host_bin / "sign_target_files_apks"
    signer.write_text(
        FAKE_SIGNER.replace("RECORD", repr(str(record))).replace("MODE", repr(mode))
    )
    os.chmod(signer, 0o755)
    scratch = case / "scratch"
    scratch.mkdir()
    outcome: dict[str, object] = {"record": record, "scratch": scratch, "keys": keys}
    try:
        outcome["result"] = sign_release.sign_target_files(
            target_files=target_files,
            signed_output=case / "signed.zip",
            keys=keys,
            build_variant=build_variant,
            host_bin=host_bin,
            aosp_source=case,
            jdk_bin=case / "jdk",
            temporary_parent=scratch,
        )
        outcome["signed_zip"] = case / "signed.zip"
        outcome["case"] = case
    except SystemExit as error:
        outcome["error"] = str(error)
    return outcome


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.source_root / "android/lineage-24/tools"))
    import sign_release  # noqa: E402

    with tempfile.TemporaryDirectory() as raw:
        work = Path(raw)

        ok = run_case(sign_release, work, "ok", "ok")
        if "error" in ok:
            fail(f"a valid signing run failed: {ok['error']}")
        signed, unsigned, digests = ok["result"]
        if signed != 3 or unsigned != ["Presigned.apk"]:
            fail(f"unexpected signing summary: {signed} {unsigned}")
        if sorted(digests) != ["media", "platform", "releasekey", "shared"]:
            fail("certificate digests are incomplete")
        if len(set(digests.values())) != 1 or len(next(iter(digests.values()))) != 64:
            fail("certificate digest is not a SHA-256")
        if entries(ok["scratch"]):
            fail("the private work directory survived a successful signing")
        arguments = Path(ok["record"]).read_text().splitlines()
        if "-o" not in arguments:
            fail("signing did not replace the OTA keys")
        # The signer runs in a private work directory under the scratch parent, where keys/ links the key
        # directory (no key is copied), and is given that relative name as its default key directory.
        signer_cwd = Path(Path(ok["record"]).with_name("record.txt.cwd").read_text())
        if arguments[arguments.index("-d") + 1] != "keys":
            fail("the default key directory is not the relative name of the key link")
        if signer_cwd.parent.resolve() != ok["scratch"].resolve() or str(ok["keys"]) in " ".join(arguments):
            fail("signing did not run in a private work directory")
        if Path(Path(ok["record"]).with_name("record.txt.keys").read_text()) != ok["keys"].resolve():
            fail("keys/ does not link the key directory")
        copy = f"{ok['scratch']}/{signer_cwd.name}/keys"
        expected = {
            f"com.android.adbd.apex={copy}/com.android.adbd",
            f"com.android.adbd.apex={copy}/com.android.adbd.pem",
            f"com.android.tethering.apex={copy}/com.android.tethering",
            f"com.android.tethering.apex={copy}/com.android.tethering.pem",
            f"WifiDialog.apk={copy}/releasekey",
        }
        if not expected <= set(arguments):
            fail(f"key routing is wrong: {sorted(expected - set(arguments))}")
        joined = " ".join(arguments)
        for forbidden in ("cts.certs", "CtsHelper", "Presigned", "cts.shim", "Settings.apk"):
            if forbidden in joined:
                fail(f"{forbidden} must not be routed to a release key")
        with zipfile.ZipFile(ok["signed_zip"]) as archive:
            if archive.read("IMAGES/userdata.img") != b"seed":
                fail("the data seed image was not carried into the signed archive")
        # The OTA certificate is named by the relative path the signer was given, not by a directory of the build host.
        first = ota_members(ok["signed_zip"])
        if first != {name: ["keys/releasekey.x509.pem"] for name in OTA_ZIPS}:
            fail(f"the otacerts.zip members are not the stable name: {first}")
        # The links to the source tree are gone and nothing of the tree went with them.
        for kept in (
            "build/make/target/product/security/testkey.x509.pem",
            "vendor/lineage/build/target/product/security/lineage.x509.pem",
        ):
            if not (ok["case"] / kept).is_file():
                fail(f"signing removed {kept} from the source tree")
        # A second signing in a work directory of another name gives the same members.
        again = run_case(sign_release, work, "again", "ok")
        if "error" in again or ota_members(again["signed_zip"]) != first:
            fail("a second signing did not give the same otacerts.zip members")
        second_cwd = Path(again["record"]).with_name("record.txt.cwd").read_text()
        if second_cwd == str(signer_cwd) or Path(second_cwd).name == signer_cwd.name:
            fail("the two signings did not run in work directories of different names")

        for label, case_args, text in (
            ("leak", {"mode": "leak"}, "outside the release key set"),
            ("abs-ota", {"mode": "abs-ota"}, "named them after a path of the build host"),
            ("collide", {"mode": "ok", "collide": True}, "the name of the key link"),
            ("fail", {"mode": "fail"}, "sign_target_files_apks failed"),
            (
                "ota-aosp", {"mode": "ok", "ota": "ota-aosp"},
                "RECOVERY/RAMDISK/system/etc/security/otacerts.zip still trusts a public",
            ),
            (
                "ota-lineage", {"mode": "ok", "ota": "ota-lineage"},
                "RECOVERY/RAMDISK/system/etc/security/otacerts.zip still trusts a public",
            ),
            (
                "ota-system-aosp", {"mode": "ok", "ota": "ota-system-aosp"},
                "SYSTEM/etc/security/otacerts.zip still trusts a public",
            ),
            (
                "ota-missing", {"mode": "ok", "ota": "ota-missing"},
                "RECOVERY/RAMDISK/system/etc/security/otacerts.zip omits the release",
            ),
            (
                "userdebug", {"mode": "ok", "build_variant": "userdebug"},
                "a userdebug build is not signed with the release keys",
            ),
            ("missing", {"mode": "ok", "skip": "com.android.adbd.pem"}, "lacks: com.android.adbd.pem"),
            ("loose", {"mode": "ok", "loose": True}, "accessible to group or others"),
            ("open-dir", {"mode": "ok", "key_mode": 0o755}, "must not be accessible"),
        ):
            outcome = run_case(sign_release, work, label, **case_args)
            if text not in str(outcome.get("error", "")):
                fail(f"{label}: expected a refusal containing {text!r}, got {outcome.get('error')!r}")
            if entries(outcome["scratch"]):
                fail(f"{label}: a private work directory survived the refusal")
            if label in ("loose", "open-dir", "missing", "collide", "userdebug") and Path(outcome["record"]).exists():
                fail(f"{label}: the signer ran despite the refusal")
        mode = stat.S_IMODE(os.stat(work / "ok" / "source-keys").st_mode)
        if mode != 0o700 or entries(work / "ok" / "source-keys") != entries(ok["keys"]):
            fail("the source key directory was modified")
        if not (work / "ok" / "source-keys" / "releasekey.pk8").is_file():
            fail("signing deleted a key of the source key directory")

    print("Android 17 release signing fixture passed")


if __name__ == "__main__":
    main()
