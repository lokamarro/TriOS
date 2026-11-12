#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the AArch32 encoding table, operand decoders and decoder test data.

The virtual CPU's AArch32 profile (docs/a32-instruction-coverage.md) is
Armv8-A AArch32 at EL0. This generator reads the pinned Arm AArch32 XML
release and writes, for the shared decoder in darwin/cpu/arm64/aarch32:

- a32_encodings.hpp and a32_encodings.cpp: one Form per XML encoding, one Row
  per allocated partition of the official encoding index, a decision tree per
  instruction set over the rows, and what the profile does with each form;
- a32_operand_decode.cpp: Arm's own Decode pseudocode of every encoding,
  translated to C++, which computes the operands both engines and the
  semantic library read; and
- the decoder's test data under tests/unit/arm64/aarch32_decoder: sample
  words per row with the form the index names and what LLVM MC decodes them
  to, and the exhaustive 16-bit Thumb space in runs.

Everything comes from the XML, the profile policy and LLVM MC 22.1.8, which
is a tooling-time oracle and never a build input. A shape the generator does
not recognise, or a disagreement it cannot explain, fails the generation.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import random
import sys
import tempfile
from pathlib import Path

import a32_architecture_oracle as oracle
import a32_llvm_data as data
import a32_llvm_oracle as llvm
import a32_profile_policy as policy
import a32_expected as expected
import a32_inventory as inventory
import a32_table_model as tm
import a32_table_render as render
from a64_bounded_generation import (
    BoundedCommandRunner,
    add_bounded_generation_arguments,
    workspace_from_options,
)
from a64_one_shot_publication import OneShotPublication, render_tsv


REPOSITORY = Path(__file__).resolve().parents[3]
SEED = "a32-encoding-table-v1"
RANDOM_WORDS = 60000
OUTPUTS = (
    "darwin/cpu/arm64/aarch32/a32_encodings.hpp",
    "darwin/cpu/arm64/aarch32/a32_encodings.cpp",
    "darwin/cpu/arm64/aarch32/a32_operand_decode.cpp",
    "tests/unit/arm64/aarch32_decoder/operands_digest.hpp",
    "tests/unit/arm64/aarch32_decoder/samples.tsv",
    "tests/unit/arm64/aarch32_decoder/t16_reference.tsv",
    "tests/unit/arm64/a32_instruction_inventory.tsv",
)
SAMPLE_COLUMNS = (
    "isa", "word", "kind", "form", "clean", "violation", "llvm", "soft",
    "status", "reason", "sysreg", "cond", "digest",
)
T16_COLUMNS = ("first", "last", "form", "llvm", "soft", "status", "reason")
# The profile every sample is decoded under: the whole of it, with the host
# features all present.
FEATURES = set(policy.FEATURE_ORDER)


def check_model(model: tm.Model) -> None:
    """Facts the C++ decoder hard-codes about the model."""
    it = model.by_name["IT_T1"].encoding
    fields = {f.name: (f.low, f.width) for f in it.fields}
    if fields.get("firstcond") != (20, 4) or fields.get("mask") != (16, 4):
        raise ValueError("decode.cpp reads IT's firstcond and mask at bits 23:20 and 19:16")
    members = {m.name for m in model.member_list}
    for name in ("d", "setflags", "read_spsr", "write_spsr"):
        if name not in members:
            raise ValueError(f"decode.cpp reads the operand {name}, which no program sets")
    # The engines load the vector registers lazily for a block that holds an
    # fp_state form, so the trait must be the XML's FP&SIMD section exactly.
    for form in model.forms:
        if form.encoding is None:
            continue
        in_section = form.encoding.section == "FP&SIMD"
        if in_section != ("fp_state" in form.traits):
            raise ValueError(
                f"{form.name}: the fp_state trait disagrees with the FP&SIMD section"
            )


def check_cover(model: tm.Model) -> None:
    """The tree finds the index partition's row, over many random words."""
    cache = render.tables(model)
    trees, rows = cache["trees"], cache["rows"]
    rng = random.Random(SEED)
    for isa in ("A32", "T16", "T32"):
        partitions = model.partitions["A32" if isa == "A32" else "T32"]
        for _ in range(RANDOM_WORDS):
            if isa == "A32":
                word = rng.getrandbits(32)
            elif isa == "T16":
                word = rng.randrange(0, 0xE800) << 16
            else:
                word = rng.randrange(0xE800, 0x10000) << 16 | rng.getrandbits(16)
            matching = [p for p in partitions if p.matches(word)]
            if len(matching) != 1:
                raise ValueError(
                    f"{isa} {word:#010x}: {len(matching)} index partitions match"
                )
            expected = matching[0]
            row = render.walk_tree(trees, rows, isa, word)
            if expected.kind == "unallocated":
                if row is not None and row.kind != "reserved_hint":
                    raise ValueError(f"{isa} {word:#010x}: row for unallocated word")
            elif row is None or row.partition != expected.name:
                raise ValueError(f"{isa} {word:#010x}: tree misses {expected.name}")


def check_samples(model: tm.Model, samples: list[data.Sample]) -> None:
    cache = render.tables(model)
    trees, rows = cache["trees"], cache["rows"]
    for sample in samples:
        form = model.by_name[sample.form]
        row = render.walk_tree(trees, rows, sample.isa, sample.word)
        if row is None or model.forms[row.form].name != sample.form:
            raise ValueError(f"{sample.isa} {sample.word:#010x} is not {sample.form}")
        if form.encoding is not None and not sample.violation:
            if not form.encoding.matches(sample.word):
                raise ValueError(f"{sample.word:#010x} is outside {sample.form}")


def witnesses(model: tm.Model, samples: list[data.Sample], tool: Path, runner) -> dict[str, int]:
    """A word LLVM decodes in every form: a stored sample where one exists,
    otherwise one searched for."""
    result: dict[str, int] = {}
    for sample in samples:
        if sample.kind == "instruction" and not sample.violation and sample.llvm != "-":
            result.setdefault(sample.form, sample.word)
    missing = {f.name for f in model.forms if f.encoding is not None and f.name not in result}
    result.update(data.find_witnesses(model, missing, SEED, tool, runner))
    for form in model.forms:
        if form.encoding is not None and form.name not in result:
            raise ValueError(f"no word LLVM decodes witnesses {form.name}")
    return result


def sample_rows(model: tm.Model, samples: list[data.Sample]) -> list[dict[str, str]]:
    order = {form.name: form.index for form in model.forms}
    isas = {"A32": 0, "T16": 1, "T32": 2}
    ordered = sorted(samples, key=lambda s: (isas[s.isa], order[s.form], s.word))
    return [
        {
            "isa": s.isa,
            "word": f"{s.word:08x}",
            "kind": s.kind,
            "form": s.form,
            "clean": str(int(s.clean)),
            "violation": str(int(s.violation)),
            "llvm": s.llvm,
            "soft": str(int(s.soft)),
            "status": s.expected.status,
            "reason": s.expected.reason,
            "sysreg": s.expected.sysreg,
            "cond": str(s.expected.cond),
            "digest": f"{s.expected.digest:016x}",
        }
        for s in ordered
    ]


def t16_rows(runs) -> list[dict[str, str]]:
    return [
        {
            "first": f"{first:04x}",
            "last": f"{last:04x}",
            "form": form,
            "llvm": opcode,
            "soft": str(int(soft)),
            "status": status,
            "reason": reason,
        }
        for first, last, form, opcode, soft, status, reason in runs
    ]


def inventory_rows(model: tm.Model, statuses: dict[str, str]) -> list[dict[str, str]]:
    """One inventory row per decoded (non-alias) XML encoding, in index order,
    each keeping the engine status the checked-in inventory records for it."""
    for name in policy.NEVER_EXECUTED:
        if name not in model.by_name:
            raise ValueError(f"profile policy names unknown encoding {name}")
    entries = [
        (
            form.encoding.section, form.encoding.form, form.name, form.isa,
            form.encoding.mnemonic, form.level, form.feature,
            form.availability, form.check or "none", form.nop,
        )
        for form in model.forms
        if form.encoding is not None
    ]
    return inventory.rows(entries, statuses)


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--architecture-archive", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, default=REPOSITORY)
    parser.add_argument("--llvm-mc", type=Path)
    parser.add_argument(
        "--status-input",
        type=Path,
        default=REPOSITORY / "tests/unit/arm64/a32_instruction_inventory.tsv",
        help="the inventory whose engine statuses the regeneration keeps "
        "(the checked-in one, not the one in --output-root)",
    )
    parser.add_argument("--timeout-seconds", type=int, default=900)
    add_bounded_generation_arguments(parser)
    return parser.parse_args()


def main() -> int:
    options = arguments()
    tool = llvm.find_llvm_mc(options.llvm_mc).resolve(strict=True)
    statuses = inventory.read_statuses(options.status_input)
    targets = [options.output_root / name for name in OUTPUTS]
    publication = OneShotPublication.prepare(targets, [options.architecture_archive, tool])
    with workspace_from_options(
        options,
        source_root=REPOSITORY,
        publication_targets=publication.targets,
        prefix="a32-",
    ) as workspace:
        runner = BoundedCommandRunner(workspace, options.timeout_seconds)
        version = llvm.require_pinned_llvm(tool, runner.run)
        root = oracle.archive_root(
            options.architecture_archive, workspace.require_path() / "archive", workspace
        )
        catalog = oracle.ArchitectureCatalog(root)
        index = oracle.DecodeIndex(root)
        model = tm.build_model(catalog, index)
        check_model(model)
        check_cover(model)
        reference = expected.Reference(model, catalog)
        samples = data.build_samples(model, reference, FEATURES, SEED, tool, runner.run)
        data.check_families(model, samples)
        data.check_agreement(model, samples)
        check_samples(model, samples)
        runs = data.t16_reference(model, reference, FEATURES, tool, runner.run)
        wit = witnesses(model, samples, tool, runner.run)
    payloads = [
        render.render_header(model).encode("utf-8"),
        render.render_tables(model, wit).encode("utf-8"),
        render.render_operands(model).encode("utf-8"),
        expected.render_digest_header(model).encode("utf-8"),
    ]
    sample_table = sample_rows(model, samples)
    t16_table = t16_rows(runs)
    payloads.append(render_tsv(SAMPLE_COLUMNS, sample_table))
    payloads.append(render_tsv(T16_COLUMNS, t16_table))
    payloads.append(render_tsv(inventory.COLUMNS, inventory_rows(model, statuses)))

    def exact(expected: bytes):
        def validate(observed: bytes) -> None:
            if observed != expected:
                raise ValueError("published output differs from the rendered one")

        return validate

    publication.publish_payloads(
        [(target, payload, exact(payload)) for target, payload in zip(publication.targets, payloads)]
    )
    print(
        f"AArch32 encoding table generated: {len(model.forms) - len(tm.PSEUDO_FORMS)} forms, "
        f"{sum(len(r) for r in model.rows.values())} rows, {len(model.programs)} operand "
        f"programs, {len(samples)} samples, {len(runs)} T16 runs, "
        f"{oracle.SOURCE_ORACLE}, {version}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, tm.ModelError, oracle.CatalogError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
