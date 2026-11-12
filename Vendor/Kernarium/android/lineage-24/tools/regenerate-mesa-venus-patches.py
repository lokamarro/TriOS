#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate the Mesa Venus Android port series from its maintained topic.

The topic checkout is the materialized `external/mesa3d-venus` tree: the pinned
upstream Mesa revision, the replayed guest Vulkan series, then the Android port
commits on `MESA_VENUS_PATCH_BRANCH`. Only the Android commits are generated
here; the guest series stays owned by `tools/rootfs/vulkan/`.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from pathlib import Path

from pinned_source import RegenerationError, regenerate, run


def guest_base(topic: Path, metadata: dict[str, str]) -> str:
    """Find the replayed guest Mesa commit the Android port sits on."""
    guest_tree = metadata["MESA_VENUS_GUEST_TREE"]
    commit = run("git", "rev-parse", "HEAD", cwd=topic, error=RegenerationError)
    for _ in range(64):
        if run(
            "git", "rev-parse", f"{commit}^{{tree}}", cwd=topic, error=RegenerationError
        ) == guest_tree:
            return commit
        parents = run(
            "git", "rev-list", "--parents", "-n", "1", commit, cwd=topic,
            error=RegenerationError,
        ).split()
        if len(parents) != 2:
            break
        commit = parents[1]
    raise RegenerationError(
        "the topic does not contain the pinned guest Mesa tree; replay the port "
        "from the pinned upstream revision and the guest Vulkan series first"
    )


if __name__ == "__main__":
    regenerate(
        tool="regenerate-mesa-venus-patches.py",
        description="Regenerate the Mesa Venus Android port patch series",
        label="Mesa Venus",
        subject="the Mesa Venus Android port",
        directory="mesa-venus",
        prefix="MESA_VENUS",
        find_base=guest_base,
    )
