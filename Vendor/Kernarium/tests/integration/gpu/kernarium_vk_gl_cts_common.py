# SPDX-License-Identifier: GPL-2.0-or-later
"""Authenticated rootfs options shared by the rendered GPU runners."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import subprocess
import sys
from pathlib import Path
from typing import Any

import run_vk_gl_cts as cts


REPO_ROOT = Path(__file__).resolve().parents[3]
DERIVATIVE_RECEIPT_PATH = Path(__file__).with_name(
    "vk_gl_cts_derivative_rootfs.json"
)


def add_rootfs_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--rootfs", type=Path, default=cts.DEFAULT_ROOTFS)
    parser.add_argument(
        "--rootfs-type",
        choices=("erofs", "ext4"),
        default="erofs",
        help="authenticated lower filesystem type (default: erofs)",
    )
    parser.add_argument(
        "--rootfs-receipt",
        type=Path,
        help="required identity receipt for a derivative ext4 rootfs",
    )


def finish_rootfs_options(
    parser: argparse.ArgumentParser,
    options: argparse.Namespace,
    identity: dict[str, Any],
) -> None:
    try:
        receipt, contract = cts.select_rootfs_contract(
            identity, options.rootfs_type, options.rootfs_receipt
        )
        if receipt is not None:
            checked_receipt, checked_contract = cts.load_derivative_receipt(
                DERIVATIVE_RECEIPT_PATH, identity
            )
            if receipt != checked_receipt:
                raise cts.CampaignError(
                    "ext4 receipt does not match the checked derivative "
                    "rootfs identity"
                )
            receipt = checked_receipt
            contract = checked_contract
    except cts.CampaignError as error:
        parser.error(str(error))
    options.derivative_receipt = receipt
    options.rootfs_contract = contract


def validate_rootfs(
    rootfs: Path,
    identity: dict[str, Any],
    contract: cts.RootfsContract | None = None,
) -> None:
    if contract is None:
        _, contract = cts.select_rootfs_contract(identity, "erofs", None)
    if contract.filesystem_type == "ext4":
        cts.validate_rootfs_file(rootfs, contract)
        return
    frozen = identity["rootfs"]
    if not rootfs.is_file() or rootfs.stat().st_size != frozen["size"]:
        raise cts.CampaignError("rootfs size does not match the frozen identity")
    subprocess.run(
        [
            sys.executable,
            "-B",
            str(REPO_ROOT / "tools/rootfs/verify.py"),
            "--image",
            str(rootfs),
            "--manifest",
            str(REPO_ROOT / "tools/rootfs/manifest.json"),
            "--quiet",
        ],
        check=True,
    )

