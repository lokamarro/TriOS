#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Record the actual producer sources and the installed Steam client identity."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
from pathlib import Path


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputs", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    repository = here.parents[2]
    recorded = json.loads((args.inputs / "inputs.json").read_text())
    sources = sorted(path for path in here.rglob("*")
                     if path.is_file() and path.name != "README.md")
    # build.sh and seal-image.sh source the helpers every rootfs producer shares.
    sources.append(here.parent / "producer.sh")
    recorded["producer_sources"] = {path.relative_to(repository).as_posix(): digest(path)
                                    for path in sources}
    # The maintained Vulkan guest publishes the installed runtime's identity
    # here, and the image carries no other record of which one it ships.
    graphics = args.root / "usr/share/uml-vulkan/guest-runtime.json"
    if graphics.is_file():
        recorded["vulkan_runtime"] = json.loads(graphics.read_text())
    steam = args.root / "usr/share/uml-steam/home/uml/.local/share/Steam"
    recorded["steam_seed"] = {
        "client": digest(steam / "steamrt64/steam"),
        "webhelper": digest(steam / "steamrt64/steamwebhelper"),
        "installed_manifest": digest(
            steam / "package" / f"steam_client_{recorded['steam_channel']}_{recorded['steam_platform']}.manifest"),
    }
    args.output.write_text(json.dumps(recorded, indent=2) + "\n")


if __name__ == "__main__":
    main()
