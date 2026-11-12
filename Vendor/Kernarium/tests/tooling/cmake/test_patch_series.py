#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pin the shared patch-series contract.

A series exports to the same bytes every time, changes only where its topics
change, replays with ``git am`` to the tree it records, rejects anything
that did not come from its topic branch, and refuses a placeholder author.
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

import patch_series  # noqa: E402

SeriesError = patch_series.SeriesError
TOOL = ROOT / "scripts" / "patch_series.py"


def git(repository: Path, *arguments: str, date: str | None = None) -> str:
    # Fixture commits are made as the caller, like the replays under test.
    environment = patch_series.isolated_environment()
    if date is not None:
        environment["GIT_AUTHOR_DATE"] = date
        environment["GIT_COMMITTER_DATE"] = date
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
    return result.stdout.decode()


def commit(repository: Path, subject: str, files: dict[str, bytes | None], day: int) -> str:
    for relative, content in files.items():
        path = repository / relative
        if content is None:
            path.unlink()
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    git(repository, "add", "-A")
    git(repository, "commit", "-q", "-m", subject, "-m", f"Body of {subject}.",
        date=f"2026-01-{day:02d}T12:00:00+0900")
    return git(repository, "rev-parse", "HEAD").strip()


def expect_error(label: str, fragment: str, action) -> None:
    try:
        action()
    except SeriesError as error:
        if fragment not in str(error):
            raise AssertionError(f"{label}: unexpected error: {error}") from error
        return
    raise AssertionError(f"{label}: accepted")


def snapshot(directory: Path) -> dict[str, bytes]:
    return {entry.name: entry.read_bytes() for entry in sorted(directory.iterdir())
            if entry.is_file()}


def export(source: Path, base: str, head: str, directory: Path, **options) -> str:
    directory.mkdir(parents=True, exist_ok=True)
    return patch_series.export_series(source, base, head, directory, **options)


def main() -> int:
    scratch = Path(tempfile.mkdtemp(prefix="uml-patch-series."))
    try:
        run(scratch)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print("patch-series contract passed")
    return 0


def run(scratch: Path) -> None:
    source = scratch / "source"
    source.mkdir()
    git(source, "init", "-q", "-b", "base")
    base = commit(source, "base", {
        "a.txt": b"alpha\n" * 20,
        "b.txt": b"bravo\n" * 20,
        "crlf.txt": b"one\r\ntwo\r\nthree\r\n",
        "sub/c.txt": b"".join(b"charlie %d\n" % index for index in range(40)),
    }, 1)
    git(source, "checkout", "-q", "-b", "topic")
    commit(source, "fix: change a", {"a.txt": b"alpha\n" * 19 + b"ALPHA\n"}, 2)
    commit(source, "feat: add binary", {"bin.dat": bytes(range(256)) * 4}, 3)
    commit(source, "fix: keep carriage returns", {"crlf.txt": b"one\r\nTWO\r\nthree\r\n"}, 4)
    commit(source, "refactor: move c", {
        "sub/c.txt": None,
        "sub/d.txt": b"".join(b"charlie %d\n" % index for index in range(40)) + b"delta\n",
    }, 5)
    head_tree = git(source, "rev-parse", "topic^{tree}").strip()

    # Export: slug names in order, no ordinals or commit IDs, full index lines.
    series = scratch / "series"
    tree = export(source, base, "topic", series, record_tree=True)
    assert tree == head_tree
    parsed = patch_series.read_series(series)
    assert parsed.tree == head_tree
    assert parsed.names == (
        "fix-change-a.patch",
        "feat-add-binary.patch",
        "fix-keep-carriage-returns.patch",
        "refactor-move-c.patch",
    ), parsed.names
    first = (series / "fix-change-a.patch").read_text()
    assert first.startswith(patch_series.ZERO_FROM.decode())
    assert "Subject: [PATCH] fix: change a\n" in first
    index_lines = [line for line in first.splitlines() if line.startswith("index ")]
    assert index_lines and all(len(line.split()[1]) == 82 for line in index_lines), index_lines
    assert "GIT binary patch" in (series / "feat-add-binary.patch").read_text()
    assert b"\r\n" in (series / "fix-keep-carriage-returns.patch").read_bytes()
    assert "rename from sub/c.txt" in (series / "refactor-move-c.patch").read_text()

    # Exporting unchanged commits reproduces every byte, and a check writes
    # nothing below the checked-in directory, not even transiently.
    before = snapshot(series)
    export(source, base, "topic", series, record_tree=True)
    assert snapshot(series) == before
    series.chmod(0o555)
    try:
        export(source, base, "topic", series, record_tree=True, check=True)
    finally:
        series.chmod(0o755)

    # Inserting one topic changes that topic's file and series.tsv only.
    git(source, "checkout", "-q", "-b", "inserted", base)
    git(source, "cherry-pick", "topic~3")
    commit(source, "fix: change b", {"b.txt": b"bravo\n" * 19 + b"BRAVO\n"}, 6)
    git(source, "cherry-pick", "topic~2", "topic~1", "topic")
    widened = scratch / "widened"
    shutil.copytree(series, widened)
    export(source, base, "inserted", widened, record_tree=True)
    after = snapshot(widened)
    changed = sorted(name for name in set(before) | set(after)
                     if before.get(name) != after.get(name))
    assert changed == ["fix-change-b.patch", "series.tsv"], changed
    assert patch_series.read_series(widened).names[1] == "fix-change-b.patch"

    # A replay reproduces the tree, and exporting the replay reproduces the bytes.
    replay = scratch / "replay"
    git(scratch, "clone", "-q", "--no-hardlinks", str(source), str(replay))
    git(replay, "checkout", "-q", "-b", "replayed", base)
    assert patch_series.apply_series(replay, [series], expected_tree=head_tree) == head_tree
    export(replay, base, "replayed", series, record_tree=True, check=True)
    assert patch_series.series_tree(source, base, [series]) == head_tree

    # A common series plus an overlay compose in order.
    common, overlay = scratch / "common", scratch / "overlay"
    export(source, base, "topic~2", common)
    export(source, "topic~2", "topic", overlay, record_tree=True)
    assert patch_series.read_series(common).tree is None
    assert patch_series.series_tree(source, base, [common, overlay]) == head_tree
    composed = scratch / "composed"
    git(scratch, "clone", "-q", "--no-hardlinks", str(source), str(composed))
    git(composed, "checkout", "-q", "-b", "composed", base)
    patch_series.apply_series(composed, [common, overlay], expected_tree=head_tree)

    # A series that does not apply names its first failing patch, and a failed
    # replay aborts git am and leaves the checkout where it found it.
    expect_error("tree on a conflicting base", "fix-change-a.patch does not apply",
                 lambda: patch_series.series_tree(source, "topic", [series]))
    conflict = scratch / "conflict"
    git(scratch, "clone", "-q", "--no-hardlinks", str(source), str(conflict))
    git(conflict, "checkout", "-q", "-b", "conflict", base)
    before_replay = commit(conflict, "conflict", {"crlf.txt": b"one\r\nZWEI\r\nthree\r\n"}, 12)
    expect_error("conflicting replay", "series does not apply",
                 lambda: patch_series.apply_series(conflict, [series]))
    assert git(conflict, "rev-parse", "HEAD").strip() == before_replay
    assert not git(conflict, "status", "--porcelain", "--untracked-files=all").strip()
    assert not (conflict / ".git" / "rebase-apply").exists()

    # Anything that did not come from the topic branch is rejected.
    tampered = scratch / "tampered"
    shutil.copytree(series, tampered)
    (tampered / "stray.patch").write_bytes(patch_series.ZERO_FROM)
    expect_error("unlisted patch", "unlisted=['stray.patch']",
                 lambda: patch_series.read_series(tampered))
    (tampered / "stray.patch").unlink()
    edited = tampered / "fix-change-a.patch"
    edited.write_text(edited.read_text().replace("From 0000000000", "From 1111111111", 1))
    expect_error("rewritten header", "was not exported",
                 lambda: patch_series.read_series(tampered))
    edited.write_text(first.replace("+ALPHA", "+OMEGA"))
    expect_error("hand edit", "changed fix-change-a.patch",
                 lambda: export(source, base, "topic", tampered, record_tree=True, check=True))
    assert patch_series.series_tree(source, base, [tampered]) != head_tree
    (tampered / "refactor-move-c.patch").unlink()
    expect_error("missing patch", "missing=['refactor-move-c.patch']",
                 lambda: patch_series.read_series(tampered))

    # Topic branches the series cannot represent are refused.
    git(source, "checkout", "-q", "topic")
    (source / "a.txt").write_text("dirty\n")
    expect_error("dirty checkout", "not clean",
                 lambda: export(source, base, "topic", scratch / "dirty"))
    git(source, "checkout", "-q", "--", "a.txt")
    expect_error("unrelated base", "is not based on",
                 lambda: export(source, "inserted", "topic", scratch / "unrelated"))
    git(source, "checkout", "-q", "-b", "merged", base)
    commit(source, "side", {"e.txt": b"echo\n"}, 7)
    git(source, "merge", "-q", "--no-edit", "--no-ff", "topic", date="2026-01-08T12:00:00+0900")
    expect_error("merge", "contains a merge",
                 lambda: export(source, base, "merged", scratch / "merge"))
    git(source, "checkout", "-q", "-b", "empty", "topic")
    git(source, "commit", "-q", "--allow-empty", "-m", "chore: nothing",
        date="2026-01-09T12:00:00+0900")
    expect_error("empty topic", "changes nothing",
                 lambda: export(source, base, "empty", scratch / "empty"))
    git(source, "checkout", "-q", "-b", "twins", "topic")
    commit(source, "fix: change a", {"a.txt": b"ALPHA\n" * 20}, 10)
    expect_error("colliding subjects", "two topics export to fix-change-a.patch",
                 lambda: export(source, base, "twins", scratch / "twins"))

    # A topic's author is the only identity a series records, so export refuses
    # a placeholder or a host-derived default, names the commit, and leaves the
    # fix to an ordinary amend by the topic's owner.
    for email in ("dev@users.noreply.github.com", "dev@kernel.org",
                  "dev@example.community", "dev@localhost.dev", "dev@locality.org"):
        assert not patch_series.placeholder_email(email), email
    for email in ("dev@localhost", "dev@build.localhost", "dev@Mac.local",
                  "dev@HOST.LOCAL", "dev@box.localdomain", "dev@example.invalid",
                  "dev@host.(none)", "(none)", "dev@example.com",
                  "dev@mail.example.org", "dev@example.net", "dev", "dev@"):
        assert patch_series.placeholder_email(email), email
    git(source, "checkout", "-q", "-b", "placeholder", "topic")
    (source / "f.txt").write_bytes(b"foxtrot\n")
    git(source, "add", "f.txt")
    git(source, "commit", "-q", "-m", "fix: add f",
        "--author", "Placeholder <placeholder@example.invalid>",
        date="2026-01-11T12:00:00+0900")
    placeholder = scratch / "placeholder"
    shutil.copytree(series, placeholder)
    before = snapshot(placeholder)
    rejection = "(fix: add f) has the placeholder author Placeholder <placeholder@example.invalid>"
    expect_error("placeholder author", rejection,
                 lambda: export(source, base, "placeholder", placeholder, record_tree=True))
    expect_error("placeholder author check", rejection,
                 lambda: export(source, base, "placeholder", placeholder, check=True))
    assert snapshot(placeholder) == before
    command = [sys.executable, "-B", str(TOOL), "export", "--source", str(source),
               "--base", base, "--head", "placeholder", "--series-dir", str(placeholder)]
    rejected = subprocess.run(command, capture_output=True, check=False)
    assert rejected.returncode == 1 and b"--reset-author" in rejected.stderr, rejected.stderr
    assert snapshot(placeholder) == before
    git(source, "commit", "-q", "--amend", "--no-edit", "--reset-author",
        date="2026-01-11T12:00:00+0900")
    export(source, base, "placeholder", placeholder, record_tree=True)
    fixed = (placeholder / "fix-add-f.patch").read_text()
    assert "placeholder@example.invalid" not in fixed and "\nFrom: " in fixed, fixed

    # A session link in a topic message describes no change: export leaves the
    # line out of the patch, keeps the rest of the message and every diff line,
    # and the series still produces the topic's tree. A series that carries the
    # line no longer matches its topic, so a replay cannot bring it back.
    link = "Claude" + "-Session: https://example.invalid/session/1"
    trailing = scratch / "trailing"
    trailing.mkdir()
    git(trailing, "init", "-q", "-b", "base")
    trailing_base = commit(trailing, "base", {"a.txt": b"alpha\n"}, 1)
    git(trailing, "checkout", "-q", "-b", "topic")
    (trailing / "a.txt").write_bytes(b"alpha\n" + link.encode() + b"\n")
    git(trailing, "add", "-A")
    git(trailing, "commit", "-q", "-m", "fix: keep a diff line", "-m", "Body.", "-m",
        "Co-Authored-By: Example <example@example.com>\n" + link,
        date="2026-01-02T12:00:00+0900")
    (trailing / "b.txt").write_bytes(b"bravo\n")
    git(trailing, "add", "-A")
    git(trailing, "commit", "-q", "-m", "feat: link last", "-m", "Body.", "-m", link,
        date="2026-01-03T12:00:00+0900")
    trailing_tree = git(trailing, "rev-parse", "topic^{tree}").strip()
    trailer_series = scratch / "trailer-series"
    assert export(trailing, trailing_base, "topic", trailer_series, record_tree=True) == trailing_tree
    kept = (trailer_series / "fix-keep-a-diff-line.patch").read_text()
    assert "\n" + link not in kept and "\n+" + link + "\n" in kept, kept
    assert "\nBody.\n\nCo-Authored-By: Example <example@example.com>\n---\n" in kept, kept
    last = (trailer_series / "feat-link-last.patch").read_text()
    assert link not in last and "\nBody.\n---\n" in last, last
    assert patch_series.series_tree(trailing, trailing_base, [trailer_series]) == trailing_tree
    export(trailing, trailing_base, "topic", trailer_series, record_tree=True, check=True)
    stale = scratch / "trailer-stale"
    shutil.copytree(trailer_series, stale)
    stale_patch = stale / "feat-link-last.patch"
    stale_patch.write_text(stale_patch.read_text().replace("\n---\n", "\n\n" + link + "\n---\n", 1))
    expect_error("stale session link", "changed feat-link-last.patch",
                 lambda: export(trailing, trailing_base, "topic", stale, record_tree=True, check=True))
    assert patch_series.series_tree(trailing, trailing_base, [stale]) == trailing_tree

    # The command line reports tree mismatches with a non-zero status.
    verify = [sys.executable, "-B", str(TOOL), "verify", "--series-dir", str(series),
              "--repo", str(source), "--base", base]
    assert subprocess.run(verify, capture_output=True, check=False).returncode == 0
    wrong = subprocess.run(verify + ["--expect-tree", base], capture_output=True, check=False)
    assert wrong.returncode == 1 and b"expected" in wrong.stderr, wrong.stderr

    # Command-line paths are relative to the caller's directory, although Git
    # runs in the repository.
    relative = [sys.executable, "-B", str(TOOL), "verify", "--series-dir", "series",
                "--repo", "source", "--base", base]
    checked = subprocess.run(relative, cwd=scratch, capture_output=True, check=False)
    assert checked.returncode == 0, checked.stderr
    git(scratch, "clone", "-q", "--no-hardlinks", str(source), str(scratch / "relative"))
    git(scratch / "relative", "checkout", "-q", "-b", "relative", base)
    relative = [sys.executable, "-B", str(TOOL), "apply", "--series-dir", "series",
                "--repo", "relative"]
    applied = subprocess.run(relative, cwd=scratch, capture_output=True, check=False)
    assert applied.returncode == 0 and applied.stdout.decode() == head_tree + "\n", applied

    # recorded-tree checks every layer and prints the last one's tree.
    recorded = [sys.executable, "-B", str(TOOL), "recorded-tree",
                "--series-dir", str(common), "--series-dir", str(overlay)]
    printed = subprocess.run(recorded, capture_output=True, check=False)
    assert printed.returncode == 0 and printed.stdout.decode() == head_tree + "\n", printed
    (common / "stray.patch").write_bytes(patch_series.ZERO_FROM)
    stray = subprocess.run(recorded, capture_output=True, check=False)
    assert stray.returncode == 1 and b"unlisted=['stray.patch']" in stray.stderr, stray.stderr
    (common / "stray.patch").unlink()
    treeless = subprocess.run(recorded[:-2], capture_output=True, check=False)
    assert treeless.returncode == 1 and b"records no tree" in treeless.stderr, treeless.stderr


if __name__ == "__main__":
    raise SystemExit(main())
