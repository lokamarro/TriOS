#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pin the layered kernel-series contract.

Every maintained line's series files are well formed. On a synthetic line a
replay reproduces the recorded tree, verification rejects histories the
series do not describe, and a change below an overlay reaches it only
through an explicit refresh.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "kernel/common/tooling"))

import kernel_series  # noqa: E402
import patch_series  # noqa: E402

SeriesError = patch_series.SeriesError
LINES = (
    "kernel/mainline-6.18/patches/metadata.env",
    "kernel/mainline-6.18/slices/x86_64/patches/metadata.env",
)


def git(repository: Path, *arguments: str) -> str:
    # Fixture commits are made as the caller, like the replays under test.
    environment = patch_series.isolated_environment()
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"git {' '.join(arguments)}: {result.stderr.decode(errors='replace')}"
        )
    return result.stdout.decode().strip()


def commit(repository: Path, subject: str, files: dict[str, str]) -> str:
    for relative, content in files.items():
        (repository / relative).write_text(content)
    git(repository, "add", "-A")
    git(repository, "commit", "-q", "-m", subject)
    return git(repository, "rev-parse", "HEAD")


def expect_error(label: str, fragment: str, action) -> None:
    try:
        action()
    except SeriesError as error:
        if fragment not in str(error):
            raise AssertionError(f"{label}: unexpected error: {error}") from error
        return
    raise AssertionError(f"{label}: accepted")


def write_line(path: Path, base: str | None, series: list[tuple[Path, str]],
               owned: list[Path], final_tree: str) -> kernel_series.Line:
    entries = " ".join(f"{directory}:{branch}" for directory, branch in series)
    path.write_text(
        "UML_KERNEL_SERIES_SCHEMA=4\n"
        + (f"UML_KERNEL_UPSTREAM_COMMIT='{base}'\n" if base else "")
        + f"UML_KERNEL_SERIES='{entries}'\n"
        f"UML_KERNEL_OWNED_SERIES='{' '.join(str(item) for item in owned)}'\n"
        f"UML_KERNEL_FINAL_TREE='{final_tree}'\n"
    )
    return kernel_series.load_line(path)


def main() -> int:
    for relative in LINES:
        kernel_series.check(kernel_series.load_line(ROOT / relative))
    scratch = Path(tempfile.mkdtemp(prefix="uml-kernel-series."))
    try:
        run(scratch)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print("kernel series contract passed")
    return 0


def run(scratch: Path) -> None:
    source = scratch / "source"
    source.mkdir()
    git(source, "init", "-q", "-b", "upstream")
    base = commit(source, "upstream", {
        "core.c": "".join(f"core {index}\n" for index in range(30)),
        "arch.c": "".join(f"arch {index}\n" for index in range(30)),
    })
    git(source, "switch", "-q", "-c", "common")
    commit(source, "core: first", {
        "core.c": "core first\n" + "".join(f"core {index}\n" for index in range(1, 30)),
    })
    commit(source, "core: second", {"extra.c": "extra\n"})
    git(source, "switch", "-q", "-c", "overlay")
    commit(source, "arch: overlay", {
        "arch.c": "".join(f"arch {index}\n" for index in range(29)) + "arch overlay\n",
    })

    common_dir, overlay_dir = scratch / "common", scratch / "overlay"
    common_dir.mkdir()
    overlay_dir.mkdir()
    common_tree = patch_series.export_series(source, base, "common", common_dir)
    overlay_tree = patch_series.export_series(source, "common", "overlay", overlay_dir)
    common_line = write_line(scratch / "common.env", base, [(common_dir, "common")],
                             [common_dir], common_tree)
    overlay_line = write_line(scratch / "overlay.env", base,
                              [(common_dir, "common"), (overlay_dir, "overlay")],
                              [overlay_dir], overlay_tree)
    assert [layer.owned for layer in overlay_line.layers] == [False, True]
    assert kernel_series.line_tree(overlay_line, source) == overlay_tree
    kernel_series.verify_branch(overlay_line, source, "overlay")
    count, digest = kernel_series.provenance(overlay_line)
    assert count == 3 and len(digest) == 64

    # Metadata that cannot describe a layered line is refused.
    expect_error("owned prefix", "trailing series", lambda: write_line(
        scratch / "bad.env", base, [(common_dir, "common"), (overlay_dir, "overlay")],
        [common_dir], overlay_tree))
    expect_error("shared branch", "same branch", lambda: write_line(
        scratch / "bad.env", base, [(common_dir, "same"), (overlay_dir, "same")],
        [overlay_dir], overlay_tree))

    # An overlay of the line whose metadata sits beside its last inherited
    # series takes that line's upstream identity and restates none of it.
    parent_dir = scratch / "parent/series"
    parent_dir.mkdir(parents=True)
    write_line(scratch / "parent/metadata.env", base, [(parent_dir, "parent")],
               [parent_dir], common_tree)
    inheriting = [(parent_dir, "parent"), (overlay_dir, "overlay")]
    assert write_line(scratch / "inheriting.env", None, inheriting,
                      [overlay_dir], overlay_tree).base == base
    expect_error("restated identity", "restates UML_KERNEL_UPSTREAM_COMMIT", lambda: write_line(
        scratch / "bad.env", base, inheriting, [overlay_dir], overlay_tree))
    expect_error("foreign series", "not the series of", lambda: write_line(
        scratch / "bad.env", None, [(parent_dir, "other"), (overlay_dir, "overlay")],
        [overlay_dir], overlay_tree))

    # A replay from the upstream commit reproduces the tree and marks each
    # layer's branch; a detached replay leaves the branches alone.
    replay = scratch / "replay"
    git(scratch, "clone", "-q", "--no-hardlinks", str(source), str(replay))
    git(replay, "switch", "-q", "--detach", base)
    git(replay, "branch", "-q", "-D", "overlay")
    assert kernel_series.replay(overlay_line, replay) == overlay_tree
    git(replay, "switch", "-q", "-c", "overlay")
    kernel_series.verify_branch(overlay_line, replay, "overlay")
    assert git(replay, "rev-parse", "common") == git(replay, "rev-parse", "overlay~1")
    assert git(replay, "config", "rerere.enabled") == "true"
    detached = scratch / "detached"
    git(scratch, "clone", "-q", "--no-hardlinks", str(source), str(detached))
    git(detached, "switch", "-q", "--detach", base)
    kernel_series.replay(overlay_line, detached, detached=True)
    kernel_series.verify_branch(overlay_line, detached, "HEAD", detached=True)
    assert git(detached, "for-each-ref", "refs/heads/common") == ""

    # Histories the series do not describe are rejected even at the right tree.
    squashed = git(source, "commit-tree", f"{common_tree}", "-p", base, "-m", "squashed")
    git(source, "branch", "squashed-overlay", squashed)
    git(source, "switch", "-q", "squashed-overlay")
    git(source, "cherry-pick", "overlay")
    expect_error("squashed common", "the series have 3",
                 lambda: kernel_series.verify_branch(overlay_line, source, "squashed-overlay"))
    git(source, "switch", "-q", "overlay")
    git(replay, "branch", "-f", "common", "common~1")
    expect_error("moved layer branch", "does not end",
                 lambda: kernel_series.verify_branch(overlay_line, replay, "overlay"))
    git(replay, "branch", "-f", "common", "overlay~1")
    # A replay made before its series was split gains the missing branch.
    git(replay, "branch", "-D", "common")
    kernel_series.verify_branch(overlay_line, replay, "overlay")
    assert git(replay, "for-each-ref", "refs/heads/common") == ""
    kernel_series.verify_branch(overlay_line, replay, "overlay", mark=True)
    assert git(replay, "rev-parse", "common") == git(replay, "rev-parse", "overlay~1")

    # Amending the overlay regenerates only the overlay and its final tree.
    git(source, "commit", "-q", "--amend", "-m", "arch: overlay, amended")
    common_before = {entry.name: entry.read_bytes() for entry in common_dir.iterdir()}
    overlay_tree = kernel_series.regenerate(overlay_line, source)
    overlay_line = kernel_series.load_line(scratch / "overlay.env")
    assert overlay_line.final_tree == overlay_tree
    assert {entry.name: entry.read_bytes() for entry in common_dir.iterdir()} == common_before
    assert "arch-overlay-amended.patch" in patch_series.read_series(overlay_dir).names

    # A change below the overlay is visible only after a refresh rebases it.
    # Here the common branch moved under the overlay in the same checkout.
    git(source, "switch", "-q", "common")
    (source / "extra.c").write_text("extra, amended\n")
    git(source, "commit", "-q", "--amend", "-a", "-m", "core: second, amended")
    kernel_series.regenerate(common_line, source)
    expect_error("overlay left behind", "is not based on",
                 lambda: kernel_series.regenerate(overlay_line, source))
    overlay_patch = (overlay_dir / "arch-overlay-amended.patch").read_bytes()
    refreshed = kernel_series.refresh(overlay_line, source)
    assert git(source, "branch", "--show-current") == "overlay"
    assert refreshed == kernel_series.regenerate(overlay_line, source)
    overlay_line = kernel_series.load_line(scratch / "overlay.env")
    assert (overlay_dir / "arch-overlay-amended.patch").read_bytes() == overlay_patch
    kernel_series.verify_branch(overlay_line, source, "overlay")
    assert kernel_series.provenance(overlay_line)[1] != digest

    # Another checkout's common branch still ends at the old topic, so the
    # refresh replays the checked-in common series under its overlay.
    expect_error("stale inherited branch", "run refresh first",
                 lambda: kernel_series.regenerate(overlay_line, replay))
    assert kernel_series.refresh(overlay_line, replay) == overlay_line.final_tree
    assert git(replay, "rev-parse", "common^{tree}") == git(source, "rev-parse", "common^{tree}")

    # Without a reflog there is no record of where the overlay left.
    git(source, "switch", "-q", "common")
    (source / "extra.c").write_text("extra, amended twice\n")
    git(source, "commit", "-q", "--amend", "-a", "-m", "core: second, amended twice")
    git(source, "reflog", "expire", "--expire=all", "--all")
    expect_error("no fork point", "cannot tell where",
                 lambda: kernel_series.refresh(overlay_line, source))


if __name__ == "__main__":
    raise SystemExit(main())
