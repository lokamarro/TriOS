#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise authenticated rootfs provisioning with a small image fixture."""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest import mock


ROOTFS_TOOL_ROOT = Path(__file__).resolve().parents[3] / "tools/rootfs"
sys.path.insert(0, str(ROOTFS_TOOL_ROOT))

import provision  # noqa: E402
from verify import VerificationError, verify  # noqa: E402


def make_fixture(path: Path) -> dict[str, object]:
    image = bytes(range(256)) * 32
    path.write_bytes(image)
    return {
        "schema_version": 4,
        "image": "rootfs.erofs",
        "sha256": hashlib.sha256(image).hexdigest(),
        "size": len(image),
    }


def run(arguments: list[str]) -> int:
    with mock.patch.object(sys, "argv", ["provision.py", *arguments]):
        return provision.main()


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="uml-rootfs-provision-test-") as name:
        root = Path(name)
        source = root / "published-rootfs.erofs"
        manifest_value = make_fixture(source)
        manifest = root / "manifest.json"
        manifest.write_text(json.dumps(manifest_value), encoding="utf-8")
        verify(source, manifest)

        invalid = root / "invalid.erofs"
        invalid_bytes = bytearray(source.read_bytes())
        invalid_bytes[1024] ^= 0xFF
        invalid.write_bytes(invalid_bytes)
        try:
            verify(invalid, manifest)
        except VerificationError as error:
            if "SHA-256 does not match" not in str(error):
                raise
        else:
            raise RuntimeError("a changed image was accepted")

        output = root / "local" / "rootfs.erofs"
        if run(
            [
                "--source",
                str(source),
                "--output",
                str(output),
                "--manifest",
                str(manifest),
            ]
        ) != 0:
            raise RuntimeError("local provisioning failed")
        verify(output, manifest)
        if run(
            [
                "--source",
                str(source),
                "--output",
                str(output),
                "--manifest",
                str(manifest),
            ]
        ) == 0:
            raise RuntimeError("provisioning replaced an existing image")
        if run(
            [
                "--source",
                str(source),
                "--output",
                str(output),
                "--manifest",
                str(manifest),
                "--replace",
            ]
        ) != 0:
            raise RuntimeError("explicit rootfs replacement failed")
        verify(output, manifest)

        raced_output = root / "raced" / "rootfs.erofs"
        raced_output.parent.mkdir()
        real_verify = provision.verify

        def publish_during_verification(image: Path, manifest_path: Path):
            result = real_verify(image, manifest_path)
            raced_output.write_bytes(b"concurrent publisher")
            return result

        with mock.patch.object(
            provision, "verify", side_effect=publish_during_verification
        ):
            status = run(
                [
                    "--source",
                    str(source),
                    "--output",
                    str(raced_output),
                    "--manifest",
                    str(manifest),
                ]
            )
        if status == 0 or raced_output.read_bytes() != b"concurrent publisher":
            raise RuntimeError("no-replace provisioning lost a concurrent output")

        statement = root / "release-statement.json"
        statement.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "artifact": manifest_value["image"],
                    "sha256": manifest_value["sha256"],
                    "size": manifest_value["size"],
                }
            ),
            encoding="utf-8",
        )
        private_key = root / "release-private.pem"
        public_key = root / "release-public.pem"
        signature = root / "release-statement.sig"
        subprocess.run(
            [
                "openssl",
                "genpkey",
                "-algorithm",
                "RSA",
                "-pkeyopt",
                "rsa_keygen_bits:2048",
                "-out",
                private_key,
            ],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["openssl", "pkey", "-in", private_key, "-pubout", "-out", public_key],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            [
                "openssl",
                "dgst",
                "-sha256",
                "-sign",
                private_key,
                "-out",
                signature,
                statement,
            ],
            check=True,
            capture_output=True,
        )

        downloads = {
            "https://release.invalid/rootfs.erofs": source,
            "https://release.invalid/release-statement.json": statement,
            "https://release.invalid/release-statement.sig": signature,
        }

        def copy_fixture(url: str, destination: Path) -> None:
            shutil.copyfile(downloads[url], destination)

        release_output = root / "release" / "rootfs.erofs"
        with mock.patch.object(provision, "copy_from_url", side_effect=copy_fixture):
            status = run(
                [
                    "--url",
                    "https://release.invalid/rootfs.erofs",
                    "--statement-url",
                    "https://release.invalid/release-statement.json",
                    "--signature-url",
                    "https://release.invalid/release-statement.sig",
                    "--public-key",
                    str(public_key),
                    "--allow-network",
                    "--output",
                    str(release_output),
                    "--manifest",
                    str(manifest),
                ]
            )
        if status != 0:
            raise RuntimeError("authenticated release provisioning failed")
        verify(release_output, manifest)

        tampered = root / "tampered-statement.json"
        tampered.write_text(
            statement.read_text(encoding="utf-8") + " ", encoding="utf-8"
        )
        try:
            provision.verify_signature(tampered, signature, public_key)
        except VerificationError:
            pass
        else:
            raise RuntimeError("tampered release statement was accepted")

    print("rootfs provisioning tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
