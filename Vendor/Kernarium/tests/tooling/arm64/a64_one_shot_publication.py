#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Small validated atomic writer for A64 tooling outputs."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import csv
import io
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Mapping, Sequence

from a64_publication_paths import atomic_publish_bytes, validate_publication_topology


MAX_OUTPUT_BYTES = 64 * 1024 * 1024
MAX_OUTPUT_SET_BYTES = 128 * 1024 * 1024


def render_tsv(columns: Sequence[str], rows: Sequence[Mapping[str, str]]) -> bytes:
    """Render and round-trip-check one canonical UTF-8 TSV payload."""
    destination = io.StringIO(newline="")
    writer = csv.DictWriter(
        destination,
        fieldnames=list(columns),
        delimiter="\t",
        lineterminator="\n",
        extrasaction="raise",
    )
    writer.writeheader()
    writer.writerows(rows)
    payload = destination.getvalue().encode("utf-8")
    _validate_tsv_payload(payload, columns, rows)
    return payload


def _validate_tsv_payload(
    payload: bytes,
    columns: Sequence[str],
    rows: Sequence[Mapping[str, str]],
) -> None:
    try:
        text = payload.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise ValueError("published TSV is not valid UTF-8") from error
    reader = csv.DictReader(io.StringIO(text, newline=""), delimiter="\t")
    expected_columns = list(columns)
    if reader.fieldnames != expected_columns:
        raise ValueError(
            f"published TSV columns are {reader.fieldnames!r}, "
            f"expected {expected_columns!r}"
        )
    observed = list(reader)
    expected = [dict(row) for row in rows]
    if observed != expected:
        raise ValueError("published TSV rows differ from the validated result")


@dataclass(frozen=True)
class OneShotPublication:
    """Validated non-aliasing output targets."""

    targets: tuple[Path, ...]
    read_inputs: tuple[Path, ...]

    @classmethod
    def prepare(
        cls,
        targets: Sequence[Path],
        read_inputs: Sequence[Path],
    ) -> "OneShotPublication":
        safe_targets, _directories, safe_inputs = validate_publication_topology(
            tuple(targets), read_inputs=tuple(read_inputs)
        )
        return cls(safe_targets, safe_inputs)

    def recheck(self) -> None:
        validate_publication_topology(
            self.targets,
            read_inputs=self.read_inputs,
        )

    def publish_tsvs(
        self,
        outputs: Sequence[
            tuple[Path, Sequence[str], Sequence[Mapping[str, str]]]
        ],
    ) -> None:
        rendered = self._render_tsv_outputs(outputs)
        self.publish_payloads(rendered)

    def _render_tsv_outputs(
        self,
        outputs: Sequence[
            tuple[Path, Sequence[str], Sequence[Mapping[str, str]]]
        ],
    ) -> list[tuple[Path, bytes, Callable[[bytes], None]]]:
        rendered: list[tuple[Path, bytes, Callable[[bytes], None]]] = []
        for path, columns, rows in outputs:
            payload = render_tsv(columns, rows)

            def validate(
                observed: bytes,
                expected_columns: Sequence[str] = columns,
                expected_rows: Sequence[Mapping[str, str]] = rows,
            ) -> None:
                _validate_tsv_payload(
                    observed, expected_columns, expected_rows
                )

            rendered.append((path, payload, validate))
        return rendered

    def publish_payloads(
        self,
        outputs: Sequence[tuple[Path, bytes, Callable[[bytes], None]]],
    ) -> None:
        """Validate each payload, then publish the set atomically."""
        if tuple(path for path, _payload, _validator in outputs) != self.targets:
            raise ValueError("publication outputs differ from prepared targets")

        total_size = 0
        for path, payload, validator in outputs:
            if len(payload) > MAX_OUTPUT_BYTES:
                raise ValueError(
                    f"publication output exceeds {MAX_OUTPUT_BYTES} bytes: {path}"
                )
            validator(payload)
            total_size += len(payload)
        if total_size > MAX_OUTPUT_SET_BYTES:
            raise ValueError(
                "publication output set exceeds "
                f"{MAX_OUTPUT_SET_BYTES} bytes"
            )

        self.recheck()
        atomic_publish_bytes(
            [(path, payload) for path, payload, _validator in outputs],
        )
