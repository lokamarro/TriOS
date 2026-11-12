#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate exact reserved/allocated predicates from the Arm decode index."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import sys
from collections import defaultdict
from pathlib import Path

from a64_architecture_oracle import SOURCE_ORACLE, ArchitectureCatalog, DecodeCatalog
from a64_bounded_generation import (
    add_bounded_generation_arguments,
    workspace_from_options,
)
from a64_one_shot_publication import OneShotPublication
from generate_a64_instruction_allocations import archive_root


COLUMNS = [
    "partition_id",
    "decode_layer",
    "partition_kind",
    "decode_class",
    "partition_name",
    "architecture_file",
    "related_stable_ids",
    "architecture_mask",
    "architecture_value",
    "architecture_constraints",
    "feature_condition",
    "architecture_oracle",
]


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument("--architecture-archive", type=Path, required=True)
    parser.add_argument(
        "--output",
        type=Path,
        default=(
            repository
            / "tests/unit/arm64/a64_instruction_decode_partitions.tsv"
        ),
    )
    add_bounded_generation_arguments(parser)
    return parser.parse_args()


def generate(root: Path) -> list[dict[str, str]]:
    catalog = ArchitectureCatalog(root)
    stable_ids_by_file: dict[str, list[str]] = defaultdict(list)
    for stable_id, (_section, _form, filename) in catalog.stable_forms.items():
        stable_ids_by_file[filename].append(stable_id)
    partitions = DecodeCatalog(root).partitions()
    stable_ids_by_class: dict[str, set[str]] = defaultdict(set)
    for partition in partitions:
        if partition.kind == "allocated":
            stable_ids_by_class[partition.decode_class].update(
                stable_ids_by_file.get(partition.file, [])
            )
    rows: list[dict[str, str]] = []
    for partition in partitions:
        constraints = ";".join(
            constraint.text() for constraint in partition.constraints
        ) or "none"
        identity = "\0".join(
            (
                partition.layer,
                partition.kind,
                partition.decode_class,
                partition.name,
                partition.file,
                f"{partition.mask:08x}",
                f"{partition.value:08x}",
                constraints,
            )
        )
        rows.append(
            {
                "partition_id": "decode-"
                + hashlib.sha256(identity.encode()).hexdigest()[:20],
                "decode_layer": partition.layer,
                "partition_kind": partition.kind,
                "decode_class": partition.decode_class,
                "partition_name": partition.name,
                "architecture_file": partition.file,
                "related_stable_ids": "+".join(sorted(
                    stable_ids_by_file.get(partition.file, [])
                    or stable_ids_by_class.get(partition.decode_class, set())
                ))
                or "none",
                "architecture_mask": f"{partition.mask:08x}",
                "architecture_value": f"{partition.value:08x}",
                "architecture_constraints": constraints,
                "feature_condition": ";".join(
                    partition.feature_expressions
                )
                or "baseline",
                "architecture_oracle": SOURCE_ORACLE,
            }
        )
    rows.sort(
        key=lambda row: (
            row["decode_layer"],
            row["decode_class"],
            row["partition_name"],
            row["partition_id"],
        )
    )
    return rows


def main() -> int:
    options = arguments()
    publication = OneShotPublication.prepare(
        [options.output], [options.architecture_archive]
    )
    with workspace_from_options(
        options,
        source_root=Path(__file__).resolve().parents[3],
        publication_targets=publication.targets,
        prefix="a64-decode-",
    ) as workspace:
        rows = generate(
            archive_root(
                options.architecture_archive,
                workspace.require_path() / "archive",
                workspace,
            )
        )
    counts = {
        kind: sum(row["partition_kind"] == kind for row in rows)
        for kind in ("allocated", "unallocated", "instruction-class")
    }
    if counts != {
        "allocated": 4334,
        "unallocated": 1253,
        "instruction-class": 638,
    }:
        raise ValueError(f"unexpected official decode partition counts {counts}")
    if len({row["partition_id"] for row in rows}) != len(rows):
        raise ValueError("official decode partition IDs are not unique")
    publication.publish_tsvs([(publication.targets[0], COLUMNS, rows)])
    print(
        "A64 official decode partitions generated: "
        f"{counts['allocated']} allocated, {counts['unallocated']} "
        f"unallocated, {counts['instruction-class']} instruction-class; "
        f"wrote {options.output}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
