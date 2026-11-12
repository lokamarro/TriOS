#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the x86 corpus producer's repository cases against the real runner.

The producer encodes a repository case's configuration for the unit runner,
and accepts a case only when a native run ends in its declared state. This
runs the encoding through the runner and feeds the native check responses
that agree and disagree with the declaration.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import pathlib
import struct
import subprocess
import sys
import tempfile

MOV_EAX_42_HLT = bytes.fromhex("b82a000000f4")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"test_x86_repository_cases.py: {message}")


def load_producer(path: pathlib.Path):
    specification = importlib.util.spec_from_file_location(
        "build_unit_corpus", path)
    require(specification is not None and specification.loader is not None,
            f"cannot load {path}")
    module = importlib.util.module_from_spec(specification)
    # Its dataclasses resolve their annotations through sys.modules.
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


def fixture(producer, rax: int):
    return producer.RepositoryCase(
        "cases/fixture.s", MOV_EAX_42_HLT, [],
        ((0x2000_0000, 0x1000),), (("RAX", (rax,)),))


def run_case(runner: pathlib.Path, scratch: pathlib.Path, code: bytes,
             config: bytes) -> tuple[int, str]:
    binary = scratch / "case.bin"
    configuration = scratch / "case.config.bin"
    binary.write_bytes(code)
    configuration.write_bytes(config)
    completed = subprocess.run(
        [str(runner), "--repository", str(binary), str(configuration)],
        check=False, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, timeout=60)
    return completed.returncode, completed.stdout


def response(producer, request: bytes, name: bytes, rip_offset: int,
             rax: int) -> bytes:
    state = bytearray(producer.NATIVE_STATE_SIZE)
    struct.pack_into("<IIQ", state, 0, 0, rip_offset, rax)
    return (producer.CASE_RESPONSE_MAGIC +
            struct.pack("<II", 1, 1) + b"GenuineIntel" +
            struct.pack("<I", 0xc06f2) + request[16:48] +
            struct.pack("<I", len(name)) + name + bytes(state))


def check_native_authority(producer, scratch: pathlib.Path) -> None:
    case = fixture(producer, 42)
    request = producer.repository_request([case])
    path = scratch / "response.bin"
    path.write_bytes(response(producer, request, b"cases/fixture.s", 5, 42))
    identity = producer.verify_repository_cases(path, request, [case])
    require(identity["vendor"] == "GenuineIntel" and
            identity["cpuid_signature"] == "0x000c06f2",
            f"native identity was not recorded: {identity}")
    for offset, rax, message in (
        (5, 41, "a declared value native execution did not produce"),
        (0, 42, "a native run that did not stop at HLT"),
    ):
        path.write_bytes(response(
            producer, request, b"cases/fixture.s", offset, rax))
        try:
            producer.verify_repository_cases(path, request, [case])
        except SystemExit:
            continue
        require(False, f"the producer accepted {message}")
    path.write_bytes(response(
        producer, request, b"cases/fixture.s", 5, 42)[:-1])
    try:
        producer.verify_repository_cases(path, request, [case])
    except SystemExit:
        return
    require(False, "the producer accepted a truncated response")


def check_vex_pseudo_prefix(producer) -> None:
    """LLVM prints {vex} before an instruction that also has an EVEX form."""
    mnemonic, form = producer.canonical_form(
        "{vex}\tvpdpbusd\t%ymm2, %ymm1, %ymm4")
    require((mnemonic, form) == ("vpdpbusd", "vpdpbusd:ymm,ymm,ymm"),
            f"the {{vex}} pseudo-prefix became part of a form: {form}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=pathlib.Path)
    parser.add_argument("--producer", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    producer = load_producer(arguments.producer)
    with tempfile.TemporaryDirectory(prefix="x86-repository-") as directory:
        scratch = pathlib.Path(directory)
        status, output = run_case(
            arguments.runner, scratch, MOV_EAX_42_HLT,
            producer.case_config(fixture(producer, 42)))
        require(status == 0 and output.startswith("status=passed\t"),
                f"a matching repository case failed: {output}")
        status, output = run_case(
            arguments.runner, scratch, MOV_EAX_42_HLT,
            producer.case_config(fixture(producer, 43)))
        require(status == 1 and "RAX expected=0x2b actual=0x2a" in output,
                f"a mismatching repository case was not reported: {output}")
        check_native_authority(producer, scratch)
    check_vex_pseudo_prefix(producer)
    print("repository cases reach the runner as declared and need a native "
          "run that ends in their declared state")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
