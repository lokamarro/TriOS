#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the SME idiom corpus and list what compilers emit at SM = 0.

usage: sme-idiom-corpus.py build --cc LABEL=COMPILER[,FLAG...] --out DIR
       sme-idiom-corpus.py report [--llvm-mc PATH] [--tsv FILE] DIR

build compiles sme_idioms.cpp to objects with each compiler, for
-march=armv9-a+sme (SVE2 with SME), armv8.6-a+sme and armv9-a+sme+nosve
(SME without SVE where the compiler allows it), at -O1, -O2 and -O3, with
and without callee-saved register pressure (-DPRESSURE), and with the
compiler's default flags, hardened ones (stack protector, stack-clash
protection, branch protection) and no unwind tables. A combination the
compiler rejects is recorded with its first error. COMPILER is a C++
compiler for AArch64 Linux; give a cross target as a FLAG, such as
clang-22=clang++,--target=aarch64-linux-gnu.

report walks every function of every object from the mode its name
prefix declares (sme_idioms.cpp) with sme_sm0.py and prints each
distinct SVE or SME word executed at SM = 0, by class: family words, SME
words legal there, and the decisions (an SVE word outside the family or a
word that needs streaming mode; the callee-saved Z and P spills are family
words, whose note column names them). It also checks that the entry mode
the image scan would infer agrees with the declared one. The exit status is
1 when a decision exists.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import collections
import itertools
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sme_sm0  # noqa: E402

SOURCE = HERE / "sme_idioms.cpp"
MARCHES = ("armv9-a+sme", "armv8.6-a+sme", "armv9-a+sme+nosve")
LEVELS = ("-O1", "-O2", "-O3")
PRESSURE = {"base": [], "pressure": ["-DPRESSURE"]}
FLAGS = {
    "default": [],
    "hardened": ["-fstack-protector-strong", "-fstack-clash-protection",
                 "-mbranch-protection=standard"],
    "nounwind": ["-fno-asynchronous-unwind-tables", "-fno-unwind-tables"],
}
COMMON = ["-std=c++17", "-ffreestanding", "-fno-exceptions", "-fno-rtti",
          "-c"]
DECLARED = {"ns_": "non-streaming", "ls_": "non-streaming",
            "sc_": "streaming-compatible", "st_": "streaming"}


def variant_name(march: str, level: str, pressure: str, flags: str) -> str:
    return f"{march}_{level[1:]}_{pressure}_{flags}"


def build(arguments) -> int:
    out = arguments.out
    status = 0
    for spec in arguments.cc:
        label, _, command = spec.partition("=")
        if not label or not command:
            print(f"--cc {spec!r}: want LABEL=COMPILER[,FLAG...]")
            return 2
        compiler = command.split(",")
        directory = out / label
        directory.mkdir(parents=True, exist_ok=True)
        version = subprocess.run(compiler[:1] + ["--version"],
                                 capture_output=True, text=True,
                                 stdin=subprocess.DEVNULL, timeout=60)
        record = {"compiler": version.stdout.splitlines()[0]
                  if version.stdout else "", "variants": {}}
        for march, level, pressure, flags in itertools.product(
                MARCHES, LEVELS, PRESSURE, FLAGS):
            name = variant_name(march, level, pressure, flags)
            target = directory / f"{name}.o"
            command_line = compiler + [f"-march={march}", level] + \
                PRESSURE[pressure] + FLAGS[flags] + COMMON + \
                [str(SOURCE), "-o", str(target)]
            result = compile_once(command_line)
            if result.returncode == 0:
                record["variants"][name] = "built"
                continue
            first = first_error(result)
            # The retry leaves out the functions sme_idioms.cpp marks as
            # breaking a compiler, so the rest still gets built.
            retry = compile_once(command_line[:-3] + ["-DSME_IDIOMS_RETRY"] +
                                 command_line[-3:])
            if retry.returncode == 0:
                record["variants"][name] = f"built by the retry: {first}"
            else:
                target.unlink(missing_ok=True)
                record["variants"][name] = f"rejected: {first}"
        (directory / "build.json").write_text(
            json.dumps(record, indent=1, sort_keys=True) + "\n")
        built = sum(1 for value in record["variants"].values()
                    if value.startswith("built"))
        print(f"{label}: {record['compiler']}: {built} of "
              f"{len(record['variants'])} variants built")
    return status


def compile_once(command_line: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(command_line, capture_output=True, text=True,
                          stdin=subprocess.DEVNULL, timeout=600)


def first_error(result: subprocess.CompletedProcess) -> str:
    errors = [line for line in result.stderr.splitlines() if "error" in line]
    line = errors[0] if errors else result.stderr.strip()[:300]
    return line.replace(str(SOURCE), SOURCE.name)


def declared_kind(name: str) -> str | None:
    for prefix, kind in DECLARED.items():
        if name.startswith(prefix):
            return kind
    return None


def analyse_object(path: pathlib.Path, table) -> tuple[list, list]:
    """(rows, mismatches) for one object: a row per SM = 0 word that is
    not plain A64, and the functions whose inferred entry mode differs
    from the declared one."""
    elf = sme_sm0.Elf(path.read_bytes())
    analyzer = sme_sm0.Analyzer(elf, table)
    markers = elf.markers(table)
    rows = []
    mismatches = []
    for symbol in sorted(elf.symbols, key=lambda item: item.value):
        kind = declared_kind(symbol.name)
        if kind is None or symbol.kind != 2 or not symbol.size:
            continue
        start, end = symbol.value, symbol.value + symbol.size
        own = [(address, mark) for address, mark in markers
               if start <= address < end]
        function = analyzer.analyse(start, end, "symbol", own,
                                    {symbol.name: kind})
        if own:
            inferred = analyzer._infer(start, [(start, end)], [],
                                       function.calls)
            compatible = (inferred == kind or
                          (inferred == "either" and kind in (
                              "streaming-compatible", "non-streaming")))
            if not compatible:
                mismatches.append((symbol.name, kind, inferred))
        for item in function.sm0:
            rows.append((symbol.name, kind, item.address, item.word,
                         item.classified))
    return rows, mismatches


def report(arguments) -> int:
    table = sme_sm0.load_table()
    root = arguments.directory
    objects = sorted(root.rglob("*.o"))
    if not objects:
        print(f"{root}: no objects")
        return 2
    records = []
    mismatches = []
    for path in objects:
        label = path.parent.name
        variant = path.stem
        rows, wrong = analyse_object(path, table)
        records.extend((label, variant) + row for row in rows)
        mismatches.extend((label, variant) + item for item in wrong)
    words = {record[5] for record in records}
    texts = sme_sm0.disassemble(words,
                                sme_sm0.find_llvm_mc(arguments.llvm_mc))
    rejected = []
    for build_record in sorted(root.glob("*/build.json")):
        data = json.loads(build_record.read_text())
        for name, state in sorted(data["variants"].items()):
            if state != "built":
                rejected.append((build_record.parent.name, name, state))
    if arguments.tsv:
        with open(arguments.tsv, "w") as out:
            out.write("compiler\tvariant\tfunction\tentry\taddress\tword\t"
                      "class\tencoding\ttext\tnote\n")
            for label, variant, name, kind, address, word, classified in \
                    records:
                out.write("\t".join([
                    label, variant, name, kind, f"{address:#x}",
                    f"{word:08x}", classified.cls, classified.encoding,
                    texts.get(word, ""), classified.note]) + "\n")
    labels = sorted({path.parent.name for path in objects})
    print(f"SME idiom corpus: {len(objects)} objects from "
          f"{', '.join(labels)}")
    for label, name, state in rejected:
        print(f"  {label} {name}: {state}")
    groups: dict[tuple, dict] = {}
    for label, variant, name, kind, address, word, classified in records:
        decision = classified.cls in sme_sm0.DECISION_CLASSES
        key = (decision, classified.cls, classified.encoding,
               classified.note.split(" ")[0] if classified.note else "")
        group = groups.setdefault(key, {
            "count": 0, "functions": collections.Counter(),
            "compilers": set(), "marches": set(), "text": set()})
        group["count"] += 1
        group["functions"][name] += 1
        group["compilers"].add(label)
        group["marches"].add(variant.split("_")[0])
        group["text"].add(texts.get(word, "").split(" ")[0])
    decisions = 0
    for decision in (False, True):
        heading = ("Decisions: SM=0 words the guest traps" if decision else
                   "Covered: family words and SME words legal at SM=0")
        print(f"  {heading}")
        for key in sorted(k for k in groups if k[0] == decision):
            group = groups[key]
            _decision, cls, encoding, note = key
            decisions += group["count"] if decision else 0
            functions = ", ".join(sorted(group["functions"]))
            print(f"    {cls:14} {encoding:22} {note:12} "
                  f"{group['count']:5}  {','.join(sorted(group['compilers']))}"
                  f"  {','.join(sorted(group['marches']))}  [{functions}]")
    for label, variant, name, kind, inferred in mismatches:
        print(f"  entry mismatch {label} {variant} {name}: declared {kind}, "
              f"inferred {inferred}")
    return 1 if decisions or mismatches else 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    build_parser = commands.add_parser("build")
    build_parser.add_argument("--cc", action="append", required=True)
    build_parser.add_argument("--out", type=pathlib.Path, required=True)
    report_parser = commands.add_parser("report")
    report_parser.add_argument("directory", type=pathlib.Path)
    report_parser.add_argument("--llvm-mc")
    report_parser.add_argument("--tsv", type=pathlib.Path)
    arguments = parser.parse_args(argv)
    if arguments.command == "build":
        return build(arguments)
    return report(arguments)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
