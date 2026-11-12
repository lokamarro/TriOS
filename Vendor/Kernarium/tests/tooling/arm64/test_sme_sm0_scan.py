#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the SM = 0 classifier of the SME image scan and idiom corpus.

tools/arm64-tests/sme_sm0.py walks the SME functions of an ELF file from
the entry mode it infers and classifies every word reachable with
PSTATE.SM = 0 against the SM=0 family, which includes the Z and P spills.
The fixtures are synthetic AArch64
ELF files built here from known words: a libyuv-style locally streaming
row function (only RDSVL and CNTD outside streaming mode), a
streaming-compatible body with an SVE word, a streaming function that
calls out, a callee-saved Z spill, glibc's ZA-disable shape, a streaming
ZA kernel, an indirect jump with no BTI J pads, a call to
__arm_sme_state, and UBSan-style prefix data that looks like a ZA store.
The scan tool runs on a tree holding the fixture, an APK with it and an
x86-64 ELF, with --expect; the corpus tool reports a relocatable fixture.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import pathlib
import struct
import subprocess
import sys
import tempfile
import zipfile

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
TOOLS = REPOSITORY / "tools" / "arm64-tests"
sys.path.insert(0, str(TOOLS))
import sme_sm0  # noqa: E402

RET = 0xD65F03C0
SMSTART_SM = 0xD503437F
SMSTOP_SM = 0xD503427F


def tbz_w16_bit0(words: int) -> int:
    return 0x36000010 | (words & 0x3FFF) << 5


def cbz_x14(words: int) -> int:
    return 0xB400000E | (words & 0x7FFFF) << 5


def bl(words: int) -> int:
    return 0x94000000 | (words & 0x3FFFFFF)


def adr_x8(words: int) -> int:
    return 0x10000008 | (words & 0x7FFFF) << 5


def functions() -> list[tuple[str, list]]:
    """(name, words) per fixture function; a callable word is resolved
    against the function starts once they are laid out."""
    return [
        ("ls_libyuv", [
            0xD503245F,     # bti c
            0x04BF5829,     # rdsvl x9, #1
            0xD343FD29,     # lsr x9, x9, #3
            0xF81B0FE9,     # str x9, [sp, #-0x50]!
            0x04E0E3E9,     # cntd x9
            0x6D013BEF,     # stp d15, d14, [sp, #0x10]
            SMSTART_SM,
            0x2518E3E0,     # ptrue p0.b
            0xA400A000,     # ld1b {z0.b}, p0/z, [x0]
            0xE400E020,     # st1b {z0.b}, p0, [x1]
            SMSTOP_SM,
            0x6D413BEF,     # ldp d15, d14, [sp, #0x10]
            0x910143FF,     # add sp, sp, #0x50
            RET]),
        ("sc_vector", [
            0xD53B4250,     # mrs x16, SVCR
            tbz_w16_bit0(2),
            SMSTOP_SM,
            0x04A10000,     # add z0.s, z0.s, z1.s
            tbz_w16_bit0(2),
            SMSTART_SM,
            RET]),
        ("st_calls_out", [
            0x2518E3E0,     # ptrue p0.b
            0xA400A000,     # ld1b {z0.b}, p0/z, [x0]
            SMSTOP_SM,
            lambda here, starts: bl((starts["ls_libyuv"] - here) // 4),
            SMSTART_SM,
            0xE400E020,     # st1b {z0.b}, p0, [x1]
            RET]),
        ("ls_zspill", [
            0x043F5FFF,     # addsvl sp, sp, #-1
            0xE58043E8,     # str z8, [sp]
            SMSTART_SM,
            SMSTOP_SM,
            0x858043E8,     # ldr z8, [sp]
            0x043F583F,     # addsvl sp, sp, #1
            RET]),
        ("za_disable", [
            0xD53BD0AE,     # mrs x14, TPIDR2_EL0
            cbz_x14(5),
            0xE1206200,     # str za[w15, 0], [x16]
            0x04305830,     # addsvl x16, x16, #1
            0xD51BD0BF,     # msr TPIDR2_EL0, xzr
            0xD503447F,     # smstop za
            RET]),
        ("st_za_mopa", [
            0x2598E3E0,     # ptrue p0.s
            0x80810000,     # fmopa za0.s, p0/m, p0/m, z0.s, z1.s
            RET]),
        (None, [
            0xC105CAFE,     # UBSan function-type prefix ...
            0xE0ED3E6E]),   # ... that decodes as st1d {za7h.d[w13, 0]}
        ("ns_jump", [
            adr_x8(2),
            0xD61F0100,     # br x8
            0x0420E3E0,     # cntb x0
            SMSTART_SM,
            SMSTOP_SM,
            RET]),
        ("__arm_sme_state", [
            0xD53B4240,     # mrs x0, SVCR
            RET]),
        ("sc_via_state", [
            lambda here, starts: bl((starts["__arm_sme_state"] - here) // 4),
            0x36000040,     # tbz w0, #0, +2
            SMSTOP_SM,
            0x04A0E3E1,     # cntw x1
            RET]),
    ]


def layout(base: int, selected: list[str] | None = None):
    """(code bytes, [(name, address, size)]) for the fixture functions."""
    chosen = [(name, words) for name, words in functions()
              if selected is None or name in selected]
    starts = {}
    address = base
    for name, words in chosen:
        if name is not None:
            starts[name] = address
        address += 4 * len(words)
    code = bytearray()
    symbols = []
    address = base
    for name, words in chosen:
        if name is not None:
            symbols.append((name, address, 4 * len(words)))
        for word in words:
            if callable(word):
                word = word(address, starts)
            code += struct.pack("<I", word)
            address += 4
    return bytes(code), symbols


def eh_frame(address: int, fdes: list[tuple[int, int]]) -> bytes:
    """A CIE ("zR", pc-relative sdata4 pointers) and one FDE per range."""
    cie_body = (struct.pack("<I", 0) + b"\x01zR\x00" + b"\x04" + b"\x78" +
                b"\x1e" + b"\x01" + b"\x1b")
    cie_body += b"\x00" * (-(len(cie_body) + 4) % 4)
    data = struct.pack("<I", len(cie_body)) + cie_body
    for start, size in fdes:
        record = len(data)
        pointer_field = address + record + 8
        body = struct.pack("<I", record + 4)
        body += struct.pack("<i", start - pointer_field)
        body += struct.pack("<i", size) + b"\x00"
        body += b"\x00" * (-(len(body) + 4) % 4)
        data += struct.pack("<I", len(body)) + body
    return data + struct.pack("<I", 0)


def elf(selected: list[str] | None = None, symbols: bool = True,
        fdes: list[str] | None = None, relocatable: bool = False,
        machine: int = 183) -> bytes:
    base = 0 if relocatable else 0x10000
    code, entries = layout(base, selected)
    sections = [(".text", 1, 0x6, base, code, 0, 0, 4, 0)]
    if fdes is not None:
        frame_address = base + len(code) + 0x100
        ranges = [(start, size) for name, start, size in entries
                  if name in fdes]
        sections.append((".eh_frame", 1, 0x2, frame_address,
                         eh_frame(frame_address, ranges), 0, 0, 8, 0))
    if symbols:
        strings = b"\0"
        table = bytes(24)
        for name, start, size in entries:
            offset = len(strings)
            strings += name.encode() + b"\0"
            value = start - base if relocatable else start
            table += struct.pack("<IBBHQQ", offset, 0x12, 0, 1, value, size)
        symtab_index = len(sections) + 1
        sections.append((".symtab", 2, 0, 0, table, symtab_index + 1, 1, 8,
                         24))
        sections.append((".strtab", 3, 0, 0, strings, 0, 0, 1, 0))
    names = b"\0"
    name_offsets = []
    for section in sections:
        name_offsets.append(len(names))
        names += section[0].encode() + b"\0"
    name_offsets.append(len(names))
    names += b".shstrtab\0"
    sections.append((".shstrtab", 3, 0, 0, names, 0, 0, 1, 0))
    body = bytearray(64)
    offsets = []
    for section in sections:
        while len(body) % 8:
            body.append(0)
        offsets.append(len(body))
        body += section[4]
    while len(body) % 8:
        body.append(0)
    shoff = len(body)
    body += bytes(64)
    for index, section in enumerate(sections):
        _name, kind, flags, addr, data, link, info, align, entsize = section
        body += struct.pack("<IIQQQQIIQQ", name_offsets[index], kind, flags,
                            addr, offsets[index], len(data), link, info,
                            align, entsize)
    header = b"\x7fELF" + bytes([2, 1, 1]) + bytes(9)
    header += struct.pack("<HHIQQQIHHHHHH", 1 if relocatable else 3,
                          machine, 1, 0, 0, shoff, 0, 64, 56, 0, 64,
                          len(sections) + 1, len(sections))
    body[:64] = header
    return bytes(body)


FAILURES: list[str] = []


def expect(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def words_of(function) -> dict[str, list]:
    found: dict[str, list] = {}
    for item in function.sm0:
        found.setdefault(item.classified.encoding, []).append(item)
    return found


def check_classes(table) -> None:
    for word, cls in ((0xD503201F, "a64"), (0x04BF5829, "sme"),
                      (0x04E0E3E9, "family"), (0x04A10000, "sve"),
                      (0xA400A000, "sve"), (0x80810000, "streaming-only"),
                      (0xE11F8200, "unadvertised"), (0xE1206200, "sme")):
        got = sme_sm0.classify(word, table).cls
        expect(got == cls, f"{word:08x} classifies as {got}, not {cls}")
    names = {row.encoding for row in table.rows}
    for encoding in sorted(sme_sm0.FAMILY):
        expect(encoding in names, f"the table lacks family form {encoding}")
        for row in table.rows:
            if row.encoding == encoding:
                got = sme_sm0.classify(row.witness, table)
                expect(got.cls == "family",
                       f"{encoding} witness classifies as {got.cls}")
    for encoding in ("sqincb_r_rs_x", "incp_r_p_r_", "incd_z_zs_",
                     "whilelo_p_p_rr_", "ld1b_z_p_bi_u8"):
        rows = [row for row in table.rows if row.encoding == encoding]
        expect(bool(rows) and all(
            sme_sm0.classify(row.witness, table).cls == "sve"
            for row in rows), f"{encoding} is not an SVE decision")
    spill = sme_sm0.classify(0xE58043E8, table)
    expect(spill.cls == "family" and spill.note == "callee-saved z8 save",
           f"str z8 is {spill}")
    spill = sme_sm0.classify(0x858017E9, table)
    expect(spill.cls == "family" and spill.note == "callee-saved p9 restore",
           f"ldr p9 is {spill}")


def check_analysis(table) -> None:
    analyzer = sme_sm0.Analyzer(sme_sm0.Elf(elf()), table)
    found = {function.name: function for function in analyzer.functions()}
    expect(sorted(found) == sorted([
        "ls_libyuv", "sc_vector", "st_calls_out", "ls_zspill", "za_disable",
        "st_za_mopa", "ns_jump", "__arm_sme_state", "sc_via_state"]),
        f"SME functions {sorted(found)}")
    expect(len(analyzer.data_like) == 1 and
           analyzer.data_like[0].bounds == "gap",
           "the ZA-like prefix data is not set aside")
    kinds = {name: function.kind for name, function in found.items()}
    expect(kinds == {
        "ls_libyuv": "non-streaming", "sc_vector": "streaming-compatible",
        "st_calls_out": "streaming", "ls_zspill": "non-streaming",
        "za_disable": "either", "st_za_mopa": "streaming",
        "ns_jump": "either", "__arm_sme_state": "streaming-compatible",
        "sc_via_state": "streaming-compatible"}, f"entry modes {kinds}")
    expected = {
        "ls_libyuv": {"rdsvl_r_i_": "sme", "cntd_r_s_": "family"},
        "sc_vector": {"add_z_zz_": "sve"},
        "st_calls_out": {},
        "ls_zspill": {"addsvl_r_ri_": "sme", "str_z_bi_": "family",
                      "ldr_z_bi_": "family"},
        "za_disable": {"str_za_ri_": "sme", "addsvl_r_ri_": "sme"},
        "st_za_mopa": {},
        "ns_jump": {"cntb_r_s_": "family"},
        "__arm_sme_state": {},
        "sc_via_state": {"cntw_r_s_": "family"},
    }
    for name, words in expected.items():
        function = found.get(name)
        if function is None:
            continue
        got = {encoding: items[0].classified.cls
               for encoding, items in words_of(function).items()}
        expect(got == words, f"{name}: SM=0 words {got}, want {words}")
    if "ls_zspill" in found:
        notes = sorted(item.classified.note for item in found["ls_zspill"].sm0
                       if item.classified.note)
        expect(notes == ["callee-saved z8 restore", "callee-saved z8 save"],
               f"ls_zspill notes {notes}")
        expect(len(found["ls_zspill"].findings()) == 0,
               "ls_zspill decisions")
    if "ns_jump" in found:
        expect(found["ns_jump"].unresolved_jump,
               "ns_jump's indirect jump is not marked unresolved")
    if "ls_libyuv" in found:
        expect(found["ls_libyuv"].bounds == "symbol", "ls_libyuv bounds")
        expect(found["ls_libyuv"].reached_markers == 2,
               "ls_libyuv reached markers")

    # Stripped: bounds from .eh_frame, then from the BTI C landing pad.
    _code, entries = layout(0x10000, ["st_za_mopa", "ls_libyuv"])
    start = {name: address for name, address, _size in entries}["ls_libyuv"]
    for fdes, bounds in ((["ls_libyuv"], "fde"), (None, "heuristic")):
        stripped = sme_sm0.Elf(elf(["st_za_mopa", "ls_libyuv"],
                                   symbols=False, fdes=fdes))
        result = {function.start: function for function in
                  sme_sm0.Analyzer(stripped, table).functions()}
        libyuv = result.get(start)
        expect(libyuv is not None and libyuv.bounds == bounds and
               libyuv.kind == "non-streaming" and
               sorted(words_of(libyuv)) == ["cntd_r_s_", "rdsvl_r_i_"],
               f"stripped ({bounds}) libyuv function: "
               f"{libyuv and (libyuv.bounds, libyuv.kind, words_of(libyuv))}")


def run(tool: str, *arguments) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-B", str(TOOLS / tool), *map(str, arguments)],
        capture_output=True, text=True, check=False,
        stdin=subprocess.DEVNULL, timeout=300)


def check_scan_tool(directory: pathlib.Path) -> None:
    tree = directory / "tree"
    library = tree / "usr" / "lib" / "libfixture.so"
    library.parent.mkdir(parents=True)
    library.write_bytes(elf())
    (tree / "usr" / "bin").mkdir(parents=True)
    (tree / "usr" / "bin" / "x86tool").write_bytes(elf(machine=62))
    with zipfile.ZipFile(tree / "app.apk", "w") as archive:
        archive.writestr("lib/arm64-v8a/libfixture.so", elf())
    prefix = directory / "report" / "scan"
    result = run("sme-sm0-scan.py", "--jobs", "2", "--report", prefix,
                 "--expect",
                 "lib/libfixture.so=add_z_zz_,cntb_r_s_,cntd_r_s_,cntw_r_s_,"
                 "ldr_z_bi_,str_z_bi_", "--expect", "usr/bin/missing=x",
                 tree)
    expect(result.returncode == 1, f"scan exit {result.returncode}")
    out = result.stdout
    expect("x86-64: 1" in out, "the x86-64 ELF is not counted")
    expect("aarch64: 2" in out, "the APK member is not scanned")
    expect("SM=0 words outside the family that the guest traps: 2" in out,
           "decision count:\n" + out)
    expect("expectation lib/libfixture.so: SM=0 SVE words" in out and
           "as expected" in out, "the expectation does not hold:\n" + out)
    expect("expectation usr/bin/missing: no such ELF file" in out,
           "a missing expected binary passes")
    rows = (directory / "report" / "scan.words.tsv").read_text()
    expect("app.apk!/lib/arm64-v8a/libfixture.so" in rows,
           "the APK member has no rows")
    expect("callee-saved z8 save" in rows, "the spill note is missing")

    clean = directory / "clean"
    clean.mkdir()
    (clean / "libyuv.so").write_bytes(elf(["st_za_mopa", "ls_libyuv"]))
    result = run("sme-sm0-scan.py", "--jobs", "1",
                 "--expect", "libyuv.so=cntd_r_s_", clean)
    expect(result.returncode == 0, "a family-only tree fails:\n" +
           result.stdout + result.stderr)
    result = run("sme-sm0-scan.py", "--jobs", "1",
                 "--expect", "libyuv.so=cntd_r_s_,rdvl_r_i_", clean)
    expect(result.returncode == 1, "a wrong expectation passes")
    empty = directory / "empty"
    empty.mkdir()
    result = run("sme-sm0-scan.py", "--jobs", "1", empty)
    expect(result.returncode == 2 and "no ELF files" in result.stderr,
           "an input without ELF files is not an error")


def check_corpus_tool(directory: pathlib.Path) -> None:
    objects = directory / "corpus" / "fixture-cc"
    objects.mkdir(parents=True)
    (objects / "armv9-a+sme_O2_base_default.o").write_bytes(
        elf(relocatable=True))
    (objects / "build.json").write_text(
        '{"compiler": "fixture", "variants": '
        '{"armv9-a+sme_O2_base_default": "built", '
        '"armv9-a+sme+nosve_O2_base_default": "rejected: no SVE"}}\n')
    result = run("sme-idiom-corpus.py", "report",
                 directory / "corpus")
    out = result.stdout
    expect(result.returncode == 1, f"corpus exit {result.returncode}")
    expect("rejected: no SVE" in out, "a rejected variant is not listed")
    decisions = out.split("Decisions:")[-1]
    expect("add_z_zz_" in decisions, "add_z_zz_ is not a decision")
    covered = out.split("Decisions:")[0]
    for encoding in ("cntd_r_s_", "str_z_bi_", "ldr_z_bi_"):
        expect(encoding in covered, f"{encoding} is not covered")
    for encoding in ("str_z_bi_", "ldr_z_bi_"):
        expect(encoding not in decisions, f"{encoding} is a decision")
    expect("entry mismatch" not in out, "declared and inferred modes "
           "disagree:\n" + out)


def main() -> int:
    table = sme_sm0.load_table()
    check_classes(table)
    check_analysis(table)
    with tempfile.TemporaryDirectory() as directory:
        check_scan_tool(pathlib.Path(directory))
        check_corpus_tool(pathlib.Path(directory))
    for failure in FAILURES:
        print(failure)
    if FAILURES:
        return 1
    print("SME SM=0 classifier: fixtures, scan and corpus tools agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
