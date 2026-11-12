#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Derive a desktop image that carries its own setup from its source image.

Runs as root on an arm64 Linux host with erofs-utils: it checks the source
against images.json, extracts it with ownership, modes and extended
attributes, runs the image's setup script in a chroot of the extracted tree,
removes the source's machine ID, and writes a new EROFS image with the
source's compressor beside a receipt naming both images. README.md beside this script owns the contract.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DIRECTORY = Path(__file__).resolve().parent
DESCRIPTOR = DIRECTORY / "images.json"
SETUP_ENVIRONMENT = {
    "PATH": "/usr/sbin:/usr/bin:/sbin:/bin",
    "HOME": "/root",
    "LC_ALL": "C",
}


class DerivationError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def run(command: list[str], **keywords: object) -> subprocess.CompletedProcess:
    print("+ " + " ".join(command), flush=True)
    return subprocess.run(command, check=True, **keywords)


def superblock(image: Path) -> dict[str, str]:
    output = subprocess.run(
        ["dump.erofs", "-s", str(image)],
        check=True, capture_output=True, text=True,
    ).stdout
    fields = {}
    for line in output.splitlines():
        key, separator, value = line.partition(":")
        if separator:
            fields[key.strip()] = value.strip()
    return fields


def tool_version(tool: str) -> str:
    output = subprocess.run(
        [tool, "--version"], check=True, capture_output=True, text=True
    ).stdout
    return output.splitlines()[0].strip()


def check_driver(driver: Path, machine: str) -> None:
    """Refuse a driver that is not a shared object for the image's machine."""
    machines = {"aarch64": 183, "x86_64": 62}
    header = driver.read_bytes()[:20] if driver.is_file() else b""
    if (
        len(header) < 20
        or header[:4] != b"\x7fELF"
        or header[4] != 2  # ELFCLASS64
        or header[5] != 1  # little endian
        or int.from_bytes(header[16:18], "little") != 3  # ET_DYN
        or int.from_bytes(header[18:20], "little") != machines[machine]
    ):
        raise DerivationError(f"{driver} is not a {machine} shared object")


def install_driver(root: Path, entry: dict, driver: Path, gbm_module: Path) -> dict:
    """Install the VA-API driver, the gbm module and the Chromium file.

    The driver and the gbm backend module that lets libgbm allocate the planar
    buffers Chromium's encoder needs are built together from tools/video/va-v4l2
    on an arm64 Debian 13 host, against the libva and libdrm Debian 13 ships,
    which is what the image carries. Their recorded digests name the exact
    files the image holds, and the Chromium file selects the driver.
    """
    spec = entry["driver"]
    check_driver(driver, spec["machine"])
    check_driver(gbm_module, spec["machine"])
    configuration = (DIRECTORY / spec["configuration"]["from"]).resolve()
    for source, destination, mode in (
        (driver, spec["destination"], 0o644),
        (gbm_module, spec["gbm"]["destination"], 0o644),
        (configuration, spec["configuration"]["destination"], 0o644),
    ):
        target = root / destination
        if (root / Path(destination).parts[0]).is_symlink():
            raise DerivationError(f"the image's /{Path(destination).parts[0]} is a symbolic link")
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists() or target.is_symlink():
            raise DerivationError(f"the image already has /{destination}")
        shutil.copyfile(source, target)
        os.chmod(target, mode)
        if os.geteuid() == 0:
            os.chown(target, 0, 0)
    return {
        "destination": spec["destination"],
        "sha256": sha256(driver),
        "size": driver.stat().st_size,
        "gbm": {
            "destination": spec["gbm"]["destination"],
            "sha256": sha256(gbm_module),
            "size": gbm_module.stat().st_size,
        },
        "configuration": {
            "destination": spec["configuration"]["destination"],
            "sha256": sha256(configuration),
        },
    }


def reset_machine_id(root: Path) -> None:
    """Leave the extracted image at ``root`` without a machine ID.

    Every guest then generates its own at first boot and keeps it in its
    writable root. systemd fills an empty /etc/machine-id, while
    dbus-uuidgen --ensure, which OpenRC's D-Bus service runs, creates a
    missing file but rejects an empty one. D-Bus reads /var/lib/dbus/machine-id
    before /etc/machine-id, and systemd adopts a regular D-Bus copy, so that
    copy becomes a link to /etc/machine-id.
    """
    for directory in ("etc", "var", "var/lib", "var/lib/dbus"):
        if (root / directory).is_symlink():
            raise DerivationError(f"the image's /{directory} is a symbolic link")
    machine_id = root / "etc/machine-id"
    if machine_id.is_symlink() or (
        machine_id.exists() and not machine_id.is_file()
    ):
        raise DerivationError("the image's /etc/machine-id is not a file")
    machine_id.unlink(missing_ok=True)
    systemd = os.path.lexists(root / "usr/lib/systemd/systemd") or (
        not (root / "lib").is_symlink()
        and os.path.lexists(root / "lib/systemd/systemd")
    )
    if systemd:
        descriptor = os.open(
            machine_id, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o444
        )
        os.close(descriptor)
        os.chmod(machine_id, 0o444)
    dbus = root / "var/lib/dbus"
    if dbus.is_dir():
        copy = dbus / "machine-id"
        if copy.is_dir() and not copy.is_symlink():
            raise DerivationError(
                "the image's /var/lib/dbus/machine-id is a directory"
            )
        copy.unlink(missing_ok=True)
        copy.symlink_to("/etc/machine-id")


def derive(
    image: str, source: Path, output: Path, work: Path,
    driver: Path | None = None, gbm_module: Path | None = None,
) -> dict:
    descriptor = json.loads(DESCRIPTOR.read_text(encoding="utf-8"))
    try:
        entry = descriptor["images"][image]
    except KeyError as error:
        raise DerivationError(f"images.json names no image {image}") from error
    expected = entry["source"]
    if source.stat().st_size != expected["size"]:
        raise DerivationError(f"{source} is not {expected['file']}: size")
    source_digest = sha256(source)
    if source_digest != expected["sha256"]:
        raise DerivationError(f"{source} is not {expected['file']}: digest")
    if output.exists():
        raise DerivationError(f"refusing to replace {output}")
    receipt_path = output.with_name(output.name + ".json")
    if receipt_path.exists():
        raise DerivationError(f"refusing to replace {receipt_path}")
    setup = DIRECTORY / entry["setup"]
    if "driver" in entry and (driver is None or gbm_module is None):
        raise DerivationError(
            f"{image} carries the VA-API driver and its gbm module: pass --driver "
            "and --gbm-module"
        )
    if "driver" not in entry and (driver is not None or gbm_module is not None):
        raise DerivationError(f"{image} carries no VA-API driver")
    if driver is not None and gbm_module is not None:
        check_driver(driver, entry["driver"]["machine"])
        check_driver(gbm_module, entry["driver"]["machine"])

    stage = Path(tempfile.mkdtemp(prefix=f"desktop-{image}-", dir=work))
    root = stage / "root"
    mounted: list[Path] = []
    try:
        run([
            "fsck.erofs", f"--extract={root}", "--xattrs", "--preserve",
            str(source),
        ])
        for target, arguments in (
            (root / "proc", ["-t", "proc", "proc"]),
            (root / "dev", ["--bind", "/dev"]),
        ):
            if not target.is_dir() or target.is_symlink():
                raise DerivationError(f"the image has no {target.name} directory")
            run(["mount", *arguments, str(target)])
            mounted.append(target)
        installed = None
        if driver is not None and gbm_module is not None:
            installed = install_driver(root, entry, driver, gbm_module)
        with setup.open("rb") as script:
            run(
                ["chroot", str(root), "/bin/sh", "-s"],
                stdin=script, env=SETUP_ENVIRONMENT,
            )
        if installed is not None:
            # The driver and the module must resolve every library on the image
            # they join.
            for installed_file in (installed, installed["gbm"]):
                linked = subprocess.run(
                    ["chroot", str(root), "/usr/bin/ldd", "/" + installed_file["destination"]],
                    check=True, capture_output=True, text=True,
                    env=SETUP_ENVIRONMENT,
                ).stdout
                if "not found" in linked:
                    raise DerivationError(
                        f"/{installed_file['destination']} needs libraries the image lacks:\n" + linked
                    )
        while mounted:
            run(["umount", str(mounted.pop())])
        reset_machine_id(root)
        temporary = output.with_name(f".{output.name}.{os.getpid()}")
        run([
            "mkfs.erofs", f"-z{entry['compression']}", str(temporary),
            str(root),
        ])
        run(["fsck.erofs", str(temporary)])
        os.link(temporary, output)
        temporary.unlink()
    finally:
        while mounted:
            subprocess.run(["umount", str(mounted.pop())], check=False)
        shutil.rmtree(stage)

    source_fields = superblock(source)
    output_fields = superblock(output)
    receipt = {
        "schema_version": 1,
        "image": image,
        "source": {
            "file": expected["file"],
            "size": expected["size"],
            "sha256": source_digest,
            "inodes": int(source_fields["Filesystem inode count"]),
        },
        "setup": {
            "file": entry["setup"],
            "sha256": sha256(setup),
        },
        "output": {
            "file": output.name,
            "size": output.stat().st_size,
            "sha256": sha256(output),
            "inodes": int(output_fields["Filesystem inode count"]),
            "uuid": output_fields["Filesystem UUID"],
            "compression": entry["compression"],
        },
        **({"driver": installed} if installed is not None else {}),
        "tools": {
            "mkfs.erofs": tool_version("mkfs.erofs"),
            "host": f"{platform.system()} {platform.release()} {platform.machine()}",
        },
    }
    receipt_path.write_text(
        json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True,
                        help="an image named in images.json")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path,
                        help="the new image; never an existing file")
    parser.add_argument("--driver", type=Path,
                        help="the VA-API driver (uml_drv_video.so) an image "
                             "that names one carries")
    parser.add_argument("--gbm-module", type=Path,
                        help="the gbm backend module (virtio_gpu_gbm.so) built "
                             "with the driver")
    parser.add_argument("--work", type=Path,
                        help="where the tree is extracted (default: beside "
                             "the output)")
    options = parser.parse_args()
    if os.geteuid() != 0:
        print("derive.py: extraction and the chroot need root", file=sys.stderr)
        return 1
    if not re.fullmatch(r"[A-Za-z0-9._-]+\.erofs", options.output.name):
        print("derive.py: the output must be a plain *.erofs name that a "
              "kernel command line can carry", file=sys.stderr)
        return 1
    output = options.output.resolve()
    work = (options.work or output.parent).resolve()
    try:
        receipt = derive(
            options.image, options.source.resolve(), output, work,
            options.driver.resolve() if options.driver else None,
            options.gbm_module.resolve() if options.gbm_module else None,
        )
    except (DerivationError, subprocess.CalledProcessError, OSError) as error:
        print(f"derive.py: {error}", file=sys.stderr)
        return 1
    print(json.dumps(receipt["output"], indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
