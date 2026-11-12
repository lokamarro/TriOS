#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate the shared-engine A64 instruction inventory."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import csv
import re
import sys
from collections import Counter
from pathlib import Path


COLUMNS = [
    "stable_id",
    "section",
    "pdf_label",
    "current_suite",
    "current_variant_model",
    "native_feature_requirements",
    "engine_status",
]
# Each section numbers its stable IDs under its own prefix.
SECTION_PREFIXES = {"Base": "BASE", "FP&SIMD": "FPSIMD", "SVE": "SVE",
                    "SME": "SME"}
ENGINE_STATUSES = {"absent", "pending", "verified"}
# Features the virtual CPU omits. An absent entry needs one of them in every
# alternative; its corpus cases expect its encodings to be UNDEFINED. Every
# SVE feature is among them, as is every SME feature outside the SME profile:
# the virtual CPU executes SVE only in streaming mode. These names are valid
# requirement tokens too, whether or not they have a runtime ArmFeature
# identity.
ABSENT_FEATURES = {
    "cmpbr", "cpa", "d128", "f16f32dot", "f16f32mm", "f16mm", "f32mm", "f64mm",
    "f8f16mm", "f8f32mm", "faminmax", "fp8", "fp8dot2", "fp8dot4", "fp8fma",
    "fprcvt", "gcs", "hbc", "lor", "lrcpc3", "ls64", "ls64_accdata", "ls64_v",
    "lscp", "lse128", "lsfe", "lsui", "lut", "mte", "mte2", "mtetc", "pauth",
    "sm3", "sm4", "sme2p1", "sme2p2", "sme2p3", "sme_b16b16", "sme_f16f16",
    "sme_f8f16", "sme_f8f32", "sme_lutv2", "sme_mop4", "sme_tmop", "ssbs",
    "ssve_fexpa", "ssve_fp8dot2", "ssve_fp8dot4", "ssve_fp8fma", "sve", "sve2",
    "sve2p1", "sve2p2", "sve2p3", "sve_aes", "sve_aes2", "sve_b16b16",
    "sve_b16mm", "sve_bfscale", "sve_bitperm", "sve_f16f32mm", "sve_pmull128",
    "sve_sha3", "sve_sm4", "the", "wfxt", "xs",
}
SUITE_VARIANT_MODELS = {
    "shared-base-control-system": {
        "condition+target-form+register-alias+architectural-state+reserved",
        "condition+target-form+register-alias+flags+reserved",
    },
    "shared-base-data-processing": {
        "register-width+operand-form+register-alias+flags+reserved",
    },
    "shared-base-memory": {
        "access-size+addressing-mode+writeback+register-alias+fault-ordering+reserved",
        "immediate+monitor-state+register-preservation",
    },
    "shared-floating-scalar": {
        "precision+register-alias+fp-state+rounding+reserved",
    },
    "shared-simd-data-processing": {
        "scalar-vector+element-size+arrangement+register-alias+fp-state+reserved",
    },
    "shared-simd-memory": {
        "structure+element-size+addressing-mode+writeback+register-alias+fault-ordering+reserved",
    },
    "shared-sve-data-processing": {
        "element-size+predication+register-alias+streaming-mode+reserved",
    },
    "shared-sve-memory": {
        "element-size+predication+addressing-mode+page-crossing+fault-ordering+streaming-mode+reserved",
    },
    "shared-sme-data-processing": {
        "tile+slice+element-size+predication+register-group+za-state+reserved",
    },
    "shared-sme-memory": {
        "tile+slice+element-size+predication+addressing-mode+page-crossing+fault-ordering+za-state+reserved",
    },
}
def fail(message: str) -> None:
    raise ValueError(message)


def validate_requirement(
    value: str, stable_id: str, features: set[str]
) -> None:
    if value == "baseline":
        return
    expression = value
    if value.startswith("per_variant(") and value.endswith(")"):
        expression = value[len("per_variant(") : -1]
    elif "|" in value:
        fail(f"{stable_id}: alternatives require per_variant(...)")
    terms = expression.split("|")
    if not terms or any(not term for term in terms):
        fail(f"{stable_id}: malformed native feature expression")
    for term in terms:
        if term == "baseline":
            continue
        tokens = term.split("+")
        if any(token not in features for token in tokens):
            fail(f"{stable_id}: unknown native feature in {term!r}")


def main() -> int:
    repository = Path(__file__).resolve().parents[3]
    inventory = repository / "tests/unit/arm64/a64_instruction_inventory.tsv"
    # The feature representation is a neutral platform contract shared by the
    # Apple probe and CPU engines. Neither side owns the other's types.
    host_features = repository / "darwin/platform/host_capabilities.hpp"

    with inventory.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != COLUMNS:
            fail(f"columns are {reader.fieldnames!r}, expected {COLUMNS!r}")
        rows = list(reader)

    feature_source = host_features.read_text(encoding="utf-8")
    # The inventory features are the X(token, "Label") rows of
    # UML_ARM_INVENTORY_FEATURES; the extra ones after them have no row.
    inventory_match = re.search(
        r"#define UML_ARM_INVENTORY_FEATURES\(X\)"
        r"(?P<body>(?:[^\n]*\\\n)*[^\n]*)",
        feature_source,
    )
    if inventory_match is None:
        fail("cannot parse ArmFeature inventory members")
    enum_features = set(
        re.findall(
            r"^\s*X\(([a-z][a-z0-9_]*),",
            inventory_match.group("body"),
            re.MULTILINE,
        )
    )
    if not enum_features:
        fail("ArmFeature lists no inventory members")
    counts = Counter(row["section"] for row in rows)
    status_counts = Counter(row["engine_status"] for row in rows)
    verified_sections = Counter(
        row["section"]
        for row in rows
        if row["engine_status"] == "verified"
    )

    ids = [row["stable_id"] for row in rows]
    if len(set(ids)) != len(ids):
        fail("stable IDs are not unique")
    identities = [(row["section"], row["pdf_label"]) for row in rows]
    if len(set(identities)) != len(identities):
        fail("(section, PDF label) identities are not unique")

    expected_ids: list[str] = []
    section_indices = Counter()
    for row in rows:
        section = row["section"]
        if section not in SECTION_PREFIXES:
            fail(f"{row['stable_id']}: unknown section {section!r}")
        section_indices[section] += 1
        expected_ids.append(
            f"{SECTION_PREFIXES[section]}-{section_indices[section]:04d}"
        )
        for field in ("pdf_label", "current_suite", "current_variant_model"):
            if not row[field].strip():
                fail(f"{row['stable_id']}: {field} is empty")
        suite = row["current_suite"]
        model = row["current_variant_model"]
        if suite not in SUITE_VARIANT_MODELS:
            fail(f"{row['stable_id']}: unknown current suite {suite!r}")
        if model not in SUITE_VARIANT_MODELS[suite]:
            fail(
                f"{row['stable_id']}: variant model {model!r} is not "
                f"valid for suite {suite!r}"
            )
        if row["engine_status"] not in ENGINE_STATUSES:
            fail(
                f"{row['stable_id']}: unknown engine status "
                f"{row['engine_status']!r}"
            )
        validate_requirement(
            row["native_feature_requirements"],
            row["stable_id"],
            enum_features | ABSENT_FEATURES,
        )
        if row["engine_status"] == "absent":
            requirement = row["native_feature_requirements"]
            if requirement.startswith("per_variant("):
                requirement = requirement[len("per_variant(") : -1]
            if any(
                not set(term.split("+")) & ABSENT_FEATURES
                for term in requirement.split("|")
            ):
                fail(
                    f"{row['stable_id']}: absent entry needs an omitted "
                    "feature in every alternative"
                )
    if ids != expected_ids:
        fail("stable IDs are not contiguous in section order")

    sections = ", ".join(
        f"{counts[section]} {section}" for section in SECTION_PREFIXES
    )
    verified = ", ".join(
        f"{verified_sections[section]} {section}"
        for section in SECTION_PREFIXES
    )
    print(
        "A64 instruction inventory valid: "
        f"{len(rows)} rows ({sections}); "
        f"{status_counts['verified']} verified ({verified}), "
        f"{status_counts['absent']} absent, "
        f"{status_counts['pending']} pending"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
