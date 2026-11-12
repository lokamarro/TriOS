# SPDX-License-Identifier: GPL-2.0-or-later
"""The host's UML_X86_MM_SHARED_LOG, as the x86 runners read it.

Each line is a change of an address space's (generation's) state at the
dispatch that resumed at RIP: "mm-shared ... shared=0|1" for the kernel's
mm-shared mark, which starts unshared, "jit-space ... ordered=0|1" for the
x86-64 JIT's ordering, logged at a space's first dispatch too, and
"jit-unordered-shared" for a dispatch the kernel marked shared that ran
unordered code, which must never appear.
"""

from __future__ import annotations

import pathlib
import re
import typing


LOG_LINE = re.compile(
    r"uml-x86 (mm-shared|jit-space|jit-unordered-shared) generation=(\d+)"
    r"(?: shared=([01]))?(?: ordered=([01]))? rip=(0x[0-9a-f]+|0)"
)
# Which of shared= and ordered= each event carries.
FIELDS = {
    "mm-shared": (True, False),
    "jit-space": (False, True),
    "jit-unordered-shared": (False, False),
}
MARK = "mm-shared"
ORDERING = "jit-space"
UNORDERED_SHARED = "jit-unordered-shared"


class Entry(typing.NamedTuple):
    event: str
    generation: int
    state: int | None
    rip: int


def logged(log: pathlib.Path) -> list[Entry]:
    """Each line of the log; state is the mark or the ordering."""
    if not log.exists():
        return []
    entries = []
    for line in log.read_text().splitlines():
        match = LOG_LINE.fullmatch(line)
        if match is None or FIELDS[match.group(1)] != (
                match.group(3) is not None, match.group(4) is not None):
            raise RuntimeError(f"unexpected log line: {line!r}")
        state = match.group(3) or match.group(4)
        entries.append(Entry(match.group(1), int(match.group(2)),
                             None if state is None else int(state),
                             int(match.group(5), 16)))
    return entries


def changes(entries: list[Entry], event: str) -> dict[int, list[Entry]]:
    """Each generation's logged changes of one kind, in order."""
    by_generation: dict[int, list[Entry]] = {}
    for entry in entries:
        if entry.event == event:
            by_generation.setdefault(entry.generation, []).append(entry)
    return by_generation


def history(entries: list[Entry]) -> str:
    """A generation's changes as state@rip, for a report."""
    return " ".join(f"{entry.state}@{entry.rip:#x}" for entry in entries)
