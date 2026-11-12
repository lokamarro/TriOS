#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarise CTest JUnit timings by owning directory and by name family.

A suite's wall clock is only actionable once it is attributed. This joins one
or more `ctest --output-junit` files with the registrations of the build tree
that produced them and prints the time each owning directory and each
test-name family costs, plus the slowest individual tests.

    tools/ctest-time-breakdown.py --test-dir .build/presets/macos-unit \
        --junit unit=unit.xml

A test's owner is the CMake file that registered it, relative to
`--owner-root`; a name family is the first `--family-depth` dash-separated
components of the test name. Tests the build tree does not register are
reported under `(unregistered)` so they stay visible in the totals.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import json
import os
import pathlib
import statistics
import subprocess
import sys
import xml.etree.ElementTree as ElementTree

UNREGISTERED = "(unregistered)"

# name, seconds, status, and the JUnit file the case came from.
Case = tuple[str, float, str, str]


def read_owners(test_dir: pathlib.Path, source_root: pathlib.Path) -> dict[str, str]:
    """Map every test the build tree registers to the CMake file that did so."""
    listing = json.loads(
        subprocess.run(
            ["ctest", "--show-only=json-v1", "--test-dir", str(test_dir)],
            check=True,
            stdout=subprocess.PIPE,
            text=True,
        ).stdout
    )
    graph = listing["backtraceGraph"]
    owners: dict[str, str] = {}
    for test in listing["tests"]:
        node = test.get("backtrace")
        owner = None
        # The innermost frames are the shared suite helpers; the owner is
        # the first caller outside cmake/.
        while node is not None:
            frame = graph["nodes"][node]
            path = os.path.relpath(graph["files"][frame["file"]], source_root)
            if not path.startswith("cmake" + os.sep):
                owner = path
                break
            node = frame.get("parent")
        if owner is not None:
            owners[test["name"]] = owner
    return owners


def owner_group(owner: str | None, root: str) -> str:
    """Reduce an owner path to the directory that owns the test."""
    if owner is None:
        return UNREGISTERED
    root = root.rstrip("/")
    if owner == root:
        return "(top-level)"
    prefix = root + "/"
    rest = owner[len(prefix):] if owner.startswith(prefix) else owner
    head, separator, _ = rest.partition("/")
    if separator:
        return head
    # A bare leaf is either a directory owner or a single source file.
    return "(top-level)" if "." in head else head


def name_family(name: str, depth: int) -> str:
    parts = name.split("-")
    if len(parts) <= depth:
        return name
    return "-".join(parts[:depth])


def read_junit(path: pathlib.Path, origin: str) -> list[Case]:
    """Return (name, seconds, status, origin) for every case in a JUnit file."""
    cases: list[Case] = []
    for case in ElementTree.parse(path).getroot().iter("testcase"):
        name = case.get("name") or ""
        try:
            seconds = float(case.get("time") or 0.0)
        except ValueError:
            seconds = 0.0
        status = case.get("status") or ""
        if case.find("failure") is not None:
            status = status or "fail"
        elif case.find("skipped") is not None:
            status = status or "skipped"
        cases.append((name, seconds, status or "run", origin))
    return cases


def emit_table(title: str, groups: dict[str, list[float]], total: float) -> None:
    print(f"\n{title}")
    print(
        f"{'group':<24}{'tests':>7}{'seconds':>11}{'share':>9}"
        f"{'mean':>9}{'median':>9}{'max':>9}"
    )
    for group, times in sorted(
        groups.items(), key=lambda item: sum(item[1]), reverse=True
    ):
        group_total = sum(times)
        share = (group_total / total * 100.0) if total else 0.0
        print(
            f"{group:<24}{len(times):>7}{group_total:>11.2f}{share:>8.1f}%"
            f"{statistics.fmean(times):>9.3f}"
            f"{statistics.median(times):>9.3f}{max(times):>9.3f}"
        )
    print(f"{'TOTAL':<24}{sum(len(t) for t in groups.values()):>7}{total:>11.2f}")


def report(
    label: str,
    cases: list[Case],
    owners: dict[str, str],
    arguments: argparse.Namespace,
) -> None:
    total = sum(case[1] for case in cases)
    print(f"\n{'=' * 72}\n{label}: {len(cases)} tests, {total:.2f} s of test time")

    by_owner: dict[str, list[float]] = {}
    for name, seconds, _, _ in cases:
        by_owner.setdefault(
            owner_group(owners.get(name), arguments.owner_root), []
        ).append(seconds)
    emit_table(f"{label} by owner directory", by_owner, total)

    dominant = max(by_owner, key=lambda group: sum(by_owner[group]), default=None)
    if dominant is not None:
        members = [
            (name, seconds)
            for name, seconds, _, _ in cases
            if owner_group(owners.get(name), arguments.owner_root) == dominant
        ]
        by_family: dict[str, list[float]] = {}
        for name, seconds in members:
            by_family.setdefault(
                name_family(name, arguments.family_depth), []
            ).append(seconds)
        emit_table(
            f"{label} {dominant} by name family (depth {arguments.family_depth})",
            by_family,
            sum(seconds for _, seconds in members),
        )

        heaviest = max(by_family, key=lambda family: sum(by_family[family]))
        if sum(by_family[heaviest]) > sum(seconds for _, seconds in members) / 2:
            by_sub: dict[str, list[float]] = {}
            for name, seconds in members:
                if name_family(name, arguments.family_depth) != heaviest:
                    continue
                by_sub.setdefault(
                    name_family(name, arguments.family_depth + 1), []
                ).append(seconds)
            emit_table(
                f"{label} {heaviest} by sub-family "
                f"(depth {arguments.family_depth + 1})",
                by_sub,
                sum(sum(times) for times in by_sub.values()),
            )

    def line(case: Case) -> str:
        name, seconds, status, origin = case
        where = "" if origin == label else f" ({origin})"
        return f"{seconds:>9.2f}  {name}{where}  [{status}]"

    ranked = sorted(cases, key=lambda case: case[1], reverse=True)
    print(f"\n{label} slowest {arguments.top} tests")
    for case in ranked[: arguments.top]:
        print(line(case))

    over = [case for case in ranked if case[1] > arguments.over]
    print(f"\n{label} tests over {arguments.over:g} s: {len(over)}")
    for case in over:
        print(line(case))

    failures = [case for case in cases if case[2] not in ("run", "passed", "skipped")]
    print(f"\n{label} non-passing cases: {len(failures)}")
    for case in failures:
        print(line(case))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--junit",
        action="append",
        required=True,
        metavar="[LABEL=]PATH",
        help="a ctest --output-junit file; repeat for several runs",
    )
    parser.add_argument(
        "--test-dir",
        type=pathlib.Path,
        required=True,
        help="configured build tree whose registrations name each test's owner",
    )
    parser.add_argument(
        "--source-root",
        type=pathlib.Path,
        default=pathlib.Path("."),
        help="source tree the owners are relative to (default: %(default)s)",
    )
    parser.add_argument(
        "--owner-root",
        default="tests/unit",
        help="owner prefix stripped before grouping (default: %(default)s)",
    )
    parser.add_argument(
        "--family-depth",
        type=int,
        default=2,
        help="dash-separated components forming a name family (default: %(default)s)",
    )
    parser.add_argument(
        "--top", type=int, default=20, help="slowest tests to list (default: %(default)s)"
    )
    parser.add_argument(
        "--over",
        type=float,
        default=10.0,
        help="also list every test above this many seconds (default: %(default)s)",
    )
    parser.add_argument(
        "--combined",
        action="store_true",
        help="also report every JUnit file joined into one total",
    )
    arguments = parser.parse_args(argv)

    owners = read_owners(arguments.test_dir, arguments.source_root.resolve())
    everything: list[Case] = []
    for entry in arguments.junit:
        label, separator, path = entry.partition("=")
        if not separator:
            label, path = pathlib.Path(entry).stem, entry
        cases = read_junit(pathlib.Path(path), label)
        report(label, cases, owners, arguments)
        everything.extend(cases)

    if arguments.combined and len(arguments.junit) > 1:
        report("combined", everything, owners, arguments)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
