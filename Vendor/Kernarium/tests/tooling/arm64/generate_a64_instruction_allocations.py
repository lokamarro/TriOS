#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate pinned A64 form/allocation evidence from Arm XML and LLVM."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import hashlib
import sys
import tarfile
from pathlib import Path
from typing import Callable

from a64_architecture_oracle import (
    ARCHIVE_NAME,
    ARCHIVE_SHA256,
    RELEASE,
    SOURCE_ORACLE,
    ArchitectureCatalog,
    feature_alternatives,
)
from a64_bounded_generation import (
    BoundedCommandRunner,
    BoundedWorkspace,
    add_bounded_generation_arguments,
    workspace_from_options,
)
from a64_one_shot_publication import OneShotPublication
from a64_llvm_oracle import decoder_opcodes, find_llvm_mc, require_pinned_llvm


BINDING_COLUMNS = [
    "stable_id",
    "section",
    "architectural_form",
    "ctest",
    "case",
    "encoding",
    "alias_group",
    "native_required_features",
]
CONTRACT_COLUMNS = [
    "stable_id",
    "variant_id",
    "section",
    "architectural_form",
    "decoder_opcode",
    "encoding_mask",
    "encoding_value",
    "validity",
    "native_required_features",
    "expected_event",
    "expected_retirement",
    "expected_state_oracle",
    "expected_state_digest",
    "expected_memory_digest",
    "expected_fault",
    "expected_syndrome",
    "gadgets_result",
    "jit_result",
    "entry_complete",
    "oracle",
]
OUTPUT_COLUMNS = [
    "stable_id",
    "section",
    "architectural_form",
    "binding_encoding",
    "architecture_file",
    "architecture_encoding",
    "architecture_form_encoding_count",
    "architecture_mask",
    "architecture_value",
    "architecture_constraints",
    "architecture_features",
    "architecture_native_feature_alternatives",
    "decoder_opcode",
    "decoder_mask",
    "decoder_value",
    "encoding_mask",
    "encoding_value",
    "architecture_oracle",
    "decoder_oracle",
]
AUXILIARY_FEATURES = {"afp"}


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument("--architecture-archive", type=Path, required=True)
    parser.add_argument(
        "--bindings",
        type=Path,
        default=repository
        / "tests/unit/arm64/a64_instruction_evidence_bindings.tsv",
    )
    parser.add_argument(
        "--variant-contracts",
        type=Path,
        default=repository
        / "tests/unit/arm64/a64_instruction_variant_contracts.tsv",
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--llvm-mc", type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=120)
    add_bounded_generation_arguments(parser)
    return parser.parse_args()


def read_tsv(path: Path, columns: list[str]) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != columns:
            raise ValueError(
                f"{path}: columns are {reader.fieldnames!r}, expected {columns!r}"
            )
        rows = list(reader)
    if not rows:
        raise ValueError(f"{path}: contains no rows")
    return rows


def archive_root(
    archive: Path,
    temporary: Path,
    workspace: BoundedWorkspace | None = None,
) -> Path:
    if archive.name != ARCHIVE_NAME:
        raise ValueError(
            f"architecture archive is {archive.name!r}, expected {ARCHIVE_NAME!r}"
        )
    digest_hash = hashlib.sha256()
    with archive.open("rb") as archive_input:
        for block in iter(lambda: archive_input.read(1024 * 1024), b""):
            digest_hash.update(block)
    digest = digest_hash.hexdigest()
    if digest != ARCHIVE_SHA256:
        raise ValueError(
            f"{archive}: SHA-256 is {digest}, expected pinned {ARCHIVE_SHA256}"
        )
    expected_root = f"ISA_A64_xml_A_profile-{RELEASE}"
    with tarfile.open(archive, "r:gz") as source:
        members = source.getmembers()
        for member in members:
            path = Path(member.name)
            if path.is_absolute() or ".." in path.parts:
                raise ValueError(f"{archive}: unsafe archive member {member.name!r}")
            if path.parts and path.parts[0] not in {
                expected_root,
                f"ISA_A64_xml_A_profile-{RELEASE}.pdf",
                f"ISA_A64_xml_A_profile-{RELEASE}-diff-from-ISA_A64_xml_A_profile-2026-03.pdf",
                f"ISA_A64_xml_A_profile-{RELEASE}_relnote.html",
                "ISA_A64_xml_A_profile-2026-03",
            }:
                raise ValueError(
                    f"{archive}: unexpected top-level member {member.name!r}"
                )
            if member.issym() or member.islnk():
                raise ValueError(f"{archive}: link member is not accepted")
        if workspace is not None:
            workspace.validate_archive_members(members)
        source.extractall(temporary, filter="data")
    if workspace is not None:
        workspace.check()
    root = temporary / expected_root
    if not root.is_dir():
        raise ValueError(f"{archive}: lacks {expected_root}")
    return root


def exact_features(value: str) -> frozenset[str]:
    if value == "baseline":
        return frozenset()
    if value.startswith("per_variant(") or "|" in value:
        raise ValueError(f"binding feature requirement is not exact: {value!r}")
    return frozenset(value.split("+")) - AUXILIARY_FEATURES


def format_alternatives(alternatives: set[frozenset[str]]) -> str:
    terms = [
        "baseline" if not features else "+".join(sorted(features))
        for features in alternatives
    ]
    return "|".join(sorted(terms))


NEIGHBOR_BATCH_WORDS = 16_384


def decoder_masks(
    tool: Path,
    words: list[int],
    opcodes: list[str | None],
    runner: Callable[..., object],
) -> list[tuple[int, int]]:
    neighbors = [word ^ (1 << bit) for word in words for bit in range(32)]
    # One LLVM MC run per batch keeps each run's output under the bounded
    # runner's log cap.
    batch = NEIGHBOR_BATCH_WORDS
    decoded_neighbors: list[str | None] = []
    for start in range(0, len(neighbors), batch):
        decoded_neighbors += decoder_opcodes(
            tool, neighbors[start : start + batch], runner=runner
        )
    result: list[tuple[int, int]] = []
    for row_index, (word, opcode) in enumerate(zip(words, opcodes)):
        mask = 0
        for bit in range(32):
            if decoded_neighbors[row_index * 32 + bit] != opcode:
                mask |= 1 << bit
        result.append((mask, word & mask))
    return result


def reviewed_contract(
    binding: dict[str, str], word: int, contracts: list[dict[str, str]]
) -> dict[str, str] | None:
    matches = []
    for contract in contracts:
        if (
            contract["stable_id"] != binding["stable_id"]
            or contract["section"] != binding["section"]
            or contract["architectural_form"] != binding["architectural_form"]
        ):
            continue
        mask = int(contract["encoding_mask"], 16)
        value = int(contract["encoding_value"], 16)
        if word & mask == value:
            matches.append(contract)
    if matches:
        specificity = max(int(row["encoding_mask"], 16).bit_count() for row in matches)
        matches = [
            row
            for row in matches
            if int(row["encoding_mask"], 16).bit_count() == specificity
        ]
    if len(matches) > 1:
        raise ValueError(
            f"{binding['stable_id']} {word:08x}: witness matches multiple "
            "independently reviewed contracts"
        )
    return matches[0] if matches else None


def generate(
    catalog: ArchitectureCatalog,
    bindings: list[dict[str, str]],
    contracts: list[dict[str, str]],
    tool: Path,
    decoder_oracle: str,
    runner: Callable[..., object],
) -> list[dict[str, str]]:
    words = [int(binding["encoding"], 16) for binding in bindings]
    opcodes = decoder_opcodes(tool, words, runner=runner)
    masks = decoder_masks(tool, words, opcodes, runner)
    result: list[dict[str, str]] = []
    for binding, word, opcode, (mask, value) in zip(
        bindings, words, opcodes, masks
    ):
        official_section, official_form, _official_file = catalog.stable_form(
            binding["stable_id"]
        )
        if (
            binding["section"] != official_section
            or binding["architectural_form"] != official_form
        ):
            raise ValueError(
                f"{binding['stable_id']}: binding names "
                f"{binding['section']} {binding['architectural_form']!r}, "
                "but the official Arm XML index names "
                f"{official_section} {official_form!r}"
            )
        variant = catalog.resolve(
            binding["section"], binding["architectural_form"], word
        )
        alternatives = feature_alternatives(
            variant.feature_expressions, variant.file
        )
        required = exact_features(binding["native_required_features"])
        if required not in alternatives:
            raise ValueError(
                f"{binding['stable_id']} {word:08x}: exact native features "
                f"{sorted(required)!r} are not an Arm XML-derived runtime "
                f"alternative {sorted(map(sorted, alternatives))!r}"
            )
        reviewed = reviewed_contract(binding, word, contracts)
        if opcode is None:
            if reviewed is None or reviewed["validity"] == "valid":
                raise ValueError(
                    f"{binding['stable_id']} {word:08x}: invalid decoder witness "
                    "lacks one independently reviewed non-valid contract"
                )
            decoder_opcode = reviewed["decoder_opcode"]
        else:
            decoder_opcode = opcode
            if reviewed is not None and (
                reviewed["validity"] != "valid"
                or reviewed["decoder_opcode"] != opcode
            ):
                raise ValueError(
                    f"{binding['stable_id']} {word:08x}: LLVM validity/opcode "
                    "disagrees with the independently reviewed contract"
                )
        allocation_mask = (
            int(reviewed["encoding_mask"], 16) if reviewed is not None else mask
        )
        allocation_value = (
            int(reviewed["encoding_value"], 16) if reviewed is not None else value
        )
        result.append(
            {
                "stable_id": binding["stable_id"],
                "section": binding["section"],
                "architectural_form": binding["architectural_form"],
                "binding_encoding": f"{word:08x}",
                "architecture_file": variant.file,
                "architecture_encoding": variant.encoding,
                "architecture_form_encoding_count": str(
                    catalog.encoding_count(
                        binding["section"], binding["architectural_form"]
                    )
                ),
                "architecture_mask": f"{variant.mask:08x}",
                "architecture_value": f"{variant.value:08x}",
                "architecture_constraints": ";".join(
                    constraint.text() for constraint in variant.constraints
                )
                or "none",
                "architecture_features": ";".join(
                    variant.feature_expressions
                )
                or "baseline",
                "architecture_native_feature_alternatives": (
                    format_alternatives(alternatives)
                ),
                "decoder_opcode": decoder_opcode,
                "decoder_mask": f"{mask:08x}",
                "decoder_value": f"{value:08x}",
                "encoding_mask": f"{allocation_mask:08x}",
                "encoding_value": f"{allocation_value:08x}",
                "architecture_oracle": SOURCE_ORACLE,
                "decoder_oracle": decoder_oracle,
            }
        )
    return result


def main() -> int:
    options = arguments()
    tool = find_llvm_mc(options.llvm_mc).resolve(strict=True)
    publication = OneShotPublication.prepare(
        [options.output],
        [
            options.architecture_archive,
            options.bindings,
            options.variant_contracts,
            tool,
        ],
    )
    with workspace_from_options(
        options,
        source_root=Path(__file__).resolve().parents[3],
        publication_targets=publication.targets,
        prefix="a64-architecture-",
    ) as workspace:
        runner = BoundedCommandRunner(workspace, options.timeout_seconds)
        decoder_oracle = require_pinned_llvm(tool, runner.run)
        bindings = read_tsv(options.bindings, BINDING_COLUMNS)
        contracts = read_tsv(options.variant_contracts, CONTRACT_COLUMNS)
        catalog = ArchitectureCatalog(
            archive_root(
                options.architecture_archive,
                workspace.require_path() / "archive",
                workspace,
            )
        )
        rows = generate(
            catalog,
            bindings,
            contracts,
            tool,
            decoder_oracle,
            runner.run,
        )
    publication.publish_tsvs(
        [(publication.targets[0], OUTPUT_COLUMNS, rows)]
    )
    print(
        f"A64 official allocation oracle generated: {len(rows)} bindings, "
        f"{SOURCE_ORACLE}, {decoder_oracle}; wrote {options.output}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, tarfile.TarError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
