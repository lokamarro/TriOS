#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Drive the native SME oracle with base A64 cases on this host.

tools/arm64-tests/sme_native_oracle.cpp records the SME corpus expectations
on an SME host, which this host need not be. Its exception path, register
transfer, and region handling are the same for every case, so this test
runs a request without SME state through them: a completed case, an
undefined instruction, a breakpoint, read and write faults with their
addresses, NZCV, SP, and SIMD&FP results, an initial FPSR, a store's
region runs, and a trapped floating-point exception, which XNU reports as a
bad instruction with the syndrome's EC 0x2c. On an SME host it also runs streaming cases, whose FPSR XNU
resets on every kernel entry: the FPSR an instruction leaves must reach the
response after a completed case, a fault and an undefined instruction, the
initial FPSR must reach the program, and SMSTART's own reset must stay.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
import pathlib
import struct
import subprocess
import sys
import tempfile

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
GENERATOR = REPOSITORY / "tools" / "arm64-tests" / "sme-native-corpus.py"

FADD_SCALAR = 0x1E21_2802  # FADD S2, S0, S1
FADD_VECTOR = 0x4E22_D420  # FADD V0.4S, V1.4S, V2.4S
FDIV_SCALAR = 0x1E22_1820  # FDIV S0, S1, S2
DZE = 0x200
ONE = struct.pack("<I", 0x3F80_0000)  # 1.0
TINY = struct.pack("<I", 0x3080_0000)  # 2^-30: 1.0 + TINY is inexact
DZC, IXC = 0x02, 0x10
STREAMING_RESET = 0x0800_009F


def run_oracle(oracle: str, native, cases: list, svl: int):
    with tempfile.TemporaryDirectory(prefix="sme-oracle-") as scratch:
        request = pathlib.Path(scratch) / "request"
        response = pathlib.Path(scratch) / "response"
        request.write_bytes(native.write_request(cases, svl))
        subprocess.run([oracle, str(request), str(response)], check=True,
                       timeout=60)
        return native.read_response(response.read_bytes(), cases, svl)


def streaming_failures(oracle: str, native, svl: int) -> list[str]:
    """Streaming cases: (case, event, pc offset, fault address, FPSR, SVCR)."""
    State, Region, Case = native.WireState, native.WireRegion, native.WireCase
    svc, data, page = native.SVC, native.DATA, native.PAGE
    rw = native.REGION_READABLE | native.REGION_WRITABLE
    operands = {0: ONE + bytes(svl - 4), 1: TINY + bytes(svl - 4)}

    def streaming(**fields) -> State:
        x = fields.pop("x", {})
        return State(x={18: 0, **x}, svcr=native.SM, fpsr=DZC, z=operands,
                     **fields)

    checks = [
        (Case((FADD_SCALAR, svc), 0, streaming()),
         "system-call", 4, 0, DZC | IXC, native.SM),
        (Case((FADD_SCALAR, 0xF940_0023, svc), 0,  # LDR X3, [X1]
              streaming(x={1: data + page}),
              (Region(data, page, rw, seed=1),)),
         "memory-fault", 4, data + page, DZC | IXC, native.SM),
        (Case((FADD_SCALAR, FADD_VECTOR, svc), 0, streaming()),
         "illegal-instruction", 4, 0, DZC | IXC, native.SM),
        (Case((svc,), 0, streaming()), "system-call", 0, 0, DZC, native.SM),
        (Case((0xD503_437F, svc), 0,  # SMSTART SM
              State(x={18: 0}, fpsr=DZC)),
         "system-call", 4, 0, STREAMING_RESET, native.SM),
    ]
    _, results = run_oracle(oracle, native, [check[0] for check in checks],
                            svl)
    failures = []
    for index, ((_, event, pc, fault, fpsr, svcr), result) in enumerate(
            zip(checks, results)):
        got = (result.event, result.pc_offset, result.fault_address,
               result.state.fpsr, result.state.svcr)
        want = (event, pc, fault, fpsr, svcr)
        if got != want:
            failures.append(f"streaming case {index}: got {got}, "
                            f"expected {want}")
    return failures


def load_generator():
    spec = importlib.util.spec_from_file_location("sme_native_corpus",
                                                  GENERATOR)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_sme_native_oracle.py ORACLE")
    oracle = sys.argv[1]
    native = load_generator()
    State, Region, Case = native.WireState, native.WireRegion, native.WireCase
    data, svc = native.DATA, native.SVC
    page = native.PAGE
    rw = native.REGION_READABLE | native.REGION_WRITABLE

    def state(**fields) -> State:
        x = fields.pop("x", {})
        return State(x={18: 0, **x}, **fields)

    # (case, event, pc offset, fault address, X changes, other checks)
    checks = [
        (Case((0x8B02_0020, svc), 0, state(x={1: 5, 2: 7})),  # ADD X0, X1, X2
         "system-call", 4, 0, {0: 12}),
        (Case((0x0000_0000, svc), 0, state()),  # UDF #0
         "illegal-instruction", 0, 0, {}),
        (Case((0xD503_201F, 0xD420_0020, svc), 0, state()),  # NOP; BRK #1
         "breakpoint", 4, 0, {}),
        (Case((0xF940_0020, svc), 0, state(x={1: data + page}),
              (Region(data, page, rw, seed=1),)),  # LDR X0, [X1]
         "memory-fault", 0, data + page, {}),
        (Case((0xF900_0020, svc), 0, state(x={1: data + 8}),
              (Region(data, page, native.REGION_READABLE, seed=1),)),
         "memory-fault", 0, data + 8, {}),  # STR X0, [X1], read-only
        (Case((0xF900_0020, svc), 0,
              state(x={0: 0x1122_3344_5566_7788, 1: data + 8}),
              (Region(data, page, rw, seed=1),)),
         "system-call", 4, 0, {}),
        (Case((0xAB02_0020, svc), 0,  # ADDS X0, X1, X2
              state(x={1: 0x7FFF_FFFF_FFFF_FFFF, 2: 1}, nzcv=0x2000_0000)),
         "system-call", 4, 0, {0: 0x8000_0000_0000_0000}),
        (Case((0x9100_03E0, svc), 0, state(x={31: data + 0x10})),
         "system-call", 4, 0, {0: data + 0x10}),  # ADD X0, SP, #0
        (Case((0x4E22_D420, svc), 0, state()),  # FADD V0.4S, V1.4S, V2.4S
         "system-call", 4, 0, {}),
        (Case((0xD503_201F, svc, svc), 4, state()),  # entry past a NOP
         "system-call", 4, 0, {}),
        (Case((FADD_SCALAR, svc), 0,
              state(fpsr=DZC, v={0: ONE + bytes(12), 1: TINY + bytes(12)})),
         "system-call", 4, 0, {}),
        (Case((FDIV_SCALAR, svc), 0,
              state(fpcr=DZE, fpsr=IXC,
                    v={1: ONE + bytes(12), 2: bytes(16)})),
         "floating-point-trap", 0, 0, {}),
    ]
    cases = [check[0] for check in checks]
    host, results = run_oracle(oracle, native, cases, 0)
    failures = []
    for index, ((case, event, pc, fault, x), result) in enumerate(
            zip(checks, results)):
        got = (result.event, result.pc_offset, result.fault_address,
               {key: value for key, value in result.state.x.items()})
        want = (event, pc, fault, x)
        if got != want:
            failures.append(f"case {index}: got {got}, expected {want}")
    if results[6].state.nzcv != 0x9000_0000:
        failures.append(f"ADDS: NZCV {results[6].state.nzcv:#x}")
    if results[4].esr >> 6 & 1 != 1 or results[3].esr >> 6 & 1 != 0:
        failures.append("fault syndromes do not give the access")
    if results[5].runs != (((8, bytes.fromhex("8877665544332211")),),):
        failures.append(f"STR: runs {results[5].runs}")
    if set(results[8].state.v) != {0} or results[8].state.fpsr == 0:
        failures.append("FADD: V0 and FPSR.IXC were not reported")
    if results[10].state.fpsr != DZC | IXC:
        failures.append(
            f"FADD from FPSR.DZC: FPSR {results[10].state.fpsr:#x}")
    if results[11].esr != 0xB280_0002 or results[11].state.v or \
            results[11].state.fpsr != IXC:
        failures.append(f"FDIV by zero with DZE: ESR {results[11].esr:#x}, "
                        f"FPSR {results[11].state.fpsr:#x}")
    if host["svl"]:
        failures += streaming_failures(oracle, native, host["svl"])
    if failures:
        raise SystemExit("sme_native_oracle: " + "; ".join(failures))
    streaming = " and 5 streaming cases" if host["svl"] else ""
    print(f"sme_native_oracle: {len(cases)} base A64 cases{streaming} on a "
          f"host with SVL {host['svl']} and "
          f"{host['features'] or 'no SME features'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
