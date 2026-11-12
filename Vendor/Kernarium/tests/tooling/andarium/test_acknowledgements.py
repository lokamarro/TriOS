#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Andarium acknowledges every framework and prefix library it embeds.

The Acknowledgements resource must name a project, its copyright holders and
its license, with the license text, for each framework the Andarium project
embeds and each component of the iOS device prefixes' contracts. The checks run
on the checked-in resource and on copies with one acknowledgement removed, so a
framework or library added without one fails here.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import copy
import json
import pathlib
import subprocess
import sys
import unittest

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPOSITORY / "Andarium" / "scripts" / "acknowledgements.py"
sys.path.insert(0, str(SCRIPT.parent))
import acknowledgements as ack  # noqa: E402

NOT_ACKNOWLEDGED = ("Andarium", "System", "User-Mode Linux")


class AcknowledgementsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.document = json.loads(ack.RESOURCE.read_text(encoding="utf-8"))
        self.frameworks = ack.embedded_frameworks()
        self.components = ack.device_components()

    def problems(self, document: dict) -> list[str]:
        return ack.check_document(document, self.frameworks, self.components)

    def test_the_resource_covers_what_the_app_embeds(self) -> None:
        self.assertEqual(self.problems(self.document), [])

    def test_the_inputs_are_not_empty(self) -> None:
        # A checker that finds nothing to check proves nothing.
        self.assertGreaterEqual(len(self.frameworks), 5)
        self.assertGreaterEqual(len(self.components), 13)
        self.assertIn("KRuntime_Android_6_18_4K", self.frameworks)
        self.assertIn("mesa", self.components)
        self.assertIn("libslirp", self.components)

    def test_an_unacknowledged_framework_fails(self) -> None:
        for framework in sorted(self.frameworks):
            with self.subTest(framework=framework):
                document = copy.deepcopy(self.document)
                for project in document["projects"]:
                    project["frameworks"] = [name for name in project["frameworks"] if name != framework]
                found = self.problems(document)
                self.assertTrue(
                    any(f"framework {framework} has no acknowledgement" in line for line in found), found
                )

    def test_an_unacknowledged_library_fails(self) -> None:
        for component in sorted(self.components):
            with self.subTest(component=component):
                document = copy.deepcopy(self.document)
                for project in document["projects"]:
                    project["components"] = [name for name in project["components"] if name != component]
                found = self.problems(document)
                self.assertTrue(
                    any(f"library {component} has no acknowledgement" in line for line in found), found
                )

    def test_a_new_framework_or_library_fails(self) -> None:
        found = ack.check_document(self.document, self.frameworks | {"NewFramework"}, self.components | {"newlib"})
        self.assertTrue(any("NewFramework" in line for line in found), found)
        self.assertTrue(any("newlib" in line for line in found), found)

    def test_a_stale_entry_fails(self) -> None:
        found = ack.check_document(self.document, self.frameworks - {"LibZstd"}, self.components - {"zstd-decompression"})
        self.assertTrue(any("LibZstd is not embedded" in line for line in found), found)
        self.assertTrue(any("zstd-decompression is in no iOS device prefix" in line for line in found), found)

    def test_a_project_needs_its_fields_and_text(self) -> None:
        for field in ("name", "copyright", "license", "spdx"):
            with self.subTest(field=field):
                document = copy.deepcopy(self.document)
                document["projects"][-1][field] = " "
                self.assertTrue(any(f"no {field}" in line for line in self.problems(document)))
        document = copy.deepcopy(self.document)
        document["projects"][-1]["texts"] = []
        document["projects"][-1]["licenses"] = []
        self.assertTrue(any("no license text" in line for line in self.problems(document)))
        document = copy.deepcopy(self.document)
        document["licenses"]["apache-2.0"]["text"] = ""
        self.assertTrue(any("'apache-2.0' is missing" in line for line in self.problems(document)))

    def test_every_project_names_a_group_and_an_id_once(self) -> None:
        identifiers = [project["id"] for project in self.document["projects"]]
        self.assertEqual(len(identifiers), len(set(identifiers)))
        groups = {group["id"] for group in self.document["groups"]}
        self.assertTrue(all(project["group"] in groups for project in self.document["projects"]))

    def test_the_overlapping_and_unshipped_names_are_gone(self) -> None:
        names = {project["name"] for project in self.document["projects"]}
        self.assertEqual(names & set(NOT_ACKNOWLEDGED), set())
        for expected in ("Android Open Source Project", "LineageOS", "Linux", "Mesa", "virglrenderer"):
            self.assertIn(expected, names)

    def test_the_command_line_check_passes(self) -> None:
        result = subprocess.run(
            [sys.executable, "-B", str(SCRIPT), "--coverage"],
            check=False,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr)


class ReflowTests(unittest.TestCase):
    def test_hard_wrapped_paragraphs_join_and_blocks_stay(self) -> None:
        wrapped = (
            "The licenses for most software are designed to take away your freedom to\n"
            "share and change it.  By contrast, the GNU General Public License is\n"
            "intended to guarantee your freedom to share and change free software."
        )
        block = "Juha Pirkola.\nGregory M. Christy.\nThe Regents of the University of California."
        text = f"{wrapped}\n\n{block}"
        result = ack.reflow(text)
        self.assertEqual(result.split("\n\n")[0].count("\n"), 0)
        self.assertEqual(result.split("\n\n")[1], block)
        self.assertEqual(result.split(), text.split())

    def test_a_changed_word_is_refused(self) -> None:
        with self.assertRaises(ack.Problem):
            ack.clean("one two three", original="one two four")


if __name__ == "__main__":
    unittest.main()
