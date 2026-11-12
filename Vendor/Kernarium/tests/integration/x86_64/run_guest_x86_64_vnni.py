#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that an x86-64 guest takes the AVX-VNNI path of a CPUID-dispatching
program on the engine under test.

Cross-builds the freestanding vnni_probe.cpp, boots an x86-64 image on an
emulator built with the macos-x86_64 preset, and runs the probe. The probe
chooses between an AVX2 and an AVX-VNNI form of three integer dot-product
kernels the way ggml, MLAS and oneDNN do: it reads CPUID leaf 7 for the highest
subleaf and subleaf 1 EAX bit 4. The guest must report AVX-VNNI, the probe
must take the VNNI path, and both forms of every kernel must give the exact
integer results. The AVX2 form is VPMADDUBSW, VPMADDWD and VPADDD, so the
agreement between the two forms checks VPDPBUSD against them on real data.
The kernel describes the CPU in /proc/cpuinfo from a table of its own, which
must list avx_vnni as CPUID does.
"""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import pathlib
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
# python3 -I leaves the script's own directory off sys.path.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from guest import Guest  # noqa: E402
from probe_runner import (  # noqa: E402
    GUEST_DIRECTORY, X86_64, build_probes, parse_arguments)


PROBE = pathlib.Path(__file__).with_name("vnni_probe.cpp")
PROBES = {"vnni-probe": ("x86-64", X86_64)}
PROBE_TIMEOUT_SECONDS = 120
# What the probe prints when CPUID reports AVX-VNNI and its dispatcher takes
# the VNNI form.
EXPECTED = (
    "vnni-probe cpuid max-subleaf=1 avx2=1 avx-vnni=1",
    "vnni-probe dispatch path=vnni",
    "vnni-probe ok",
)


def main() -> int:
    arguments = parse_arguments(
        __doc__, compiler_help="clang driver with an x86-64 Linux target and ld.lld")

    payloads = build_probes(arguments.compiler, PROBE, PROBES)
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "512M",
        "tmpfs",
        kernel_arguments=("ncpus=10",),
        init_shell=True,
        engine=arguments.engine,
        payloads=payloads,
    ) as guest:
        guest.boot_to_shell(arguments.timeout)
        result = guest.run_command(
            f"{GUEST_DIRECTORY}/vnni-probe check vnni", PROBE_TIMEOUT_SECONDS
        )
        cpuinfo = guest.run_command("grep -m1 '^flags' /proc/cpuinfo", 30)
    print(result.output.strip())
    print(cpuinfo.output.strip())
    if "avx_vnni" not in cpuinfo.output.split(":", 1)[-1].split():
        raise RuntimeError("/proc/cpuinfo does not list avx_vnni")
    if result.return_code != 0:
        raise RuntimeError(f"the probe reported {result.return_code} failure(s)")
    lines = result.output.splitlines()
    for line in EXPECTED:
        if not any(found.strip().endswith(line) for found in lines):
            raise RuntimeError(f"the probe did not print {line!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
