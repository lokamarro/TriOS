#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Replay, verify, and regenerate the generated series of one kernel line.

A line's ``metadata.env`` names its pinned upstream commit, the ordered
generated series that turn that commit into the line's tree, and that tree:

    UML_KERNEL_SERIES='kernel/common/patches:codex/linux-6.18-common
                       kernel/mainline-6.18/patches/series:codex/linux-6.18-darwin'
    UML_KERNEL_OWNED_SERIES='kernel/common/patches kernel/mainline-6.18/patches/series'
    UML_KERNEL_FINAL_TREE='<tree>'

Each ``UML_KERNEL_SERIES`` entry pairs a series directory with the managed
replay branch that ends it. A line regenerates only the series it owns; a
series it does not own belongs to another line and is checked by the tree it
produces here, because the same topics exported from a different upstream base
carry different context and index lines.

A line whose last inherited series has a ``metadata.env`` beside it is an
overlay of that line, such as the mainline x86_64 slice. Its inherited series
are exactly that line's series, and it takes that line's upstream identity
(``IDENTITY``) instead of naming its own.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "kernel/common/artifacts"))

import patch_series  # noqa: E402
from contract_checks import parse_assignments, replace_assignment  # noqa: E402
from patch_series import SeriesError, git  # noqa: E402

BRANCH = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]*$")
# The pinned upstream source a line's series apply to.
IDENTITY = ("UML_KERNEL_RELEASE_VERSION", "UML_KERNEL_UPSTREAM_REPOSITORY",
            "UML_KERNEL_UPSTREAM_BRANCH", "UML_KERNEL_UPSTREAM_COMMIT",
            "UML_KERNEL_UPSTREAM_TREE", "UML_KERNEL_BASE_REF")


@dataclass(frozen=True)
class Layer:
    relative: str
    branch: str
    owned: bool

    @property
    def directory(self) -> Path:
        return ROOT / self.relative


@dataclass(frozen=True)
class Line:
    metadata: Path
    values: dict[str, str]
    base: str
    layers: tuple[Layer, ...]

    @property
    def final_tree(self) -> str:
        return self.values["UML_KERNEL_FINAL_TREE"]

    @property
    def tip_branch(self) -> str:
        return self.layers[-1].branch

    @property
    def owned(self) -> tuple[Layer, ...]:
        return tuple(layer for layer in self.layers if layer.owned)

    @property
    def inherited(self) -> tuple[Layer, ...]:
        return tuple(layer for layer in self.layers if not layer.owned)


def _read_line(metadata: Path) -> tuple[dict[str, str], tuple[Layer, ...]]:
    values = parse_assignments(metadata, SeriesError)
    for key in ("UML_KERNEL_SERIES", "UML_KERNEL_OWNED_SERIES", "UML_KERNEL_FINAL_TREE"):
        if not values.get(key):
            raise SeriesError(f"{metadata}: {key} is not set")
    owned = values["UML_KERNEL_OWNED_SERIES"].split()
    layers: list[Layer] = []
    for entry in values["UML_KERNEL_SERIES"].split():
        relative, separator, branch = entry.partition(":")
        if not separator or not BRANCH.fullmatch(branch) or ".." in relative:
            raise SeriesError(f"{metadata}: malformed series entry: {entry}")
        layers.append(Layer(relative, branch, relative in owned))
    if not layers:
        raise SeriesError(f"{metadata}: no series")
    if len({layer.branch for layer in layers}) != len(layers):
        raise SeriesError(f"{metadata}: two series end at the same branch")
    if sorted(owned) != sorted(layer.relative for layer in layers if layer.owned):
        raise SeriesError(f"{metadata}: owned series are not all listed in UML_KERNEL_SERIES")
    flags = [layer.owned for layer in layers]
    if not flags[-1] or flags != sorted(flags):
        raise SeriesError(f"{metadata}: owned series must be the trailing series")
    return values, tuple(layers)


def _inherited_identity(metadata: Path, values: dict[str, str],
                        layers: tuple[Layer, ...]) -> dict[str, str]:
    """Return the upstream identity an overlay takes from the line it extends."""
    inherited = [layer for layer in layers if not layer.owned]
    if not inherited:
        return {}
    parent = inherited[-1].directory.parent / "metadata.env"
    if not parent.is_file():
        return {}
    restated = [key for key in IDENTITY if key in values]
    if restated:
        raise SeriesError(f"{metadata}: an overlay of {parent} restates {', '.join(restated)}")
    parent_values, parent_layers = _read_line(parent)
    if [(layer.relative, layer.branch) for layer in parent_layers] != [
            (layer.relative, layer.branch) for layer in inherited]:
        raise SeriesError(f"{metadata}: the inherited series are not the series of {parent}")
    return {key: parent_values[key] for key in IDENTITY if key in parent_values}


def load_line(metadata: Path) -> Line:
    metadata = metadata if metadata.is_absolute() else ROOT / metadata
    values, layers = _read_line(metadata)
    values = {**_inherited_identity(metadata, values, layers), **values}
    if not values.get("UML_KERNEL_UPSTREAM_COMMIT"):
        raise SeriesError(f"{metadata}: UML_KERNEL_UPSTREAM_COMMIT is not set")
    return Line(metadata, values, values["UML_KERNEL_UPSTREAM_COMMIT"], layers)


def check(line: Line) -> list[patch_series.Series]:
    return [patch_series.read_series(layer.directory) for layer in line.layers]


def provenance(line: Line) -> tuple[int, str]:
    """Return the patch count and ordered content digest of every series."""
    return patch_series.series_provenance([layer.directory for layer in line.layers])


def line_tree(line: Line, repository: Path) -> str:
    return patch_series.series_tree(
        repository, line.base, [layer.directory for layer in line.layers])


def _branch_commit(repository: Path, branch: str) -> str | None:
    result = git(repository, "rev-parse", "--verify", "--quiet",
                 f"refs/heads/{branch}^{{commit}}", check=False).strip()
    return result or None


def _is_ancestor(repository: Path, ancestor: str, descendant: str) -> bool:
    return subprocess.run(
        ["git", "-C", str(repository), "merge-base", "--is-ancestor", ancestor, descendant],
        check=False,
    ).returncode == 0


def configure_replay(repository: Path) -> None:
    """Remember conflict resolutions and carry layer branches through rebases."""
    for key, value in (("rerere.enabled", "true"), ("rerere.autoupdate", "true"),
                       ("rebase.updateRefs", "true")):
        git(repository, "config", "--local", key, value)


def replay(line: Line, repository: Path, *, after: str | None = None,
           detached: bool = False) -> str:
    """Apply the line's series onto the checkout's HEAD.

    Without ``after`` the checkout must sit at the upstream base and every
    series is applied. With ``after`` it must sit at that series' branch, and
    only the series after it are applied. Each applied series except the last
    moves its branch to the commit that ends it, unless ``detached`` asks for
    a replay that leaves the repository's branches alone.
    """
    configure_replay(repository)
    layers = list(line.layers)
    if after is not None:
        names = [layer.branch for layer in layers]
        if after not in names:
            raise SeriesError(f"{after} does not end a series of this line")
        layers = layers[names.index(after) + 1:]
    for index, layer in enumerate(layers):
        patch_series.apply_series(repository, [layer.directory])
        if index != len(layers) - 1 and not detached:
            git(repository, "branch", "--force", layer.branch, "HEAD")
    tree = patch_series.tree_of(repository, "HEAD")
    if tree != line.final_tree:
        raise SeriesError(f"replayed tree is {tree}, expected {line.final_tree}")
    return tree


def _boundaries(line: Line, repository: Path, tip: str) -> list[str]:
    """Return the commit that ends each series, counting back from ``tip``."""
    counts = [len(series.names) for series in check(line)]
    commits = git(repository, "rev-list", "--reverse",
                  f"{line.base}..{tip}").split()
    if len(commits) != sum(counts):
        raise SeriesError(f"{tip} has {len(commits)} commits; the series have {sum(counts)}")
    boundaries: list[str] = []
    position = 0
    for count in counts:
        position += count
        boundaries.append(commits[position - 1] if position else line.base)
    return boundaries


def verify_branch(line: Line, repository: Path, tip: str, *,
                  detached: bool = False, mark: bool = False) -> None:
    """Prove that ``tip`` is exactly the line's series replayed on its base.

    Series branches that exist must end their series too, unless ``detached``
    says the replay was made without them. ``mark`` creates the missing ones
    at the commits just verified, so a replay made before a series was split
    can be regenerated.
    """
    tip_commit = patch_series.resolve_commit(repository, tip)
    if not _is_ancestor(repository, line.base, tip_commit):
        raise SeriesError(f"{tip} is not based on {line.base}")
    if git(repository, "rev-list", "--merges", f"{line.base}..{tip_commit}").strip():
        raise SeriesError(f"{tip} contains a merge")
    tree = patch_series.tree_of(repository, tip_commit)
    if tree != line.final_tree:
        raise SeriesError(f"{tip} has tree {tree}, expected {line.final_tree}")
    boundaries = _boundaries(line, repository, tip_commit)
    start = line.base
    for index, (layer, end) in enumerate(zip(line.layers, boundaries)):
        if layer.owned:
            patch_series.export_series(repository, start, end, layer.directory, check=True)
        else:
            expected = patch_series.series_tree(
                repository, line.base, [item.directory for item in line.layers[:index + 1]])
            if patch_series.tree_of(repository, end) != expected:
                raise SeriesError(f"{tip} does not reproduce {layer.relative}")
        if layer is not line.layers[-1] and not detached:
            branch = _branch_commit(repository, layer.branch)
            if branch is None and mark:
                git(repository, "branch", layer.branch, end)
            elif branch is not None and branch != end:
                raise SeriesError(f"branch {layer.branch} does not end {layer.relative}")
        start = end


def _write_final_tree(line: Line, tree: str) -> None:
    text = line.metadata.read_text(encoding="utf-8")
    updated = replace_assignment(
        text, "UML_KERNEL_FINAL_TREE", f"'{tree}'",
        lambda message: SeriesError(f"{line.metadata}: {message}"))
    line.metadata.write_text(updated, encoding="utf-8")


def regenerate(line: Line, repository: Path) -> str:
    """Export the owned series from their replay branches and record the tree."""
    start = line.base
    inherited = line.inherited
    if inherited:
        start = _branch_commit(repository, inherited[-1].branch) or ""
        if not start:
            raise SeriesError(f"branch {inherited[-1].branch} is missing")
        expected = patch_series.series_tree(
            repository, line.base, [layer.directory for layer in inherited])
        if patch_series.tree_of(repository, start) != expected:
            raise SeriesError(
                f"branch {inherited[-1].branch} no longer matches its checked-in series; "
                "run refresh first")
    tree = ""
    for layer in line.owned:
        end = _branch_commit(repository, layer.branch)
        if end is None:
            raise SeriesError(f"branch {layer.branch} is missing")
        tree = patch_series.export_series(repository, start, end, layer.directory)
        start = end
    _write_final_tree(line, tree)
    return tree


def refresh(line: Line, repository: Path) -> str:
    """Rebase the owned series onto the current inherited series.

    Inherited series are replayed from the upstream base when their branch no
    longer produces their checked-in tree. The owned commits are then rebased
    onto that branch; a conflict stops the rebase for the caller to resolve,
    and rerere records the resolution for later refreshes.
    """
    if not line.inherited:
        raise SeriesError("this line owns every series; there is nothing to refresh")
    configure_replay(repository)
    if git(repository, "status", "--porcelain", "--untracked-files=all").strip():
        raise SeriesError(f"{repository}: checkout is not clean")
    parent = line.inherited[-1]
    tip = _branch_commit(repository, line.tip_branch)
    old_parent = _branch_commit(repository, parent.branch)
    if tip is None or old_parent is None:
        raise SeriesError(f"branches {parent.branch} and {line.tip_branch} must exist")
    if not _is_ancestor(repository, old_parent, tip):
        # A lower topic was amended in this checkout and the parent branch
        # moved without the owned commits; its reflog still records where
        # they left it.
        old_parent = git(repository, "merge-base", "--fork-point", parent.branch,
                         tip, check=False).strip()
        if not old_parent:
            raise SeriesError(f"cannot tell where {line.tip_branch} left {parent.branch}; "
                              f"rebase it onto {parent.branch} and run regenerate")
    expected = patch_series.series_tree(
        repository, line.base, [layer.directory for layer in line.inherited])
    if patch_series.tree_of(repository, old_parent) != expected:
        git(repository, "checkout", "--quiet", "--detach", line.base)
        for layer in line.inherited:
            patch_series.apply_series(repository, [layer.directory])
            git(repository, "branch", "--force", layer.branch, "HEAD")
    new_parent = _branch_commit(repository, parent.branch)
    git(repository, "checkout", "--quiet", line.tip_branch)
    if new_parent != old_parent:
        result = subprocess.run(
            ["git", "-C", str(repository), *patch_series.GIT_OPTIONS,
             "rebase", "--onto", new_parent, old_parent, line.tip_branch],
            env=patch_series.isolated_environment(),
            check=False,
        )
        if result.returncode != 0:
            raise SeriesError("rebase stopped on a conflict; resolve it, continue the "
                              "rebase, and run regenerate")
    return patch_series.tree_of(repository, line.tip_branch)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("check", "tree", "replay", "verify-branch", "regenerate", "refresh"):
        command = commands.add_parser(name)
        command.add_argument("--metadata", type=Path, required=True)
        if name in ("tree", "replay", "verify-branch", "regenerate", "refresh"):
            command.add_argument("--source", type=Path, required=True)
        if name == "replay":
            command.add_argument("--after", help="series branch the checkout already ends")
        if name in ("replay", "verify-branch"):
            command.add_argument("--detached", action="store_true",
                                 help="neither create nor check series branches")
        if name == "verify-branch":
            command.add_argument("--mark", action="store_true",
                                 help="create missing series branches once verified")
            command.add_argument("--tip", required=True)
    arguments = parser.parse_args(argv)
    try:
        line = load_line(arguments.metadata)
        if arguments.command == "check":
            check(line)
        elif arguments.command == "tree":
            tree = line_tree(line, arguments.source)
            print(tree)
            if tree != line.final_tree:
                raise SeriesError(f"series produce {tree}, expected {line.final_tree}")
        elif arguments.command == "replay":
            print(replay(line, arguments.source, after=arguments.after,
                         detached=arguments.detached))
        elif arguments.command == "verify-branch":
            verify_branch(line, arguments.source, arguments.tip,
                          detached=arguments.detached, mark=arguments.mark)
        elif arguments.command == "regenerate":
            print(regenerate(line, arguments.source))
        elif arguments.command == "refresh":
            print(refresh(line, arguments.source))
    except SeriesError as error:
        print(f"kernel_series.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
