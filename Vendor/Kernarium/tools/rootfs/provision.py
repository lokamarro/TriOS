#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Provision the pinned EROFS rootfs from a local or authenticated artifact."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

from verify import DEFAULT_MANIFEST, ROOT, VerificationError, text, verify


def fail(message: str) -> int:
    print(f"rootfs provisioning failed: {message}", file=sys.stderr)
    return 1


def copy_from_url(url: str, destination: Path) -> None:
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != "https":
        raise VerificationError("release acquisition requires an https URL")
    request = urllib.request.Request(url, headers={"User-Agent": "uml-rootfs-provision/1"})
    with urllib.request.urlopen(request, timeout=120) as source, destination.open("wb") as output:
        final_url = urllib.parse.urlparse(source.geturl())
        if final_url.scheme != "https":
            raise VerificationError("release acquisition redirected outside HTTPS")
        shutil.copyfileobj(source, output, length=1024 * 1024)


def verify_signature(statement: Path, signature: Path, public_key: Path) -> None:
    command = [
        "openssl", "dgst", "-sha256", "-verify", str(public_key),
        "-signature", str(signature), str(statement),
    ]
    try:
        result = subprocess.run(command, check=False, capture_output=True, text=True)
    except FileNotFoundError as error:
        raise VerificationError("OpenSSL is required to verify a release signature") from error
    if result.returncode != 0:
        raise VerificationError(f"release signature verification failed: {result.stderr.strip()}")


def load_manifest(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise VerificationError(f"cannot read rootfs manifest {path}: {error}") from error
    if not isinstance(value, dict):
        raise VerificationError("rootfs manifest must be a JSON object")
    return value


def verify_release_statement(statement: Path, manifest: dict[str, object]) -> None:
    try:
        value = json.loads(statement.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise VerificationError(f"cannot read rootfs release statement: {error}") from error
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise VerificationError("rootfs release statement has an unsupported schema")
    expected = {
        "artifact": text(manifest.get("image"), "image"),
        "sha256": text(manifest.get("sha256"), "sha256"),
        "size": manifest.get("size"),
    }
    for field, expected_value in expected.items():
        if value.get(field) != expected_value:
            raise VerificationError(
                f"rootfs release statement {field} does not match the pinned manifest"
            )


def relative_to_manifest(value: str | None, manifest_path: Path) -> Path | None:
    if not value:
        return None
    path = Path(value)
    return path if path.is_absolute() else manifest_path.parent / path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "rootfs.erofs")
    parser.add_argument(
        "--source", type=Path, help="local artifact to authenticate and copy"
    )
    parser.add_argument("--url", help="authenticated HTTPS release artifact URL")
    parser.add_argument("--statement", type=Path, help="signed release digest statement")
    parser.add_argument("--statement-url", help="HTTPS URL of the release digest statement")
    parser.add_argument("--signature", type=Path, help="detached signature of --statement")
    parser.add_argument("--signature-url", help="HTTPS URL of the detached statement signature")
    parser.add_argument("--public-key", type=Path, help="trusted public key for the statement signature")
    parser.add_argument("--allow-network", action="store_true", help="allow the explicit HTTPS download")
    parser.add_argument(
        "--replace",
        action="store_true",
        help="allow replacement of an existing output image after verification",
    )
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    arguments = parser.parse_args()

    manifest_path = arguments.manifest.resolve()
    try:
        manifest = load_manifest(manifest_path)
    except VerificationError as error:
        return fail(str(error))
    source = arguments.source
    url = arguments.url or os.environ.get("UML_ROOTFS_URL")
    statement_url = (
        arguments.statement_url
        or os.environ.get("UML_ROOTFS_STATEMENT_URL")
    )
    signature_url = (
        arguments.signature_url
        or os.environ.get("UML_ROOTFS_SIGNATURE_URL")
    )
    statement = arguments.statement
    signature = arguments.signature
    public_key = arguments.public_key or relative_to_manifest(
        os.environ.get("UML_ROOTFS_PUBLIC_KEY"),
        manifest_path,
    )
    if bool(source) == bool(url):
        return fail("provide exactly one of --source or a configured release URL")
    output = arguments.output.resolve()
    if output.exists() and not arguments.replace:
        return fail(
            f"refusing to replace existing image {output}; pass --replace after "
            "preserving any required artifact"
        )
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(
            dir=output.parent, prefix=".rootfs-provision-"
        ) as temporary:
            temporary_path = Path(temporary)
            staged = temporary_path / "rootfs.erofs"
            if source:
                if not source.is_file():
                    raise VerificationError(f"local source is not a regular file: {source}")
                shutil.copyfile(source, staged)
            else:
                if not arguments.allow_network:
                    raise VerificationError(
                        "pass --allow-network before downloading a release artifact"
                    )
                if public_key is None or not public_key.is_file():
                    raise VerificationError(
                        "release acquisition requires a trusted public key"
                    )
                if bool(statement) == bool(statement_url):
                    raise VerificationError(
                        "provide exactly one local or HTTPS release statement"
                    )
                if bool(signature) == bool(signature_url):
                    raise VerificationError(
                        "provide exactly one local or HTTPS statement signature"
                    )
                staged_statement = temporary_path / "release-statement.json"
                staged_signature = temporary_path / "release-statement.sig"
                if statement:
                    shutil.copyfile(statement, staged_statement)
                else:
                    copy_from_url(statement_url, staged_statement)
                if signature:
                    shutil.copyfile(signature, staged_signature)
                else:
                    copy_from_url(signature_url, staged_signature)
                verify_signature(staged_statement, staged_signature, public_key)
                verify_release_statement(staged_statement, manifest)
                copy_from_url(url, staged)
            verify(staged, manifest_path)
            if arguments.replace:
                os.replace(staged, output)
            else:
                try:
                    os.link(staged, output)
                except FileExistsError as error:
                    raise VerificationError(
                        f"refusing to replace existing image {output}; pass "
                        "--replace after preserving any required artifact"
                    ) from error
    except (OSError, VerificationError) as error:
        return fail(str(error))
    print(f"provisioned and verified EROFS rootfs: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
