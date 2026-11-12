#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the checked receipt validator from the rootfs tooling entry point."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from pathlib import Path
import sys


REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "tests/integration/gpu"))

from vulkan_cts_receipt import ReceiptError, main  # noqa: E402


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ReceiptError as error:
        print(f"verify-receipt.py: {error}", file=sys.stderr)
        raise SystemExit(1) from error
