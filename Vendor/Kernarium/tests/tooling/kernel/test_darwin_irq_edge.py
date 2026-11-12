#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the Darwin IRQ backend each kernel ships, without booting a guest.

The backend is built from the series' own patches, in order, so a later topic
that changes it is tested too.
"""
__import__("sys").dont_write_bytecode = True

from pathlib import Path
import platform
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "kernel/common/tooling"))
import kernel_series  # noqa: E402

# The backend and its private header. Several topics change them.
FILES = ("arch/um/os-Darwin/irq.c", "arch/um/os-Darwin/irq.h")
LINES = (
    ROOT / "kernel/mainline-6.18/patches/metadata.env",
    ROOT / "kernel/mainline-6.18/slices/x86_64/patches/metadata.env",
)


def run(*args, cwd=None):
    subprocess.run(args, cwd=cwd, check=True, timeout=30)


def materialize(work, line):
    """Write the backend as the line's series leave it.

    The first topic adds both files whole and later topics edit irq.c, so every
    patch that names one of them is applied in series order, restricted to
    them.
    """
    include = [f"--include={name}" for name in FILES]
    for series in kernel_series.check(kernel_series.load_line(line)):
        for patch in series.patches:
            if any(name.encode() in patch.read_bytes() for name in FILES):
                run("git", "apply", *include, str(patch), cwd=work)


def main():
    if platform.system() != "Darwin":
        print("Darwin kqueue regression is only applicable on macOS")
        return
    fixtures = Path(__file__).with_name("irq-edge")
    for line in LINES:
        with tempfile.TemporaryDirectory(prefix="uml-irq-edge-") as temporary:
            work = Path(temporary)
            materialize(work, line)
            source = work / FILES[0]
            run("cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", f"-I{fixtures}", "-c", str(source), "-o", str(work / "irq.o"))
            run("c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", str(fixtures / "driver.cpp"), str(work / "irq.o"), "-o", str(work / "check"))
            run(str(work / "check"))


if __name__ == "__main__":
    main()
