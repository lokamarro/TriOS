#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""android/lineage-24/support/release/input-paths.tsv and its reader.

The list says which tracked paths decide an Android release. Its rows must be
well formed, every `input` path must exist, and every tracked file under
android/lineage-24 must be covered by a row, so that a producer added later is
classified, not silently left out of the release's identity.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def git(repository: Path, *arguments: str) -> str:
    return subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=True, stdin=subprocess.DEVNULL, capture_output=True, text=True,
    ).stdout.strip()


class ReaderTests(unittest.TestCase):
    def write(self, text: str) -> Path:
        path = Path(self.enterContext(tempfile.TemporaryDirectory())) / "list.tsv"
        path.write_text(text, encoding="utf-8")
        return path

    def test_rows_and_input_paths(self) -> None:
        path = self.write(
            "# comment\n\ninput\ta/b\treason\nexcluded\ta/b/c\tno\ninput\tz\tlast\n")
        self.assertEqual(
            release_inputs.read_rows(path),
            [("input", "a/b", "reason"), ("excluded", "a/b/c", "no"),
             ("input", "z", "last")])
        self.assertEqual(release_inputs.input_paths(path), ["a/b", "z"])

    def test_malformed_lists_are_refused(self) -> None:
        for label, text in (
            ("a row with two fields", "input\ta\n"),
            ("an empty reason", "input\ta\t \n"),
            ("an unknown kind", "optional\ta\twhy\n"),
            ("an absolute path", "input\t/a\twhy\n"),
            ("a parent path", "input\t../a\twhy\n"),
            ("a trailing slash", "input\ta/\twhy\n"),
            ("a space in a path", "input\ta b\twhy\n"),
            ("a quote in a path", "input\ta'b\twhy\n"),
            ("a repeated path", "input\ta\twhy\ninput\ta\twhy\n"),
            ("no input row", "excluded\ta\twhy\n"),
            ("no rows", "# nothing\n"),
        ):
            with self.subTest(label), self.assertRaises(release_inputs.InputError):
                release_inputs.read_rows(self.write(text))

    def test_the_longest_row_covers_a_file(self) -> None:
        rows = [("input", "a", "x"), ("excluded", "a/b", "y"), ("input", "a/b/c", "z")]
        cover = release_inputs.covering_row
        self.assertEqual(cover(rows, "a/q")[1], "a")
        self.assertEqual(cover(rows, "a/b/q")[1], "a/b")
        self.assertEqual(cover(rows, "a/b/c/q")[1], "a/b/c")
        self.assertEqual(cover(rows, "a/b")[1], "a/b")
        self.assertIsNone(cover(rows, "ab"))
        self.assertIsNone(cover(rows, "z"))

    def test_trees_round_trip_and_reject_disorder(self) -> None:
        trees = {"b/c": "2" * 40, "a": "1" * 40}
        text = release_inputs.render_trees(trees)
        self.assertEqual(text, f"a={'1' * 40} b/c={'2' * 40}")
        self.assertEqual(release_inputs.parse_trees(text), trees)
        for bad in ("", f"b/c={'2' * 40} a={'1' * 40}", f"a={'1' * 40} a={'1' * 40}",
                    "a=abc", f"a={'G' * 40}", f"a {'1' * 40}", f"a={'1' * 40}  b={'2' * 40}"):
            with self.subTest(bad), self.assertRaises(release_inputs.InputError):
                release_inputs.parse_trees(bad)


class ObjectIdTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repository = Path(self.enterContext(tempfile.TemporaryDirectory()))
        git(self.repository, "init", "-q", ".")
        (self.repository / "dir/sub").mkdir(parents=True)
        (self.repository / "dir/sub/file").write_text("one\n")
        (self.repository / "dir/other").write_text("two\n")
        (self.repository / "top").write_text("three\n")
        git(self.repository, "add", "-A")
        git(self.repository, "commit", "-q", "-m", "base")

    def test_a_directory_is_its_tree_and_a_file_its_blob(self) -> None:
        ids = release_inputs.object_ids(self.repository, ["top", "dir", "dir/sub"])
        self.assertEqual(list(ids), ["dir", "dir/sub", "top"])
        self.assertEqual(ids["dir"], git(self.repository, "rev-parse", "HEAD:dir"))
        self.assertEqual(ids["dir/sub"], git(self.repository, "rev-parse", "HEAD:dir/sub"))
        self.assertEqual(ids["top"], git(self.repository, "rev-parse", "HEAD:top"))

    def test_an_id_follows_content_not_history(self) -> None:
        before = release_inputs.object_ids(self.repository, ["dir", "top"])
        (self.repository / "top").write_text("changed\n")
        git(self.repository, "commit", "-q", "-a", "-m", "change top")
        after = release_inputs.object_ids(self.repository, ["dir", "top"])
        self.assertEqual(before["dir"], after["dir"])
        self.assertNotEqual(before["top"], after["top"])
        # A rewritten history and an exported snapshot hold the same content.
        snapshot = Path(self.enterContext(tempfile.TemporaryDirectory()))
        archive = subprocess.run(
            ["git", "-C", str(self.repository), "archive", "HEAD"],
            check=True, stdout=subprocess.PIPE).stdout
        subprocess.run(["tar", "-x", "-C", str(snapshot)], input=archive, check=True)
        git(snapshot, "init", "-q", ".")
        git(snapshot, "add", "-A")
        git(snapshot, "commit", "-q", "-m", "snapshot")
        self.assertEqual(release_inputs.object_ids(snapshot, ["dir", "top"]), after)

    def test_a_missing_path_is_named(self) -> None:
        with self.assertRaisesRegex(release_inputs.InputError, "nothing/here"):
            release_inputs.object_ids(self.repository, ["top", "nothing/here"])

    def test_an_earlier_revision_is_readable(self) -> None:
        first = git(self.repository, "rev-parse", "HEAD")
        (self.repository / "top").write_text("changed\n")
        git(self.repository, "commit", "-q", "-a", "-m", "change top")
        self.assertNotEqual(
            release_inputs.object_ids(self.repository, ["top"], first),
            release_inputs.object_ids(self.repository, ["top"]))


class TrackedListTests(unittest.TestCase):
    """The tracked list, read from the checkout under test."""

    def test_every_input_exists_and_every_file_is_covered(self) -> None:
        rows = release_inputs.read_rows(ROOT / release_inputs.LIST_RELATIVE)
        files = git(ROOT, "ls-files", "--", "android/lineage-24").splitlines()
        self.assertTrue(files)
        uncovered = [path for path in files if release_inputs.covering_row(rows, path) is None]
        self.assertEqual(
            uncovered, [],
            "tracked files under android/lineage-24 that input-paths.tsv does not "
            "classify as input or excluded")
        tracked = set(files)
        for kind, path, _ in rows:
            with self.subTest(path):
                self.assertTrue(
                    path in tracked or any(name.startswith(path + "/") for name in tracked),
                    f"{kind} row {path} names no tracked file")

    def test_the_list_is_tight(self) -> None:
        rows = release_inputs.read_rows(ROOT / release_inputs.LIST_RELATIVE)
        inputs = [path for kind, path, _ in rows if kind == release_inputs.INPUT]
        for path in inputs:
            # The producers this lane relies on are named, and no verifier or
            # documentation is an input.
            self.assertNotIn("/verify-", path)
            self.assertFalse(path.endswith(".md"), path)
            self.assertFalse(path.startswith("tests/"), path)
        for required in (
            "android/lineage-24/overlay",
            "android/lineage-24/patches",
            "android/lineage-24/metadata/base-manifest.xml",
            "android/lineage-24/tools/publish-release.py",
            "android/lineage-24/build",
        ):
            self.assertIn(required, inputs)
        # The acceptance record is written after a release exists, so it cannot
        # be an input of that release.
        self.assertNotIn("android/lineage-24/metadata/accepted-release.env", inputs)


def main() -> None:
    global release_inputs
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=ROOT)
    arguments, rest = parser.parse_known_args()
    globals()["ROOT"] = arguments.source_root.resolve()
    sys.path.insert(0, str(ROOT / "android/lineage-24/tools"))
    import release_inputs as module  # noqa: PLC0415

    release_inputs = module
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)


if __name__ == "__main__":
    main()
