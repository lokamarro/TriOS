#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot one Darwin UML engine and validate its lightweight guest contract."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import base64
import ctypes
import os
import pathlib
import re
import shlex
import subprocess
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402


GUEST_HWCAP_SANITY = "/usr/local/libexec/uml-test/hwcap-sanity"
GUEST_CPU_IDENTITY = "/usr/local/libexec/uml-test/cpu-identity.py"
HWCAP_FP = 1 << 0
HWCAP_ASIMD = 1 << 1
HWCAP_EVTSTRM = 1 << 2
HWCAP_AES = 1 << 3
HWCAP_PMULL = 1 << 4
HWCAP_SHA1 = 1 << 5
HWCAP_SHA2 = 1 << 6
HWCAP_CRC32 = 1 << 7
HWCAP_ATOMICS = 1 << 8
HWCAP_FPHP = 1 << 9
HWCAP_ASIMDHP = 1 << 10
HWCAP_CPUID = 1 << 11
HWCAP_ASIMDRDM = 1 << 12
HWCAP_JSCVT = 1 << 13
HWCAP_FCMA = 1 << 14
HWCAP_LRCPC = 1 << 15
HWCAP_SHA3 = 1 << 17
HWCAP_ASIMDDP = 1 << 20
HWCAP_SHA512 = 1 << 21
HWCAP_ASIMDFHM = 1 << 23
HWCAP_DIT = 1 << 24
HWCAP_USCAT = 1 << 25
HWCAP_ILRCPC = 1 << 26
HWCAP_FLAGM = 1 << 27
HWCAP_SB = 1 << 29
HWCAP2_FLAGM2 = 1 << 7
HWCAP2_FRINT = 1 << 8
HWCAP2_I8MM = 1 << 13
HWCAP2_BF16 = 1 << 14
HWCAP2_ECV = 1 << 19
HWCAP2_AFP = 1 << 20
HWCAP2_RPRES = 1 << 21
HWCAP2_SME = 1 << 23
HWCAP2_SME_I16I64 = 1 << 24
HWCAP2_SME_F64F64 = 1 << 25
HWCAP2_SME_I8I32 = 1 << 26
HWCAP2_SME_F16F32 = 1 << 27
HWCAP2_SME_B16F32 = 1 << 28
HWCAP2_SME_F32F32 = 1 << 29
HWCAP2_CSSC = 1 << 34
HWCAP2_SME2 = 1 << 37
HWCAP2_SME_I16I32 = 1 << 39
HWCAP2_SME_BI32I32 = 1 << 40
HWCAP2_MOPS = 1 << 43
OPTIONAL_HWCAPS = (
    ("hw.optional.arm.FEAT_AES", HWCAP_AES, "aes"),
    ("hw.optional.arm.FEAT_PMULL", HWCAP_PMULL, "pmull"),
    ("hw.optional.arm.FEAT_SHA1", HWCAP_SHA1, "sha1"),
    ("hw.optional.arm.FEAT_SHA256", HWCAP_SHA2, "sha2"),
    ("hw.optional.arm.FEAT_CRC32", HWCAP_CRC32, "crc32"),
    ("hw.optional.arm.FEAT_LSE", HWCAP_ATOMICS, "atomics"),
    ("hw.optional.arm.FEAT_FP16", HWCAP_FPHP, "fphp"),
    ("hw.optional.arm.FEAT_FP16", HWCAP_ASIMDHP, "asimdhp"),
    ("hw.optional.arm.FEAT_RDM", HWCAP_ASIMDRDM, "asimdrdm"),
    ("hw.optional.arm.FEAT_JSCVT", HWCAP_JSCVT, "jscvt"),
    ("hw.optional.arm.FEAT_FCMA", HWCAP_FCMA, "fcma"),
    ("hw.optional.arm.FEAT_LRCPC", HWCAP_LRCPC, "lrcpc"),
    ("hw.optional.arm.FEAT_SHA3", HWCAP_SHA3, "sha3"),
    ("hw.optional.arm.FEAT_DotProd", HWCAP_ASIMDDP, "asimddp"),
    ("hw.optional.arm.FEAT_SHA512", HWCAP_SHA512, "sha512"),
    ("hw.optional.arm.FEAT_FHM", HWCAP_ASIMDFHM, "asimdfhm"),
    ("hw.optional.arm.FEAT_DIT", HWCAP_DIT, "dit"),
    ("hw.optional.arm.FEAT_LSE2", HWCAP_USCAT, "uscat"),
    ("hw.optional.arm.FEAT_LRCPC2", HWCAP_ILRCPC, "ilrcpc"),
    ("hw.optional.arm.FEAT_FlagM", HWCAP_FLAGM, "flagm"),
    ("hw.optional.arm.FEAT_SB", HWCAP_SB, "sb"),
)
AFP_HOST_FEATURES = (
    "hw.optional.arm.FEAT_AFP",
    "hw.optional.arm.FEAT_FP16",
    "hw.optional.arm.FEAT_FHM",
    "hw.optional.arm.FEAT_BF16",
    "hw.optional.arm.FEAT_FRINTTS",
    "hw.optional.arm.FEAT_FCMA",
    "hw.optional.arm.FEAT_JSCVT",
)
OPTIONAL_HWCAPS2 = (
    ("hw.optional.arm.FEAT_FlagM2", HWCAP2_FLAGM2, "flagm2"),
    ("hw.optional.arm.FEAT_FRINTTS", HWCAP2_FRINT, "frint"),
    ("hw.optional.arm.FEAT_I8MM", HWCAP2_I8MM, "i8mm"),
    ("hw.optional.arm.FEAT_BF16", HWCAP2_BF16, "bf16"),
    ("hw.optional.arm.FEAT_ECV", HWCAP2_ECV, "ecv"),
    # implements_afp(): FEAT_AFP and every host feature whose instructions
    # would otherwise take a software model without FIZ, AH and NEP.
    (AFP_HOST_FEATURES, HWCAP2_AFP, "afp"),
    (AFP_HOST_FEATURES + ("hw.optional.arm.FEAT_RPRES",), HWCAP2_RPRES,
     "rpres"),
)
# arm64 Linux 6.18's name for every AT_HWCAP and AT_HWCAP2 bit, indexed by
# bit: its hwcap_str table in arch/arm64/kernel/cpuinfo.c, which the guest
# kernel prints from. The Features line is checked against whatever the
# auxiliary vector holds, so an engine that advertises a further feature
# needs no change here; a bit arm64 Linux has no name for fails the smoke.
HWCAP_NAMES = (
    "fp", "asimd", "evtstrm", "aes", "pmull", "sha1", "sha2", "crc32",
    "atomics", "fphp", "asimdhp", "cpuid", "asimdrdm", "jscvt", "fcma",
    "lrcpc", "dcpop", "sha3", "sm3", "sm4", "asimddp", "sha512", "sve",
    "asimdfhm", "dit", "uscat", "ilrcpc", "flagm", "ssbs", "sb", "paca",
    "pacg", "gcs", "cmpbr", "fprcvt", "f8mm8", "f8mm4", "svef16mm",
    "sveeltperm", "sveaes2", "svebfscale", "sve2p2", "sme2p2",
    "smesbitperm", "smeaes", "smesfexpa", "smestmop", "smesmop4",
)
HWCAP2_NAMES = (
    "dcpodp", "sve2", "sveaes", "svepmull", "svebitperm", "svesha3",
    "svesm4", "flagm2", "frint", "svei8mm", "svef32mm", "svef64mm",
    "svebf16", "i8mm", "bf16", "dgh", "rng", "bti", "mte", "ecv", "afp",
    "rpres", "mte3", "sme", "smei16i64", "smef64f64", "smei8i32",
    "smef16f32", "smeb16f32", "smef32f32", "smefa64", "wfxt", "ebf16",
    "sveebf16", "cssc", "rprfm", "sve2p1", "sme2", "sme2p1", "smei16i32",
    "smebi32i32", "smeb16b16", "smef16f16", "mops", "hbc", "sveb16b16",
    "lrcpc3", "lse128", "fpmr", "lut", "faminmax", "f8cvt", "f8fma",
    "f8dp4", "f8dp2", "f8e4m3", "f8e5m2", "smelutv2", "smef8f16",
    "smef8f32", "smesf8fma", "smesf8dp4", "smesf8dp2", "poe",
)
# Advertised on every host.
REQUIRED_HWCAPS2 = HWCAP2_CSSC | HWCAP2_MOPS
# The virtual CPU's SME profile (darwin/cpu/arm64/sme_profile.hpp and the
# ID_AA64SMFR0_EL1 it reports): SME with its base widths, F64F64 and I16I64
# on a host that has those three, and SME2 with I16I32 and BI32I32 where
# the host also has SME2 and the running engine implements it (its
# sme_support is sme2). The host prober reports it only under the
# guest-SME policy, which DARWIN_UML_GUEST_SME=0 alone turns off, and with a
# valid streaming vector length.
GUEST_SME_POLICY_VARIABLE = "DARWIN_UML_GUEST_SME"
SME_HOST_FEATURES = (
    "hw.optional.arm.FEAT_SME",
    "hw.optional.arm.FEAT_SME_F64F64",
    "hw.optional.arm.FEAT_SME_I16I64",
)
SME_HWCAPS2 = (
    HWCAP2_SME | HWCAP2_SME_I16I64 | HWCAP2_SME_F64F64 | HWCAP2_SME_I8I32
    | HWCAP2_SME_F16F32 | HWCAP2_SME_B16F32 | HWCAP2_SME_F32F32
)
SME2_HWCAPS2 = HWCAP2_SME2 | HWCAP2_SME_I16I32 | HWCAP2_SME_BI32I32
# The lines of the block arm64 Linux's c_show() (arch/arm64/kernel/cpuinfo.c)
# prints for a 64-bit task, up to each value. It prints a model name only to
# a 32-bit task. A blank line ends every block.
CPUINFO_LINES = (
    "processor\t: ",
    "BogoMIPS\t: ",
    "Features\t: ",
    "CPU implementer\t: ",
    "CPU architecture: ",
    "CPU variant\t: ",
    "CPU part\t: ",
    "CPU revision\t: ",
)
# arm64 Linux's ARCH_TIMER_EVT_STREAM_PERIOD_US: HWCAP_EVTSTRM promises that
# WFE wakes at least this often without an SEV.
EVENT_STREAM_PERIOD_US = 100
WFE_ITERATIONS = 100000
# Reads MIDR_EL1 and CNTFRQ_EL0 with MRS, as any task may, times WFE with no
# SEV against the counter, and asks LLVM for the host features it derives from
# /proc/cpuinfo, as llvmpipe and -mcpu=native do.
CPU_IDENTITY_PROBE = f"""\
import ctypes, glob, mmap, re, tempfile
libc = ctypes.CDLL(None, use_errno=True)
libc.mmap.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, ctypes.c_long)
libc.mmap.restype = ctypes.c_void_p
def function(words):
    # Mapped from a file so the kernel keeps the instruction cache coherent.
    with tempfile.TemporaryFile(dir="/tmp") as code:
        code.write(bytes.fromhex(words))
        code.flush()
        address = libc.mmap(None, mmap.PAGESIZE,
                            mmap.PROT_READ | mmap.PROT_EXEC,
                            mmap.MAP_PRIVATE, code.fileno(), 0)
    if address in (None, ctypes.c_void_p(-1).value):
        raise OSError(ctypes.get_errno(), "mmap of the MRS code failed")
    return ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_uint64)(address)
# nop; mrs x0, midr_el1; ret
print(f"midr=0x{{function('1f2003d5000038d5c0035fd6')(0):x}}")
# nop; mrs x0, cntfrq_el0; ret
print(f"cntfrq={{function('1f2003d500e03bd5c0035fd6')(0)}}")
# mrs x1, cntvct_el0; 1: wfe; subs x0, x0, #1; b.ne 1b; mrs x2, cntvct_el0;
# sub x0, x2, x1; ret: the counter ticks x0 WFEs take, with no SEV anywhere.
wfe = function("41e03bd55f2003d5000400f1c1ffff5442e03bd5400001cbc0035fd6")
print(f"wfe_ticks={{wfe({WFE_ITERATIONS})}}")
libraries = glob.glob("/usr/lib/*-linux-gnu/libLLVM.so.*")
if not libraries:
    raise SystemExit("the rootfs has no libLLVM")
newest = max(libraries, key=lambda path: [
    int(part) for part in re.findall(r"[0-9]+", path.rsplit("/", 1)[1])])
features = ctypes.CDLL(newest).LLVMGetHostCPUFeatures
features.restype = ctypes.c_char_p
print(f"llvm={{features().decode()}}")
"""


def host_sysctl_int(name: str) -> int | None:
    """An integer sysctl's value, 32 or 64 bits wide (the zeroed
    little-endian buffer takes either), or None when it is missing."""
    sysctlbyname = ctypes.CDLL(None, use_errno=True).sysctlbyname
    sysctlbyname.argtypes = (
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.c_void_p,
        ctypes.c_size_t,
    )
    sysctlbyname.restype = ctypes.c_int
    value = ctypes.c_uint64()
    size = ctypes.c_size_t(ctypes.sizeof(value))
    status = sysctlbyname(
        name.encode(), ctypes.byref(value), ctypes.byref(size), None, 0
    )
    if status != 0 or size.value not in (4, 8):
        return None
    return value.value


def host_feature(name: str) -> bool:
    return bool(host_sysctl_int(name))


def engine_sme_support(tool: pathlib.Path | None, engine: str) -> str:
    """The engine's SME support level, which uml-sme-engine-support prints
    as "gadgets=<level> jit=<level>"."""
    if tool is None:
        raise RuntimeError(
            "the guest-SME policy is on and the host has FEAT_SME2: pass "
            "--engine-support-tool, whose level for the engine decides "
            "whether the guest sees SME2"
        )
    output = subprocess.run(
        [str(tool)], capture_output=True, text=True, check=True,
        stdin=subprocess.DEVNULL, timeout=30,
    ).stdout
    return dict(field.split("=", 1) for field in output.split())[engine]


def expected_sme_hwcap2(tool: pathlib.Path | None, engine: str) -> int:
    """The SME bits this run's guest sees: the host prober's rule
    (darwin/platform/apple/host_features.hpp) with sme_profile()'s, and
    SME2 only from an engine at sme2 (sme_host_for_support())."""
    if os.environ.get(GUEST_SME_POLICY_VARIABLE) == "0":
        return 0
    if not all(host_feature(name) for name in SME_HOST_FEATURES):
        return 0
    svl = host_sysctl_int("hw.optional.arm.sme_max_svl_b") or 0
    if not 16 <= svl <= 256 or svl & (svl - 1):
        return 0
    if (host_feature("hw.optional.arm.FEAT_SME2")
            and engine_sme_support(tool, engine) == "sme2"):
        return SME_HWCAPS2 | SME2_HWCAPS2
    return SME_HWCAPS2


def expected_hwcap(
    features: tuple[tuple[str | tuple[str, ...], int, str], ...],
    initial: int = 0,
) -> int:
    """The bits whose host sysctl, or every one of whose sysctls, is set."""
    result = initial
    for names, bit, _ in features:
        if isinstance(names, str):
            names = (names,)
        if all(host_feature(name) for name in names):
            result |= bit
    return result


def auxiliary_vector(guest: Guest) -> dict[int, int]:
    output = guest.check_command("od -An -v -t u8 /proc/self/auxv", 60)
    words = [int(value) for value in re.findall(r"\b[0-9]+\b", output)]
    if len(words) % 2:
        raise RuntimeError(f"malformed auxiliary vector output:\n{output}")
    return dict(zip(words[0::2], words[1::2]))


def advertised_names(hwcap: int, hwcap2: int) -> list[str]:
    """The names of the set bits, AT_HWCAP's before AT_HWCAP2's, each word
    from bit 0 up: arm64 Linux's hwcap_str order."""
    names = []
    for word, value, table in (
        ("AT_HWCAP", hwcap, HWCAP_NAMES),
        ("AT_HWCAP2", hwcap2, HWCAP2_NAMES),
    ):
        for bit in range(64):
            if value & (1 << bit):
                if bit >= len(table):
                    raise RuntimeError(
                        f"{word} bit {bit} has no name in arm64 Linux"
                    )
                names.append(table[bit])
    return names


def arm64_bogomips(counter_frequency: int, hz: int) -> str:
    """What arm64 Linux prints for a counter of this frequency: its
    architected timer sets lpj_fine to DIV_ROUND_CLOSEST(rate, HZ), which
    calibrate_delay() takes as loops_per_jiffy, and c_show() formats that."""
    loops_per_jiffy = (counter_frequency + hz // 2) // hz
    return (
        f"{loops_per_jiffy // (500000 // hz)}."
        f"{loops_per_jiffy // (5000 // hz) % 100:02d}"
    )


def cpuinfo_blocks(guest: Guest) -> list[dict[str, str]]:
    """Each processor's block of the file's exact bytes, by line label."""
    encoded = guest.check_command("base64 -w0 /proc/cpuinfo", 60)
    text = base64.b64decode("".join(encoded.split())).decode()
    blocks = text.split("\n\n")
    if blocks[-1] != "":
        raise RuntimeError(
            "/proc/cpuinfo does not end in a blank line: "
            f"...{text[-160:]!r}"
        )
    parsed = []
    for block in blocks[:-1]:
        lines = block.split("\n")
        if len(lines) != len(CPUINFO_LINES) or not all(
            line.startswith(label) for line, label in zip(lines, CPUINFO_LINES)
        ):
            raise RuntimeError(
                f"/proc/cpuinfo block {block!r} does not have the lines "
                f"{CPUINFO_LINES}"
            )
        parsed.append(
            {
                label.partition("\t")[0].partition(":")[0]: line[len(label) :]
                for line, label in zip(lines, CPUINFO_LINES)
            }
        )
    return parsed


def check_cpuinfo(
    guest: Guest, hwcap: int, hwcap2: int, midr: int, counter_frequency: int
) -> None:
    """/proc/cpuinfo is the file arm64 Linux prints for the CPU a task sees:
    the BogoMIPS its counter frequency gives, the names of its AT_HWCAP and
    AT_HWCAP2 bits in hwcap_str order, and the MIDR_EL1 fields MRS
    returns."""
    online = int(guest.check_command("getconf _NPROCESSORS_ONLN", 60))
    hz = int(
        guest.check_command(
            "zcat /proc/config.gz | sed -n 's/^CONFIG_HZ=//p'", 60
        )
    )
    expected_bogomips = arm64_bogomips(counter_frequency, hz)
    blocks = cpuinfo_blocks(guest)
    if len(blocks) != online:
        raise RuntimeError(
            f"/proc/cpuinfo has {len(blocks)} blocks for {online} online CPUs"
        )
    expected_features = " ".join(advertised_names(hwcap, hwcap2))
    expected_identity = {
        "CPU implementer": f"0x{(midr >> 24) & 0xff:02x}",
        # arm64 Linux prints 8 whatever MIDR_EL1's architecture field holds.
        "CPU architecture": "8",
        "CPU variant": f"0x{(midr >> 20) & 0xf:x}",
        "CPU part": f"0x{(midr >> 4) & 0xfff:03x}",
        "CPU revision": f"{midr & 0xf}",
    }
    for number, values in enumerate(blocks):
        if values["processor"] != str(number):
            raise RuntimeError(
                f"/proc/cpuinfo block {number} names processor "
                f"{values['processor']}"
            )
        if values["BogoMIPS"] != expected_bogomips:
            raise RuntimeError(
                f"/proc/cpuinfo BogoMIPS is {values['BogoMIPS']!r}; arm64 "
                f"Linux prints {expected_bogomips!r} for a {counter_frequency} "
                f"Hz counter at HZ={hz}"
            )
        if values["Features"] != expected_features:
            raise RuntimeError(
                "/proc/cpuinfo Features mismatch: "
                f"expected {expected_features!r}, got {values['Features']!r}"
            )
        for key, expected in expected_identity.items():
            if values[key] != expected:
                raise RuntimeError(
                    f"/proc/cpuinfo {key} is {values[key]!r}; MIDR_EL1 "
                    f"0x{midr:x} gives {expected!r}"
                )
        if {**values, "processor": "0"} != blocks[0]:
            raise RuntimeError(
                f"/proc/cpuinfo block {number} differs from block 0 "
                "beyond its processor number"
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emulator", type=pathlib.Path, required=True)
    parser.add_argument("--rootfs", type=pathlib.Path, required=True)
    parser.add_argument("--memory", default="8G")
    parser.add_argument(
        "--engine", choices=("jit", "gadgets"), default="jit"
    )
    parser.add_argument(
        "--hwcap-sanity-binary", type=pathlib.Path, required=True
    )
    parser.add_argument("--engine-support-tool", type=pathlib.Path)
    arguments = parser.parse_args()
    hwcap_sanity = arguments.hwcap_sanity_binary.read_bytes()

    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        arguments.memory,
        "tmpfs",
        kernel_arguments=("ncpus=10",),
        init_shell=True,
        engine=arguments.engine,
        payloads={
            GUEST_HWCAP_SANITY: GuestFile(hwcap_sanity, 0o755),
            GUEST_CPU_IDENTITY: GuestFile(CPU_IDENTITY_PROBE.encode()),
        },
    ) as guest:
        guest.boot_to_shell(180)
        guest.check_command(
            "mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs /tmp", 60
        )

        auxv = auxiliary_vector(guest)
        actual_hwcap = auxv.get(16)
        actual_hwcap2 = auxv.get(26, 0)
        # HWCAP_CPUID is unconditional, exactly as arm64 Linux advertises it:
        # every engine emulates the EL0 identification-register reads.
        # HWCAP_EVTSTRM is too: every engine completes WFE at once, which the
        # WFE timing below checks.
        expected = expected_hwcap(
            OPTIONAL_HWCAPS,
            HWCAP_FP | HWCAP_ASIMD | HWCAP_EVTSTRM | HWCAP_CPUID,
        )
        if actual_hwcap != expected:
            raise RuntimeError(
                "AT_HWCAP mismatch: "
                f"expected 0x{expected:x}, got {actual_hwcap!r}"
            )
        expected2 = expected_hwcap(
            OPTIONAL_HWCAPS2,
            REQUIRED_HWCAPS2
            | expected_sme_hwcap2(
                arguments.engine_support_tool, arguments.engine
            ),
        )
        if actual_hwcap2 != expected2:
            raise RuntimeError(
                "AT_HWCAP2 mismatch: "
                f"expected 0x{expected2:x}, got 0x{actual_hwcap2:x}"
            )
        identity = guest.check_command(
            f"python3 -B {GUEST_CPU_IDENTITY}", 120
        )
        midr_match = re.search(r"^midr=0x([0-9a-f]+)$", identity, re.M)
        counter_match = re.search(r"^cntfrq=([0-9]+)$", identity, re.M)
        wfe_match = re.search(r"^wfe_ticks=([0-9]+)$", identity, re.M)
        llvm_match = re.search(r"^llvm=(.*)$", identity, re.M)
        if (
            midr_match is None
            or counter_match is None
            or wfe_match is None
            or llvm_match is None
        ):
            raise RuntimeError(f"CPU identity probe failed:\n{identity}")
        # AT_HWCAP's EVTSTRM promises that a WFE with no SEV wakes within the
        # event-stream period; one that waited for an event would never let
        # the probe finish.
        period_ticks = (
            int(counter_match.group(1)) * EVENT_STREAM_PERIOD_US // 1000000
        )
        wfe_ticks = int(wfe_match.group(1))
        if wfe_ticks >= WFE_ITERATIONS * period_ticks:
            raise RuntimeError(
                f"{WFE_ITERATIONS} WFEs took {wfe_ticks} counter ticks, more "
                f"than the {period_ticks}-tick event-stream period each"
            )
        check_cpuinfo(
            guest,
            actual_hwcap,
            actual_hwcap2,
            int(midr_match.group(1), 16),
            int(counter_match.group(1)),
        )
        # LLVM finds the host's features only in the Features line; with
        # none it reports no feature at all, not even the baseline NEON.
        llvm_features = llvm_match.group(1).strip().split(",")
        if "+neon" not in llvm_features:
            raise RuntimeError(
                f"LLVM sees no host NEON: {llvm_match.group(1)!r}"
            )

        # Every advertised family must also execute correctly. The payload
        # reads the auxiliary vector itself and skips what is not advertised.
        guest.check_command(GUEST_HWCAP_SANITY, 60, expected="OK")

        node = "/opt/node/bin/node"
        guest.check_command(f"test -x {shlex.quote(node)}", 60)

        dynamic_output = guest.check_command(
            f"{shlex.quote(node)} -e "
            + shlex.quote(
                'const f=new Function("x","return (x+1)|0");'
                "let x=0;for(let i=0;i<1000000;i++)x=f(x);"
                'if(x!==1000000)throw Error("bad JIT result");'
                'console.log("v8-jit-ok")'
            ),
            180,
        )
        if "v8-jit-ok" not in dynamic_output:
            raise RuntimeError(f"Node JIT smoke marker is missing:\n{dynamic_output}")

        wasm_output = guest.check_command(
            f"{shlex.quote(node)} -e "
            + shlex.quote(
                "const b=Buffer.from("
                '"0061736d010000000105016000017f030201000707010372756e0000'
                '0a06010400412a0b","hex");'
                "const m=new WebAssembly.Module(b);"
                "const v=new WebAssembly.Instance(m).exports.run();"
                'if(v!==42)throw Error("bad wasm result");'
                'console.log("wasm-jit-ok")'
            ),
            180,
        )
        if "wasm-jit-ok" not in wasm_output:
            raise RuntimeError(f"WebAssembly smoke marker is missing:\n{wasm_output}")

        for repetition in range(2):
            teardown_output = guest.check_command(
                f"{shlex.quote(node)} -p " + shlex.quote("6*7"), 60
            )
            if not re.search(r"(?:^|\n)42(?:\n|$)", teardown_output):
                raise RuntimeError(
                    "repeated Node teardown smoke failed "
                    f"on run {repetition + 1}:\n{teardown_output}"
                )

        print(
            f"PASS engine={arguments.engine} "
            f"AT_HWCAP=0x{actual_hwcap:x} AT_HWCAP2=0x{actual_hwcap2:x} "
            f"WFE={wfe_ticks / WFE_ITERATIONS:.3f} ticks"
        )
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
