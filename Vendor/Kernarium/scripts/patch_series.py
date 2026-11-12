#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generated Git mail-patch series shared by every patch-managed source.

A series directory holds ``series.tsv`` and the ``.patch`` files it lists in
application order. Every byte in it is produced by :func:`export_series` from
a clean topic branch, so a series is edited by amending that branch and
exporting again, never by editing the directory.

The export is stable: a patch file changes only when its own commit changes.
Mail headers carry no commit ID and no position (``--zero-commit`` and
``--no-numbered``), file names carry no ordinal, and index lines name full
blob IDs rather than a configuration-dependent abbreviation. Inserting,
dropping, or reordering one topic therefore rewrites that topic's file and a
line of ``series.tsv`` and nothing else.

A series is identified by the tree it produces from its base. An owner either
records that tree as the optional ``tree`` row of ``series.tsv`` or keeps it
in its own metadata. :func:`series_tree` recomputes it without a checkout, and
``export --check`` proves that a replay reproduces the checked-in bytes.

A patch's only identity is the author of its topic commit: ``git am`` takes the
author from each patch and a rebase keeps it, so a replay's committer never
reaches a patch file. Export therefore refuses a topic whose author is a
placeholder or one Git invented from the account and host names, and leaves
fixing it to the person who owns the topic.

A topic message may carry a ``Claude-Session:`` trailer, a link to a private
coding session that describes no change. Export drops that line from the
patch it writes, so a replay that still has it cannot bring it back; the
topic's tree, and so the series' identity, do not depend on a message.
"""

from __future__ import annotations

import argparse
import functools
import hashlib
import os
import re
import sys
from pathlib import Path
from typing import NamedTuple

sys.dont_write_bytecode = True

SERIES_FILE = "series.tsv"
DEFAULT_LICENSE = "GPL-2.0-or-later"
ZERO_FROM = b"From 0000000000000000000000000000000000000000 Mon Sep 17 00:00:00 2001\n"
# A trailer line that links a private coding session; export leaves it out.
SESSION_TRAILER = b"Claude-Session:"
PATCH_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]*\.patch$")
OBJECT_ID = re.compile(r"^[0-9a-f]{40}$")
FILENAME_MAX_LENGTH = 80
IDENT = re.compile(r"^(?P<name>.*) <(?P<email>[^>]*)> [^ ]+ [^ ]+$")
# Domains that name no mailbox: the reserved example and test names, and the
# host-local names Git builds an email from when none is configured. An author
# email in or under one of them is a placeholder, as is any "(none)" Git
# appends to a host name it cannot qualify.
PLACEHOLDER_DOMAINS = (
    "localhost",
    "local",
    "localdomain",
    "invalid",
    "example.com",
    "example.org",
    "example.net",
)

# Options that fix every formatting choice Git would otherwise take from the
# caller's configuration, so the same commits export to the same bytes for
# every developer. The global and system configuration are also suppressed.
FORMAT_PATCH_OPTIONS = (
    "--full-index",
    "--binary",
    "--no-numbered",
    "--zero-commit",
    "--no-signature",
    "--no-thread",
    "--no-cover-letter",
    "--find-renames",
    "--diff-algorithm=myers",
    f"--filename-max-length={FILENAME_MAX_LENGTH}",
)
AM_OPTIONS = (
    "--keep-cr",
    "--no-3way",
    "--no-scissors",
    "--no-message-id",
    "--quiet",
)


# Replays never run hooks.
GIT_OPTIONS = ("-c", "core.hooksPath=" + os.devnull)


class SeriesError(Exception):
    pass


# subprocess, shutil, and tempfile load only where Git or a scratch directory is
# used: the series checks that dependency preparation runs for every manifest
# row read files alone, and the imports are most of their start-up.
class Series(NamedTuple):
    directory: Path
    tree: str | None
    names: tuple[str, ...]

    @property
    def patches(self) -> tuple[Path, ...]:
        return tuple(self.directory / name for name in self.names)


@functools.cache
def _caller_identity() -> tuple[tuple[str, str], ...]:
    """Return the author and committer Git resolves for the caller.

    The isolated environment hides the caller's configuration, which is also
    where Git finds its identity. Commits made there (replayed, rebased, or a
    fixture's) carry forward whatever Git resolved outside it; when Git
    resolves nothing, nothing is carried and Git reports it.
    """
    import subprocess

    identity: list[tuple[str, str]] = []
    for role in ("AUTHOR", "COMMITTER"):
        result = subprocess.run(
            ["git", "var", f"GIT_{role}_IDENT"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        match = IDENT.match(result.stdout.decode(errors="replace").strip())
        if result.returncode == 0 and match is not None:
            identity.append((f"GIT_{role}_NAME", match.group("name")))
            identity.append((f"GIT_{role}_EMAIL", match.group("email")))
    return tuple(identity)


def isolated_environment(extra: dict[str, str] | None = None) -> dict[str, str]:
    environment = dict(os.environ)
    environment.update(_caller_identity())
    environment["GIT_CONFIG_NOSYSTEM"] = "1"
    environment["GIT_CONFIG_GLOBAL"] = os.devnull
    environment.pop("GIT_INDEX_FILE", None)
    if extra:
        environment.update(extra)
    return environment


def git(
    repository: Path,
    *arguments: str,
    environment: dict[str, str] | None = None,
    check: bool = True,
    error: str | None = None,
) -> str:
    """Run Git and return its standard output.

    A failure raises ``error``, or by default the command, with Git's output.
    """
    import subprocess

    result = subprocess.run(
        ["git", "-C", str(repository), *GIT_OPTIONS, *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment if environment is not None else isolated_environment(),
        check=False,
    )
    if check and result.returncode != 0:
        detail = (result.stdout + result.stderr).decode(errors="replace").strip()
        message = error or f"git {' '.join(arguments)} failed"
        raise SeriesError(f"{message}: {detail}" if detail else message)
    return result.stdout.decode(errors="strict")


def resolve_commit(repository: Path, revision: str) -> str:
    commit = git(repository, "rev-parse", "--verify", "--quiet", f"{revision}^{{commit}}",
                 check=False).strip()
    if not OBJECT_ID.match(commit):
        raise SeriesError(f"{repository}: {revision} is not a commit")
    return commit


def tree_of(repository: Path, revision: str) -> str:
    return git(repository, "rev-parse", f"{revision}^{{tree}}").strip()


def read_series(directory: Path) -> Series:
    """Parse and statically validate one series directory."""
    if directory.is_symlink() or not directory.is_dir():
        raise SeriesError(f"series directory is not a real directory: {directory}")
    path = directory / SERIES_FILE
    if path.is_symlink() or not path.is_file():
        raise SeriesError(f"series file is missing: {path}")
    tree: str | None = None
    names: list[str] = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 2:
            raise SeriesError(f"{path}:{number}: expected 'kind<TAB>value'")
        kind, value = fields
        if kind == "tree":
            if tree is not None or names:
                raise SeriesError(f"{path}:{number}: the tree row must be first and unique")
            if not OBJECT_ID.match(value):
                raise SeriesError(f"{path}:{number}: invalid tree object ID")
            tree = value
        elif kind == "patch":
            if not PATCH_NAME.match(value):
                raise SeriesError(f"{path}:{number}: unsafe patch file name: {value}")
            if value in names:
                raise SeriesError(f"{path}:{number}: duplicate patch: {value}")
            names.append(value)
        else:
            raise SeriesError(f"{path}:{number}: unknown row kind: {kind}")
    present = sorted(
        entry.name for entry in directory.iterdir() if entry.name.endswith(".patch")
    )
    if sorted(names) != present:
        missing = sorted(set(names) - set(present))
        unlisted = sorted(set(present) - set(names))
        raise SeriesError(
            f"{directory}: patch files differ from {SERIES_FILE}; "
            f"missing={missing} unlisted={unlisted}"
        )
    for name in names:
        patch = directory / name
        if patch.is_symlink() or not patch.is_file():
            raise SeriesError(f"patch is not a regular file: {patch}")
        with patch.open("rb") as stream:
            if stream.readline() != ZERO_FROM:
                raise SeriesError(f"patch was not exported by {Path(__file__).name}: {patch}")
    return Series(directory, tree, tuple(names))


def series_provenance(directories: list[Path]) -> tuple[int, str]:
    """Return the patch count and a SHA-256 over the ordered patch contents.

    Release manifests record this so a published artifact names the exact
    patch stack that produced it.
    """
    digest = hashlib.sha256()
    count = 0
    for directory in directories:
        series = read_series(directory)
        for name in series.names:
            content = hashlib.sha256((directory / name).read_bytes()).hexdigest()
            digest.update(f"{name}\t{content}\n".encode())
            count += 1
    return count, digest.hexdigest()


def series_tree(repository: Path, base: str, directories: list[Path]) -> str:
    """Return the tree the series produce on ``base``, without a checkout."""
    import tempfile

    base_commit = resolve_commit(repository, base)
    with tempfile.TemporaryDirectory(prefix="uml-series-index.") as scratch:
        environment = isolated_environment({"GIT_INDEX_FILE": str(Path(scratch) / "index")})
        git(repository, "read-tree", base_commit, environment=environment)
        for directory in directories:
            for patch in read_series(directory).patches:
                git(repository, "apply", "--cached", "--whitespace=nowarn", str(patch),
                    environment=environment, error=f"{patch.name} does not apply")
        return git(repository, "write-tree", environment=environment).strip()


def apply_series(
    repository: Path, directories: list[Path], *, expected_tree: str | None = None
) -> str:
    """Commit every patch onto the clean checkout's HEAD with ``git am``."""
    if git(repository, "status", "--porcelain", "--untracked-files=all").strip():
        raise SeriesError(f"{repository}: checkout is not clean")
    patches = [str(patch) for directory in directories for patch in read_series(directory).patches]
    if patches:
        try:
            git(repository, "am", *AM_OPTIONS, *patches,
                error=f"{repository}: series does not apply")
        except SeriesError:
            git(repository, "am", "--abort", check=False)
            raise
    tree = tree_of(repository, "HEAD")
    if expected_tree is not None and tree != expected_tree:
        raise SeriesError(f"{repository}: replayed tree is {tree}, expected {expected_tree}")
    return tree


def placeholder_email(email: str) -> bool:
    """Whether an author email is a placeholder or a host-derived default."""
    address = email.strip().lower()
    if "(none)" in address or "@" not in address:
        return True
    domain = address.rsplit("@", 1)[1]
    return not domain or any(
        domain == reserved or domain.endswith("." + reserved)
        for reserved in PLACEHOLDER_DOMAINS
    )


def _check_authors(source: Path, base: str, head: str) -> None:
    log = git(source, "log", "--reverse", "--format=%H%x00%an%x00%ae%x00%s",
              f"{base}..{head}")
    for line in log.splitlines():
        commit, name, email, subject = line.split("\0", 3)
        if placeholder_email(email):
            raise SeriesError(
                f"{source}: topic commit {commit[:12]} ({subject}) has the placeholder "
                f"author {name} <{email}>; the person who owns the topic must fix it "
                "with 'git commit --amend --reset-author' (or --author) and export "
                "again. Tooling never rewrites authors."
            )


def _series_text(license_id: str, tree: str | None, names: list[str]) -> str:
    lines = [
        f"# SPDX-License-Identifier: {license_id}",
        "# Generated by scripts/patch_series.py export. Amend the topic branch and",
        "# export again; never edit this directory by hand.",
    ]
    if tree is not None:
        lines.append(f"tree\t{tree}")
    lines.extend(f"patch\t{name}" for name in names)
    return "\n".join(lines) + "\n"


def _existing_license(directory: Path) -> str | None:
    path = directory / SERIES_FILE
    if not path.is_file():
        return None
    first = path.read_text(encoding="utf-8").splitlines()[:1]
    prefix = "# SPDX-License-Identifier: "
    if first and first[0].startswith(prefix):
        return first[0][len(prefix):].strip()
    return None


def drop_session_trailers(patch: bytes) -> bytes:
    """The patch without the session trailer lines of its message.

    The message ends at the last ``---`` line before the first diff, which is
    where Git writes the separator. Nothing after it is touched, and a blank
    line the trailer leaves at the end of the message goes with it.
    """
    diff = patch.find(b"\ndiff --git ")
    head = patch if diff < 0 else patch[:diff]
    end = head.rfind(b"\n---\n")
    if end < 0:
        return patch
    lines = patch[:end + 1].splitlines(keepends=True)
    kept = [line for line in lines if not line.startswith(SESSION_TRAILER)]
    if len(kept) == len(lines):
        return patch
    message = b"".join(kept)
    while message.endswith(b"\n\n"):
        message = message[:-1]
    return message + patch[end + 1:]


def _format(source: Path, base: str, head: str, output: Path) -> list[str]:
    raw = output / "raw"
    raw.mkdir()
    emitted = git(source, "format-patch", *FORMAT_PATCH_OPTIONS,
                  "--output-directory", str(raw), f"{base}..{head}")
    names: list[str] = []
    for line in emitted.splitlines():
        if not line.strip():
            continue
        path = Path(line)
        if path.parent.resolve() != raw.resolve() or not path.is_file():
            raise SeriesError(f"git format-patch emitted an unexpected path: {line}")
        # The ordinal format-patch prepends is position, which series.tsv owns.
        name = re.sub(r"^[0-9]+-", "", path.name)
        if not PATCH_NAME.match(name):
            raise SeriesError(f"generated patch name is unsafe: {name}")
        if name in names:
            raise SeriesError(
                f"two topics export to {name}; give one of them a distinct subject"
            )
        data = path.read_bytes()
        cleaned = drop_session_trailers(data)
        if cleaned != data:
            path.write_bytes(cleaned)
        path.rename(output / name)
        names.append(name)
    raw.rmdir()
    return names


def export_series(
    source: Path,
    base: str,
    head: str,
    directory: Path,
    *,
    record_tree: bool = False,
    license_id: str | None = None,
    check: bool = False,
) -> str:
    """Write, or with ``check`` compare, the series for ``base..head``."""
    import shutil
    import tempfile

    if git(source, "status", "--porcelain", "--untracked-files=all").strip():
        raise SeriesError(f"{source}: topic checkout is not clean")
    base_commit = resolve_commit(source, base)
    head_commit = resolve_commit(source, head)
    git(source, "merge-base", "--is-ancestor", base_commit, head_commit,
        error=f"{source}: {head} is not based on {base}")
    if git(source, "rev-list", "--merges", f"{base_commit}..{head_commit}").strip():
        raise SeriesError(f"{source}: {base}..{head} contains a merge")
    commits = git(source, "rev-list", "--reverse", f"{base_commit}..{head_commit}").split()
    for commit in commits:
        if tree_of(source, commit) == tree_of(source, f"{commit}^"):
            subject = git(source, "show", "-s", "--format=%s", commit).strip()
            raise SeriesError(f"{source}: topic commit changes nothing: {subject}")
    _check_authors(source, base_commit, head_commit)
    if directory.is_symlink() or not directory.is_dir():
        raise SeriesError(f"series directory is not a real directory: {directory}")
    license_id = license_id or _existing_license(directory) or DEFAULT_LICENSE
    tree = tree_of(source, head_commit)

    # A check never writes below the checked-in directory; an export stages
    # beside it so the final renames stay on one filesystem.
    staging = Path(tempfile.mkdtemp(prefix=".export.", dir=None if check else directory))
    try:
        names = _format(source, base_commit, head_commit, staging)
        if len(names) != len(commits):
            raise SeriesError(f"git format-patch emitted {len(names)} files for {len(commits)} commits")
        (staging / SERIES_FILE).write_text(
            _series_text(license_id, tree if record_tree else None, names), encoding="utf-8"
        )
        exported = read_series(staging)
        produced = series_tree(source, base_commit, [staging])
        if produced != tree:
            raise SeriesError(f"exported series produces {produced}, expected {tree}")
        if check:
            differences = _differences(directory, staging, exported)
            if differences:
                raise SeriesError(
                    f"{directory} differs from its topic branch: " + ", ".join(differences)
                )
            return tree
        for entry in directory.iterdir():
            if entry.name.endswith(".patch") or entry.name == SERIES_FILE:
                entry.unlink()
        for name in (*names, SERIES_FILE):
            (staging / name).rename(directory / name)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return tree


def _differences(directory: Path, staging: Path, exported: Series) -> list[str]:
    current = {entry.name for entry in directory.iterdir()
               if entry.name.endswith(".patch") or entry.name == SERIES_FILE}
    wanted = set(exported.names) | {SERIES_FILE}
    differences = [f"unexpected {name}" for name in sorted(current - wanted)]
    differences += [f"missing {name}" for name in sorted(wanted - current)]
    for name in sorted(current & wanted):
        if (directory / name).read_bytes() != (staging / name).read_bytes():
            differences.append(f"changed {name}")
    return differences


def _path(text: str) -> Path:
    """A command-line path, made absolute because Git runs elsewhere."""
    return Path(text).absolute()


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)

    export = commands.add_parser("export", help="write a series from base..head")
    export.add_argument("--source", type=_path, required=True)
    export.add_argument("--base", required=True)
    export.add_argument("--head", default="HEAD")
    export.add_argument("--series-dir", type=_path, required=True)
    export.add_argument("--tree", action="store_true", help="record the tree row")
    export.add_argument("--license", dest="license_id")
    export.add_argument("--check", action="store_true",
                        help="compare with the checked-in series instead of writing")

    for name, text in (("tree", "print the tree the series produce, without a checkout"),
                       ("verify", "check series files and, with --repo, their tree")):
        command = commands.add_parser(name, help=text)
        command.add_argument("--series-dir", type=_path, action="append", required=True)
        command.add_argument("--repo", type=_path, required=name == "tree")
        command.add_argument("--base", required=name == "tree")
        if name == "verify":
            command.add_argument("--expect-tree")

    apply = commands.add_parser("apply", help="git am the series onto a clean checkout")
    apply.add_argument("--repo", type=_path, required=True)
    apply.add_argument("--series-dir", type=_path, action="append", required=True)
    apply.add_argument("--expect-tree")

    recorded = commands.add_parser(
        "recorded-tree", help="check series files and print the last one's tree row")
    recorded.add_argument("--series-dir", type=_path, action="append", required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        if arguments.command == "export":
            print(export_series(
                arguments.source, arguments.base, arguments.head, arguments.series_dir,
                record_tree=arguments.tree, license_id=arguments.license_id,
                check=arguments.check,
            ))
        elif arguments.command == "tree":
            print(series_tree(arguments.repo, arguments.base, arguments.series_dir))
        elif arguments.command == "verify":
            series = [read_series(directory) for directory in arguments.series_dir]
            if arguments.repo is not None or arguments.base is not None:
                if arguments.repo is None or arguments.base is None:
                    raise SeriesError("verify needs both --repo and --base to check a tree")
                expected = arguments.expect_tree or series[-1].tree
                if expected is None:
                    raise SeriesError("no --expect-tree and no recorded tree row")
                produced = series_tree(arguments.repo, arguments.base, arguments.series_dir)
                if produced != expected:
                    raise SeriesError(f"series produce {produced}, expected {expected}")
        elif arguments.command == "apply":
            print(apply_series(arguments.repo, arguments.series_dir,
                               expected_tree=arguments.expect_tree))
        elif arguments.command == "recorded-tree":
            series = [read_series(directory) for directory in arguments.series_dir]
            if series[-1].tree is None:
                raise SeriesError(f"{arguments.series_dir[-1]} records no tree")
            print(series[-1].tree)
    except SeriesError as error:
        print(f"patch_series.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
