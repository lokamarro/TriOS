# SPDX-License-Identifier: GPL-2.0-or-later
"""Accepted CTS receipts rebound to the current guest source epoch.

An accepted receipt names the source locks and patched series trees its image
was sealed from, and the runners refuse an image whose receipt names any other.
Runner tests check runner logic, not whether that image is current, so they use
a copy of an accepted receipt that names the current locks and trees; a guest
series that produces a new tree leaves them passing. Whether the accepted
receipts themselves are current is asserted once, by
``test_vulkan_guest_rootfs.py``.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import json
from pathlib import Path
import sys
import tempfile
from typing import Any
import unittest
from unittest import mock


INTEGRATION_GPU = Path(__file__).resolve().parents[2] / "integration/gpu"
if str(INTEGRATION_GPU) not in sys.path:
    sys.path.insert(0, str(INTEGRATION_GPU))
import vulkan_cts_receipt as receipt_contract  # noqa: E402


VULKAN_RECEIPT = receipt_contract.RECEIPT_PATH
ZINK_GL_RECEIPT = INTEGRATION_GPU / "vk_gl_cts_derivative_rootfs.json"
# The receipt fields verify_source_epoch() binds to sources.tsv components.
SOURCE_FIELDS = (
    ("protocol", "venus-protocol"),
    ("mesa", "mesa"),
    ("vulkan_headers", "vulkan-headers"),
    ("vulkan_loader", "vulkan-loader"),
    ("cts", "vk-gl-cts"),
)


def current_epoch_receipt(source: Path) -> dict[str, Any]:
    """Return an accepted receipt with the current source locks and trees."""
    receipt = receipt_contract.load_json(source)
    locks = {
        row["component"]: row
        for row in receipt_contract.read_tsv(
            receipt_contract.METADATA_ROOT / "sources.tsv"
        )
    }
    for field, component in SOURCE_FIELDS:
        lock = locks[component]
        identity = receipt[field]
        for name in ("repository", "base_commit", "base_tree"):
            identity[name] = lock[name]
        if lock["release_ref"] != "-":
            identity["release_ref"] = lock["release_ref"]
            identity["release_object"] = lock["release_object"]
        if lock["series"] != "-":
            identity["patched_tree"] = receipt_contract.recorded_tree(lock)
    return receipt


def write_current_epoch_receipt(directory: Path, source: Path) -> Path:
    path = directory / source.name
    path.write_text(
        json.dumps(current_epoch_receipt(source), indent=2) + "\n",
        encoding="utf-8",
    )
    return path


def current_epoch_receipt_file(test: unittest.TestCase, source: Path) -> Path:
    """A current-epoch copy of ``source`` that lives as long as ``test``."""
    directory = tempfile.TemporaryDirectory(prefix="uml-cts-receipt-")
    test.addCleanup(directory.cleanup)
    return write_current_epoch_receipt(Path(directory.name), source)


def vulkan_receipt(test: unittest.TestCase) -> dict[str, Any]:
    """The validated current-epoch copy of the accepted Vulkan receipt."""
    return receipt_contract.validate_receipt(
        current_epoch_receipt_file(test, VULKAN_RECEIPT)
    )


def bind_vulkan_receipt(runner: Any) -> Path:
    """Point ``runner.RECEIPT_PATH`` at a current-epoch copy for this module.

    Call it from ``setUpModule``: the runner then validates, hashes, and
    records the copy wherever it would use the accepted receipt.
    """
    directory = tempfile.TemporaryDirectory(prefix="uml-cts-receipt-")
    unittest.addModuleCleanup(directory.cleanup)
    path = write_current_epoch_receipt(Path(directory.name), VULKAN_RECEIPT)
    patcher = mock.patch.object(runner, "RECEIPT_PATH", path)
    patcher.start()
    unittest.addModuleCleanup(patcher.stop)
    return path
