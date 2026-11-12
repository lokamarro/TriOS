#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Mount persistent data without making the immutable root writable."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import os
from pathlib import Path
import shutil
import subprocess
import time

SEED = Path("/usr/share/uml-steam")
STATUS = Path("/run/kernarium-progress.status")
NEW_DISK = b"KERNARIUM NEW USER DATA v1\n"


def status(message: str, fraction: float | None = None) -> None:
    temporary = STATUS.with_suffix(".new")
    temporary.write_text(f"{fraction if fraction is not None else -1}\n{message.replace(chr(10), chr(32))}\n")
    temporary.replace(STATUS)


def run(*args: str, timeout: int = 120) -> None:
    subprocess.run(args, check=True, timeout=timeout)


def seed_tree(source: Path, destination: Path, caption: str) -> None:
    if destination.exists():
        return
    temporary = destination.with_name("." + destination.name + ".kernarium-seed")
    if temporary.exists():
        # Only this interrupted first-copy staging directory is disposable.
        shutil.rmtree(temporary)
    files = [p for p in source.rglob("*") if p.is_file() and not p.is_symlink()]
    total = sum(p.stat().st_size for p in files)
    copied = 0

    def copy(src: str, dst: str) -> str:
        nonlocal copied
        with open(src, "rb") as incoming, open(dst, "wb") as outgoing:
            while chunk := incoming.read(4 << 20):
                outgoing.write(chunk)
                copied += len(chunk)
                status(caption, copied / max(total, 1))
        shutil.copystat(src, dst)
        info = os.stat(src)
        os.chown(dst, info.st_uid, info.st_gid)
        return dst

    shutil.copytree(source, temporary, symlinks=True, copy_function=copy)
    status("Finishing initial setup")
    for base, directories, names in os.walk(source):
        for entry in [Path(base), *(Path(base) / name for name in directories + names)]:
            target = temporary / entry.relative_to(source)
            info = entry.lstat()
            os.chown(target, info.st_uid, info.st_gid, follow_symlinks=False)
    os.sync()
    temporary.rename(destination)
    os.sync()


def check_filesystem(device: Path) -> None:
    identified = subprocess.run(["blkid", "-p", "-s", "TYPE", "-o", "value", str(device)],
                                capture_output=True, text=True, timeout=15)
    # blkid may identify the enclosing GPT entry successfully while returning
    # no TYPE for an unformatted partition. Only our marker permits mkfs.
    if identified.returncode in (0, 2) and not identified.stdout.strip():
        with device.open("rb") as stream:
            stream.seek(1024)
            marker = stream.read(len(NEW_DISK))
        if marker != NEW_DISK:
            raise RuntimeError("User-data filesystem is unrecognized. The disk has been preserved for repair.")
        status("Creating your data partition")
        run("mkfs.ext4", "-q", "-b", "4096", "-m", "0", "-L", "kernarium-data",
            "-E", "lazy_itable_init=1,lazy_journal_init=1", str(device))
    elif identified.returncode != 0 or identified.stdout.strip() != "ext4":
        raise RuntimeError("User-data partition is not ext4. The disk has been preserved.")
    status("Checking your data")
    checked = subprocess.run(["e2fsck", "-p", str(device)], timeout=180)
    if checked.returncode not in (0, 1):
        raise RuntimeError("Your data needs filesystem repair. The disk has been preserved; it was not erased.")


def prepare() -> None:
    status("Preparing your storage")
    device = Path("/dev/ubdb1")
    deadline = time.monotonic() + 30
    while not device.exists():
        if time.monotonic() >= deadline:
            raise RuntimeError("User-data disk is missing. Attach the Kernarium data disk and restart.")
        time.sleep(0.1)
    check_filesystem(device)
    run("mount", "-t", "ext4", "-o", "noatime", str(device), "/userdata")
    data = Path("/userdata")
    seed_tree(SEED / "var", data / "var", "Preparing system data")
    (data / "home").mkdir(exist_ok=True)
    seed_tree(SEED / "home/uml", data / "home/uml", "Installing Steam • copying bundled client")
    run("mount", "--bind", str(data / "home"), "/home")
    run("mount", "--bind", str(data / "var"), "/var")
    (data / "root").mkdir(exist_ok=True, mode=0o700)
    run("mount", "--bind", str(data / "root"), "/root")
    machine_id = Path("/var/lib/dbus/machine-id")
    if not machine_id.exists() or not machine_id.read_text().strip():
        import uuid
        machine_id.parent.mkdir(parents=True, exist_ok=True)
        machine_id.write_text(uuid.uuid4().hex + "\n")
    status("Starting Steam")


def main() -> None:
    status("Starting Kernarium")
    Path("/run/kernarium-cache").mkdir(exist_ok=True)
    splash = subprocess.Popen(["/usr/local/libexec/kernarium-screen", "--framebuffer"],
                              env={**os.environ, "XDG_CACHE_HOME": "/run/kernarium-cache"})
    Path("/run/kernarium-splash.pid").write_text(str(splash.pid))
    try:
        prepare()
    except Exception as error:
        status(str(error))
        print(f"Kernarium boot failed: {error}", flush=True)
        # Keep the error visible while exposing recovery on the serial console.
        os.execv("/bin/sh", ["/bin/sh"])
    os.execv("/lib/systemd/systemd", ["/lib/systemd/systemd"])


if __name__ == "__main__":
    main()
