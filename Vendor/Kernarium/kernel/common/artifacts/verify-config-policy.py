#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a kernel .config against a product's required and forbidden lists."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from pathlib import Path

from kernel_artifact import verify_config_policy


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--required", type=Path, required=True)
    parser.add_argument("--forbidden", type=Path, required=True)
    args = parser.parse_args()
    verify_config_policy(args.config, args.required, args.forbidden)
    print(f"verified kernel config policy: {args.config}")


if __name__ == "__main__":
    main()
