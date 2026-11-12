#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check darwin/cpu/x86_64/gadgets/engine/fit_slots.py against the engine.

With a table whose `common` bank slot is a page, the tool must name that slot
as too small and give the slot it needs; with that slot in the table it must
report every group as fitting.

usage: test_fit_slots.py --tool fit_slots.py --region region.inc
                         --build-dir DIR --ninja NINJA
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

COMMON = re.compile(r"^(\s*gadget_slot common,\s*\w+,\s*)(\w+)(,.*)$", re.M)


def run(arguments: argparse.Namespace, region: Path) -> tuple[int, str]:
    result = subprocess.run(
        [sys.executable, "-B", str(arguments.tool), "--build-dir",
         str(arguments.build_dir), "--ninja", arguments.ninja, "--region",
         str(region)], capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--region", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--ninja", default="ninja")
    arguments = parser.parse_args()
    text = arguments.region.read_text()
    if not COMMON.search(text):
        print(f"{arguments.region}: no row for common", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as temporary:
        region = Path(temporary) / "region.inc"
        region.write_text(COMMON.sub(r"\g<1>0x1000\g<3>", text))
        status, output = run(arguments, region)
        row = next((line.split() for line in output.splitlines()
                    if line.startswith("common ") and "bank" in line), None)
        if status != 1 or row is None or int(row[5]) >= 0:
            print("a page-sized common bank slot was not reported too small:\n"
                  + output, file=sys.stderr)
            return 1
        need = int(row[6])
        region.write_text(COMMON.sub(rf"\g<1>{need}\g<3>", text))
        status, output = run(arguments, region)
        if status != 0 or "every group fits" not in output:
            print(f"the slot {need} the tool asked for common does not fit:\n"
                  + output, file=sys.stderr)
            return 1
    print(f"common's bank slot needs {need} bytes; with it every group fits")
    return 0


if __name__ == "__main__":
    sys.exit(main())
