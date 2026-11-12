#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare and verify the pure x86-64 Steam appliance inputs. The only network stage."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import urllib.request


HERE = Path(__file__).resolve().parent
HOST_TOOLS = ("debootstrap", "erofs-utils", "rsync", "curl", "ca-certificates",
              "python3", "xz-utils", "zstd", "unzip", "gdisk")


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def download(url: str, path: Path, expected: str | None = None, *, fetch: bool = True) -> dict:
    if path.is_symlink():
        raise RuntimeError(f"refusing symlink input: {path}")
    if path.is_file():
        digest = sha256(path)
        if expected and digest != expected:
            raise RuntimeError(f"cached payload checksum differs: {path}")
    else:
        if not fetch:
            raise RuntimeError(f"prepared payload is missing: {path}")
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(path.name + ".partial")
        print(f"Downloading {url}", flush=True)
        with urllib.request.urlopen(url, timeout=300) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        digest = sha256(temporary)
        if expected and digest != expected:
            raise RuntimeError(f"downloaded checksum differs: {path.name}")
        temporary.rename(path)
    return {"url": url, "file": str(path), "sha256": digest}


def client_packages(manifest: str) -> list[tuple[str, str]]:
    """Return the (file, sha256) pairs the pinned client manifest names."""
    packages = []
    for _, body in re.findall(r'"([^"\n]+)"\s*\{([^{}]+)\}', manifest):
        fields = dict(re.findall(r'"([^"\n]+)"\s*"([^"\n]+)"', body))
        if "file" not in fields:
            continue
        name = fields["file"]
        if Path(name).name != name or not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
            raise RuntimeError("unsafe Steam package name")
        packages.append((name, fields["sha2"]))
    if not packages:
        raise RuntimeError("the pinned Steam client manifest names no packages")
    return packages


def payload_specs(root: Path, lock: dict) -> list[tuple[str, Path, str]]:
    payloads = [(f"https://client-update.steamstatic.com/{name}",
                 root / "steam-packages" / name, digest)
                for name, digest in client_packages((HERE / "steam-client.manifest").read_text())]
    base, version = lock["steam_runtime_repository"], lock["steam_runtime_snapshot"]
    payloads.append((f"{base}/{version}/steam-runtime-steamrt-amd64.tar.xz",
                     root / "steam-runtime.tar.xz", lock["steam_runtime_sha256"]))
    return payloads


def validate_policy(lock: dict) -> str:
    manifest = HERE / "steam-client.manifest"
    if sha256(manifest) != lock["steam_manifest_sha256"]:
        raise RuntimeError("pinned Steam manifest checksum differs")
    text = manifest.read_text()
    if not text.startswith(f'"{lock["steam_platform"]}"'):
        raise RuntimeError("pinned Steam manifest is for another platform")
    version = re.search(r'"version"\s*"(\d+)"', text)
    if not version or version[1] != lock["steam_client_version"]:
        raise RuntimeError("pinned Steam manifest version differs from the lock")
    if not lock["steam_manifest_origin"].startswith("https://"):
        raise RuntimeError("the pinned manifest needs an HTTPS origin")
    policy = hashlib.sha256()
    for name in ("sources.json", "steam-client.manifest", "prepare-base.sh"):
        path = HERE / name
        policy.update(name.encode())
        policy.update(bytes.fromhex(sha256(path)))
    return policy.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--no-fetch", action="store_true")
    parser.add_argument("--validate-manifest-only", action="store_true")
    args = parser.parse_args()
    lock = json.loads((HERE / "sources.json").read_text())
    policy = validate_policy(lock)
    if args.validate_manifest_only:
        print("Steam x86-64 source manifest is valid")
        return
    root = args.root.resolve()
    receipt = root / "inputs.json"
    if args.verify or args.no_fetch:
        recorded = json.loads(receipt.read_text())
        if any(recorded.get(key) != value for key, value in lock.items()):
            raise RuntimeError("prepared input identities differ from the current lock")
        if recorded.get("policy_sha256") != policy:
            raise RuntimeError("prepared input policy differs; run preparation with the current lock")
        # The receipt must record the lock's digest for every locked payload;
        # the pass below then hashes each recorded payload once.
        recorded_digests = {entry["file"]: entry["sha256"] for entry in recorded["payloads"]}
        for _, path, digest in payload_specs(root, lock):
            if recorded_digests.get(str(path)) != digest:
                raise RuntimeError(f"prepared payload receipt differs from the lock: {path.name}")
        for name in ("base.tar.zst", "base.packages.tsv", "builder.tar.zst",
                     "builder.packages.tsv", "steam-client.manifest"):
            if str(root / name) not in recorded_digests:
                raise RuntimeError(f"prepared payload receipt is missing: {name}")
        for entry in recorded["payloads"]:
            path = Path(entry["file"])
            if not path.is_relative_to(root):
                raise RuntimeError("prepared payload is outside the input root")
            download(entry["url"], path, entry["sha256"], fetch=False)
        print(f"Verified Steam x86-64 build inputs: {root}")
        return
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        raise RuntimeError("Steam x86-64 preparation requires native x86-64 Linux")
    root.mkdir(parents=True, exist_ok=True)
    subprocess.run(["sudo", "-n", "apt-get", "update"], check=True)
    subprocess.run(["sudo", "-n", "apt-get", "install", "-y", "--no-install-recommends", *HOST_TOOLS],
                   check=True)
    recorded = dict(lock)
    recorded["policy_sha256"] = policy
    recorded["payloads"] = [download(url, path, digest) for url, path, digest in payload_specs(root, lock)]
    manifest = root / "steam-client.manifest"
    if not manifest.exists():
        shutil.copy2(HERE / "steam-client.manifest", manifest)
    recorded["payloads"].append(download(lock["steam_manifest_origin"], manifest,
                                         lock["steam_manifest_sha256"], fetch=False))
    subprocess.run(["bash", str(HERE / "prepare-base.sh"), str(root)], check=True)
    for name in ("base.tar.zst", "base.packages.tsv", "builder.tar.zst", "builder.packages.tsv"):
        recorded["payloads"].append(download("prepared:debian-trixie-amd64", root / name))
    receipt.write_text(json.dumps(recorded, indent=2) + "\n")
    print(f"Prepared Steam x86-64 build inputs: {root}")


if __name__ == "__main__":
    main()
