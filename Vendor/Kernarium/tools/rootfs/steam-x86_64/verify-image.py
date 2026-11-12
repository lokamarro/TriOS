#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Audit the pure x86-64 Steam appliance: native execution and gaming files."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat


STEAM = "usr/share/uml-steam/home/uml/.local/share/Steam/"
# Valve ships 32-bit compatibility assets inside the client. They are retained
# only under these bundled paths; the system itself has no i386 multiarch and
# no 32-bit loader, so nothing outside them may be I386.
STEAM_BUNDLED_I386 = tuple(STEAM + name + "/" for name in (
    "linux32", "steamrt32", "steamrt64", "ubuntu12_32", "ubuntu12_64", "legacycompat",
    "steam-runtime-steamrt-amd64",
))
ELF_MACHINES = {3: "I386", 62: "AMD64", 183: "AArch64"}
REQUIRED_ELF = (
    "usr/local/libexec/kernarium-screen",
    "usr/bin/lsof",
    "usr/games/gamescope",
    "usr/games/gamescopereaper",
    STEAM + "steamrt64/steam",
    STEAM + "steamrt64/steamwebhelper",
)
REQUIRED_LIBRARIES = (
    "usr/lib/x86_64-linux-gnu/libcairo.so.2",
    "usr/lib/x86_64-linux-gnu/libX11.so.6",
    "usr/lib/x86_64-linux-gnu/libdrm.so.2",
    "usr/lib/x86_64-linux-gnu/libgtk-x11-2.0.so.0",
    "usr/lib/x86_64-linux-gnu/libopenal.so.1",
    "usr/lib/x86_64-linux-gnu/libnm.so.0",
    # The image's own VA-API backend over the Virtio video decoder.
    "usr/lib/x86_64-linux-gnu/dri/uml_drv_video.so",
)


def inventory(root: Path) -> list[tuple[str, str, str]]:
    entries = []
    observed = {}
    for directory, _, names in os.walk(root, followlinks=False):
        for name in names:
            path = Path(directory) / name
            if not stat.S_ISREG(path.lstat().st_mode):
                continue
            relative = path.relative_to(root).as_posix()
            if "\n" in relative or "\t" in relative:
                raise RuntimeError(f"binary path is not TSV-safe: {relative!r}")
            with path.open("rb") as stream:
                header = stream.read(20)
            if not header.startswith(b"\x7fELF"):
                continue
            machine = int.from_bytes(header[18:20], "little")
            if len(header) != 20 or header[5] != 1 or machine not in ELF_MACHINES:
                raise RuntimeError(f"invalid ELF payload: {relative}")
            if machine == 183:
                raise RuntimeError(f"unexpected foreign ELF payload: {relative}")
            if machine == 3 and not relative.startswith(STEAM_BUNDLED_I386):
                raise RuntimeError(f"unexpected system i386 ELF payload: {relative}")
            observed[relative] = ELF_MACHINES[machine]
            entries.append((relative, "ELF", ELF_MACHINES[machine]))
    for relative in REQUIRED_ELF:
        if observed.get(relative) != "AMD64":
            raise RuntimeError(f"required native Linux executable is absent or foreign: {relative}")
    for relative in REQUIRED_LIBRARIES:
        resolved = (root / relative).resolve()
        if not resolved.is_relative_to(root.resolve()) or observed.get(
                resolved.relative_to(root.resolve()).as_posix()) != "AMD64":
            raise RuntimeError(f"required native Linux library is absent or foreign: {relative}")
    launcher = root / "usr/local/bin/steam"
    if not launcher.is_file() or "steamrt64/steam" not in launcher.read_text():
        raise RuntimeError("the image launcher does not start the native steamrt client")
    if (root / "opt").exists() and any((root / "opt").iterdir()):
        raise RuntimeError("the pure x86-64 appliance bundles no compatibility tool")
    decoder_rule = root / "etc/udev/rules.d/70-uml-virtio-video.rules"
    if not decoder_rule.is_file() or 'SYMLINK+="video-dec%n"' not in decoder_rule.read_text():
        raise RuntimeError("V4L2 decoder discovery rule is absent or invalid")
    session = (root / "usr/local/bin/steam-gaming-session").read_text()
    if "LIBVA_DRIVER_NAME=uml" not in session or "KERNARIUM_CEF_VAAPI=1" not in session:
        raise RuntimeError("the session does not select the image's VA-API backend")
    if "-cef-enable-vaapi" not in (root / "usr/local/libexec/kernarium-steam.py").read_text():
        raise RuntimeError("the launcher never enables VA-API decode in the web helper")
    for executable in ("usr/bin/startxfce4", "usr/bin/gnome-session", "usr/bin/startplasma-wayland"):
        if (root / executable).exists():
            raise RuntimeError(f"desktop environment was installed: {executable}")
    if not list((root / "usr/share/vulkan/icd.d").glob("*.json")):
        raise RuntimeError("Mesa Vulkan ICDs are absent")
    graphics = root / "usr/share/uml-vulkan/guest-runtime.json"
    if graphics.is_file():
        recorded = json.loads(graphics.read_text())
        for record in recorded["installed_files"]:
            path = root / record["path"].lstrip("/")
            with path.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            if path.is_symlink() or digest != record["sha256"]:
                raise RuntimeError(f"installed Vulkan runtime differs: {path}")
        for relative, target in recorded["symlinks"].items():
            if os.readlink(root / relative.lstrip("/")) != target:
                raise RuntimeError(f"installed Vulkan link differs: {relative}")
        if sorted(path.name for path in (root / "usr/share/vulkan/icd.d").glob("*.json")) != ["virtio_icd.json"]:
            raise RuntimeError("Venus image contains an alternate Vulkan ICD")
        if recorded["architecture"]["debian"] != "amd64":
            raise RuntimeError("installed Vulkan runtime is for another architecture")
    return sorted(entries)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    entries = inventory(args.root.resolve(strict=True))
    args.output.write_text("path\tformat\tarchitecture\n" + "".join("\t".join(row) + "\n" for row in entries))
    print(f"Verified {len(entries)} Steam x86-64 binaries")


if __name__ == "__main__":
    main()
