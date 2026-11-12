#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that the host-side guest code sampler attributes guest time.

The workload spends nine tenths of its guest instructions in hot_loop and one
tenth in cold_loop. The emulator samples it with DARWIN_UML_GUEST_SAMPLES, and
the offline symbolizer places each sampled block in the workload's ELF by its
instruction words, so the split it reports must be close to that ratio.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import os
import pathlib
import sys
import tempfile


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402

SYMBOLIZER = REPOSITORY / "tests" / "perf" / "symbolize_guest_samples.py"
GUEST_WORKLOAD = "/usr/local/libexec/uml-test/workload"
SAMPLES_PER_SECOND = 1000
HOT_SHARE = 0.9
SHARE_TOLERANCE = 0.1
MINIMUM_ATTRIBUTED = 200
# The workload's TPIDR_EL0 (guest_sampler_workload.S).
THREAD_POINTER = 0x5A5A0000


def load_symbolizer():
    spec = importlib.util.spec_from_file_location(
        "symbolize_guest_samples", SYMBOLIZER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--workload", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    workload = arguments.workload.resolve()
    symbolizer = load_symbolizer()

    with tempfile.TemporaryDirectory(prefix="uml-guest-samples-") as directory:
        samples = pathlib.Path(directory) / "samples.tsv"
        os.environ["DARWIN_UML_GUEST_SAMPLES"] = f"{samples}:{SAMPLES_PER_SECOND}"
        with Guest(
            arguments.emulator.resolve(),
            arguments.rootfs.resolve(),
            "512M",
            "tmpfs",
            ("ncpus=10",),
            overlay_shell=True,
            # Only the gadgets engine carries the sampler.
            engine="gadgets",
            payloads={GUEST_WORKLOAD: GuestFile(workload.read_bytes(), 0o755)},
        ) as guest:
            guest.boot_and_login()
            output = guest.check_command(GUEST_WORKLOAD, 300)
            if "OK" not in output.splitlines():
                raise RuntimeError(f"workload did not finish:\n{output}")
            if not guest.shutdown(60):
                raise RuntimeError("guest did not power off")

        if not samples.is_file():
            raise RuntimeError("the emulator wrote no guest sample profile")
        profile = symbolizer.read_profile(samples)
        elf = symbolizer.read_elf(workload)
        located = symbolizer.locate(profile.blocks, [elf], 4)
        counts = {"hot_loop": 0, "cold_loop": 0}
        for index, (_, address) in located.items():
            name = symbolizer.symbol_for(elf, address).split("+")[0]
            if name in counts:
                counts[name] += profile.blocks[index].count
                thread = profile.blocks[index].thread
                if thread != THREAD_POINTER:
                    raise RuntimeError(
                        f"{name} was sampled under thread {thread:#x}, "
                        f"expected {THREAD_POINTER:#x}")
        attributed = counts["hot_loop"] + counts["cold_loop"]
        print(f"{profile.header}; hot_loop {counts['hot_loop']}, "
              f"cold_loop {counts['cold_loop']}")
        if attributed < MINIMUM_ATTRIBUTED:
            raise RuntimeError(
                f"only {attributed} samples were attributed to the workload")
        share = counts["hot_loop"] / attributed
        if abs(share - HOT_SHARE) > SHARE_TOLERANCE:
            raise RuntimeError(
                f"hot_loop received {share:.3f} of the workload's samples, "
                f"expected {HOT_SHARE} +/- {SHARE_TOLERANCE}")
        print(f"guest code sampler attributed {share:.3f} to hot_loop")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
