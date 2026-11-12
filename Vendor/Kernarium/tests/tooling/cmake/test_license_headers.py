#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the licence-header checker on fixtures, then on the tree.

A fixture starts clean and each case mutates one thing: a header, a licence
text or an exception. The checker must fail on exactly that case and name the
file, the line and the rule. The last run is the tree itself, which must be
clean.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# Only a wedged checker may fail on time: a scan takes well under a second
# alone. The hard bounds are this subprocess timeout and the test's TIMEOUT.
CHECKER_WEDGE_SECONDS = 60

# The licence texts of the tree under test seed every fixture, so a fixture
# checks the real markers rather than restating them.
SEED_TEXTS = ("LICENSE", "LICENSES/Apache-2.0.txt", "LICENSES/MIT.txt",
              "LICENSES/BSD-3-Clause.txt")

EXCEPTIONS = (
    "# fixture\n"
    "kernel/common/video/virtio_video.h\tGPL-2.0+\tmirrored in a generated topic\n"
    "darwin/derived/kernel_file.cpp\tGPL-2.0-only\tderived from kernel code\n"
    "darwin/include/uapi_copy.h\tGPL-2.0-only WITH Linux-syscall-note\tuapi header\n"
    "darwin/include/bsd_header.h\tBSD-3-Clause\ta header that states BSD terms\n"
)

# path -> text of the clean fixture
CLEAN = {
    "darwin/a.cpp": "/* SPDX-License-Identifier: GPL-2.0-or-later */\nint a;\n",
    "darwin/b.h": "// SPDX-License-Identifier: GPL-2.0-or-later\n",
    "darwin/gadgets/engine.inc": "/* SPDX-License-Identifier: GPL-2.0-or-later */\ngadget x\n",
    "tools/run.py": "#!/usr/bin/env python3\n# SPDX-License-Identifier: GPL-2.0-or-later\n",
    "tools/launch": "#!/bin/sh\n# SPDX-License-Identifier: GPL-2.0-or-later\n",
    "App/View.swift": "// SPDX-License-Identifier: GPL-2.0-or-later\n\nimport SwiftUI\n",
    "CMakeLists.txt": "# SPDX-License-Identifier: GPL-2.0-or-later\n",
    "docs/guide.md": "No header is needed here.\n",
    "data/cases.jsonl": "{\"a\": 1}\n",
    "tests/fixture.py": (
        "# SPDX-License-Identifier: GPL-2.0-or-later\n"
        + "\n" * 20
        + 'TEXT = "# SPDX-License-Identifier: GPL-2.0"\n'
    ),
    "dependencies/patches/mesa/0001-x.patch": (
        "diff --git a/a b/a\n+/* SPDX-License-Identifier: GPL-2.0 */\n"
    ),
    "dependencies/patches/mesa/series.tsv": (
        "# SPDX-License-Identifier: GPL-2.0-or-later\npatch\t0001-x.patch\n"
    ),
    "android/lineage-24/overlay/init.rc": "# SPDX-License-Identifier: Apache-2.0\n",
    "android/lineage-24/overlay/config/audio.xml": (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        "<!-- SPDX-License-Identifier: Apache-2.0 -->\n<a/>\n"
    ),
    "android/lineage-24/overlay/config/hals.conf": "/vendor/lib64/x.so\n",
    "android/lineage-24/overlay/sepolicy/file_contexts": (
        "# SPDX-License-Identifier: Apache-2.0\n/dev/hvc0 u:object_r:x:s0\n"
    ),
    "android/lineage-24/tools/build.py": "#!/usr/bin/env python3\n# SPDX-License-Identifier: GPL-2.0-or-later\n",
    "android/lineage-24/metadata/accepted.env": "# SPDX-License-Identifier: GPL-2.0-or-later\n",
    "android/lineage-24/patches/projects.tsv": "# SPDX-License-Identifier: Apache-2.0\n",
    "android/lineage-24/patches/projects/p0001/series/series.tsv": (
        "# SPDX-License-Identifier: Apache-2.0\n"
    ),
    "android/lineage-24/support/image/policy.json": "{}\n",
    "android/lineage-24/third_party/mesa-venus/soong-port.toml": "# SPDX-License-Identifier: MIT\n",
    "android/lineage-24/third_party/mesa-venus/metadata.env": "# SPDX-License-Identifier: Apache-2.0\n",
    "tests/integration/android/webgl/Android.bp": "// SPDX-License-Identifier: Apache-2.0\n",
    "tests/integration/android/run_adb.py": "#!/usr/bin/env python3\n# SPDX-License-Identifier: GPL-2.0-or-later\n",
    "kernel/common/video/virtio_video.h": "/* SPDX-License-Identifier: GPL-2.0+ */\n",
    "darwin/derived/kernel_file.cpp": "/* SPDX-License-Identifier: GPL-2.0-only */\n",
    "darwin/include/uapi_copy.h": (
        "/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */\n"
    ),
    "darwin/include/bsd_header.h": "/* SPDX-License-Identifier: BSD-3-Clause */\n",
}

# The exception text of a fixture that uses the Linux-syscall-note exception.
SYSCALL_NOTE = (
    "NOTE! This copyright does *not* cover user programs that use kernel\n"
    "services by normal system calls.\n"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def write(root: Path, name: str, text: str) -> None:
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class Fixture:
    def __init__(self, root: Path, checker: Path, source_root: Path) -> None:
        self.root = root
        self.checker = checker
        self.source_root = source_root

    def reset(self) -> None:
        for child in self.root.iterdir():
            if child.is_dir():
                shutil.rmtree(child)
            else:
                child.unlink()
        for name in SEED_TEXTS:
            destination = self.root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.source_root / name, destination)
        write(self.root, "LICENSES/Linux-syscall-note.txt", SYSCALL_NOTE)
        for name, text in CLEAN.items():
            write(self.root, name, text)
        write(self.root, "exceptions.tsv", EXCEPTIONS)

    def run(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-B", str(self.checker), "--source-root",
             str(self.root), "--exceptions", str(self.root / "exceptions.tsv")],
            check=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=CHECKER_WEDGE_SECONDS)


def findings(output: str) -> set[tuple[str, int, str]]:
    """Parse `path:line: RULE: message` into (path, line, rule)."""
    parsed: set[tuple[str, int, str]] = set()
    for line in output.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("H1", "H2", "H3", "H4", "H5"):
            path, _, number = parts[0].rpartition(":")
            parsed.add((path, int(number), parts[1]))
    return parsed


def expect(fixture: Fixture, label: str, *expected: tuple[str, int, str]) -> None:
    result = fixture.run()
    got = findings(result.stdout)
    require(got == set(expected),
            f"{label}: expected {sorted(expected)}, got {sorted(got)}:\n{result.stdout}")
    require((result.returncode == 0) == (not expected),
            f"{label}: exit status {result.returncode}")


def test_clean(fixture: Fixture) -> None:
    fixture.reset()
    result = fixture.run()
    require(result.returncode == 0 and "no findings" in result.stdout,
            f"the clean fixture failed:\n{result.stdout}")
    # What the clean fixture proves: a patch file, a fixture string past the
    # header, data and an uncommented overlay config are not headers.
    require(findings(result.stdout) == set(), "the clean fixture has findings")


def mutate(fixture: Fixture, label: str, name: str, text: str,
           *expected: tuple[str, int, str]) -> None:
    fixture.reset()
    write(fixture.root, name, text)
    expect(fixture, label, *expected)


def test_headers(fixture: Fixture) -> None:
    mutate(fixture, "deprecated GPL-2.0", "darwin/a.cpp",
           "/* SPDX-License-Identifier: GPL-2.0 */\n",
           ("darwin/a.cpp", 1, "H3"))
    mutate(fixture, "deprecated GPL-2.0+", "darwin/a.cpp",
           "/* SPDX-License-Identifier: GPL-2.0+ */\n",
           ("darwin/a.cpp", 1, "H3"))
    mutate(fixture, "deprecated in an expression", "tools/run.py",
           "#!/usr/bin/env python3\n# SPDX-License-Identifier: MIT OR GPL-3.0\n",
           ("tools/run.py", 2, "H3"))
    mutate(fixture, "GPL-3.0-or-later outside its location", "tools/run.py",
           "#!/usr/bin/env python3\n# SPDX-License-Identifier: GPL-3.0-or-later\n",
           ("tools/run.py", 2, "H2"))
    mutate(fixture, "GPL-3.0-or-later in the Android host tools",
           "android/lineage-24/tools/build.py",
           "#!/usr/bin/env python3\n# SPDX-License-Identifier: GPL-3.0-or-later\n",
           ("android/lineage-24/tools/build.py", 2, "H2"))
    mutate(fixture, "Apache-2.0 in default code", "darwin/a.cpp",
           "/* SPDX-License-Identifier: Apache-2.0 */\n",
           ("darwin/a.cpp", 1, "H2"))
    mutate(fixture, "MIT in default code", "darwin/a.cpp",
           "/* SPDX-License-Identifier: MIT */\n", ("darwin/a.cpp", 1, "H2"))
    mutate(fixture, "GPL-2.0-only without an exception", "darwin/a.cpp",
           "/* SPDX-License-Identifier: GPL-2.0-only */\n",
           ("darwin/a.cpp", 1, "H2"))
    mutate(fixture, "GPL in the Android overlay",
           "android/lineage-24/overlay/init.rc",
           "# SPDX-License-Identifier: GPL-2.0-or-later\n",
           ("android/lineage-24/overlay/init.rc", 1, "H2"))
    mutate(fixture, "GPL in an Android test app",
           "tests/integration/android/webgl/Android.bp",
           "// SPDX-License-Identifier: GPL-2.0-or-later\n",
           ("tests/integration/android/webgl/Android.bp", 1, "H2"))
    mutate(fixture, "Apache in the Android test runners",
           "tests/integration/android/run_adb.py",
           "#!/usr/bin/env python3\n# SPDX-License-Identifier: Apache-2.0\n",
           ("tests/integration/android/run_adb.py", 2, "H2"))
    mutate(fixture, "Apache in Mesa's build files",
           "android/lineage-24/third_party/mesa-venus/soong-port.toml",
           "# SPDX-License-Identifier: Apache-2.0\n",
           ("android/lineage-24/third_party/mesa-venus/soong-port.toml", 1, "H2"))
    mutate(fixture, "the patched project's licence on a series manifest",
           "dependencies/patches/mesa/series.tsv",
           "# SPDX-License-Identifier: MIT\n",
           ("dependencies/patches/mesa/series.tsv", 1, "H2"))
    mutate(fixture, "an empty identifier", "darwin/a.cpp",
           "/* SPDX-License-Identifier: */\n", ("darwin/a.cpp", 1, "H2"))
    mutate(fixture, "a blank identifier", "tools/run.py",
           "#!/usr/bin/env python3\n# SPDX-License-Identifier:\n",
           ("tools/run.py", 2, "H2"))
    mutate(fixture, "a malformed expression", "darwin/a.cpp",
           "/* SPDX-License-Identifier: GPL-2.0-or-later OR */\n",
           ("darwin/a.cpp", 1, "H2"))
    mutate(fixture, "a tag that is not in a comment", "darwin/a.cpp",
           'const char *t = "SPDX-License-Identifier: GPL-2.0-or-later";\n',
           ("darwin/a.cpp", 1, "H2"))
    mutate(fixture, "a missing header in C++", "darwin/a.cpp", "int a;\n",
           ("darwin/a.cpp", 1, "H1"))
    mutate(fixture, "a missing header in Swift", "App/View.swift",
           "import SwiftUI\n", ("App/View.swift", 1, "H1"))
    mutate(fixture, "a missing header in an assembler include",
           "darwin/gadgets/engine.inc", "gadget x\n",
           ("darwin/gadgets/engine.inc", 1, "H1"))
    mutate(fixture, "a missing header in a script without a suffix",
           "tools/launch", "#!/bin/sh\necho hi\n", ("tools/launch", 1, "H1"))
    mutate(fixture, "a missing header in CMake", "CMakeLists.txt",
           "project(x)\n", ("CMakeLists.txt", 1, "H1"))
    mutate(fixture, "a header past the first lines", "darwin/a.cpp",
           "\n" * 8 + "/* SPDX-License-Identifier: GPL-2.0-or-later */\n",
           ("darwin/a.cpp", 1, "H1"))
    mutate(fixture, "a fixture string is no header", "darwin/a.cpp",
           "int a;\n" + 'const char *t = "SPDX-License-Identifier: GPL-2.0";\n',
           ("darwin/a.cpp", 2, "H2"))
    mutate(fixture, "a missing header in an overlay config",
           "android/lineage-24/overlay/sepolicy/file_contexts",
           "/dev/hvc0 u:object_r:x:s0\n",
           ("android/lineage-24/overlay/sepolicy/file_contexts", 1, "H1"))
    mutate(fixture, "a missing header in an overlay XML",
           "android/lineage-24/overlay/config/audio.xml", "<a/>\n",
           ("android/lineage-24/overlay/config/audio.xml", 1, "H1"))
    mutate(fixture, "a new overlay file without a header",
           "android/lineage-24/overlay/new.rc", "service x /bin/x\n",
           ("android/lineage-24/overlay/new.rc", 1, "H1"))
    # Carried patch content and uncommented data are never scanned.
    mutate(fixture, "a patch with another project's header",
           "dependencies/patches/mesa/0002-y.patch",
           "+/* SPDX-License-Identifier: GPL-2.0+ */\n")
    mutate(fixture, "Apache-2.0 in the Android build-host tools",
           "android/lineage-24/tools/build.py",
           "#!/usr/bin/env python3\n# SPDX-License-Identifier: Apache-2.0\n",
           ("android/lineage-24/tools/build.py", 2, "H2"))
    mutate(fixture, "Apache-2.0 in the Android build scripts",
           "android/lineage-24/build/build-aosp.sh",
           "#!/bin/bash\n# SPDX-License-Identifier: Apache-2.0\n",
           ("android/lineage-24/build/build-aosp.sh", 2, "H2"))
    mutate(fixture, "Apache-2.0 in the Android recorded metadata",
           "android/lineage-24/metadata/accepted.env",
           "# SPDX-License-Identifier: Apache-2.0\n",
           ("android/lineage-24/metadata/accepted.env", 1, "H2"))
    mutate(fixture, "GPL on the digest-bound route table",
           "android/lineage-24/patches/projects.tsv",
           "# SPDX-License-Identifier: GPL-2.0-or-later\n",
           ("android/lineage-24/patches/projects.tsv", 1, "H2"))
    mutate(fixture, "GPL on an Android series manifest",
           "android/lineage-24/patches/projects/p0001/series/series.tsv",
           "# SPDX-License-Identifier: GPL-2.0-or-later\n",
           ("android/lineage-24/patches/projects/p0001/series/series.tsv", 1, "H2"))


def test_texts(fixture: Fixture) -> None:
    fixture.reset()
    (fixture.root / "LICENSES/MIT.txt").unlink()
    write(fixture.root, "tools/mit.py",
          "#!/usr/bin/env python3\n# SPDX-License-Identifier: GPL-2.0-or-later\n")
    write(fixture.root, "android/lineage-24/third_party/mesa-venus/soong-port.toml",
          "# SPDX-License-Identifier: MIT\n")
    expect(fixture, "a missing MIT text", ("LICENSES/MIT.txt", 1, "H4"))

    fixture.reset()
    (fixture.root / "LICENSE").unlink()
    expect(fixture, "a missing LICENSE", ("LICENSE", 1, "H4"))

    fixture.reset()
    (fixture.root / "LICENSES/Apache-2.0.txt").unlink()
    expect(fixture, "a missing Apache-2.0 text", ("LICENSES/Apache-2.0.txt", 1, "H4"))

    fixture.reset()
    (fixture.root / "LICENSES/Linux-syscall-note.txt").unlink()
    expect(fixture, "a missing exception text",
           ("LICENSES/Linux-syscall-note.txt", 1, "H4"))

    fixture.reset()
    write(fixture.root, "LICENSES/BSD-3-Clause.txt", "Redistribute freely.\n")
    result = fixture.run()
    got = findings(result.stdout)
    require({(p, rule) for p, _, rule in got} == {("LICENSES/BSD-3-Clause.txt", "H4")},
            f"a text that is not BSD-3-Clause: {sorted(got)}:\n{result.stdout}")

    fixture.reset()
    text = (fixture.root / "LICENSE").read_text(encoding="utf-8")
    write(fixture.root, "LICENSE", "Valid-License-Identifier: GPL-2.0\nLicense-Text:\n\n" + text)
    result = fixture.run()
    got = findings(result.stdout)
    require({(p, rule) for p, _, rule in got} == {("LICENSE", "H4")},
            f"a LICENSE with the kernel metadata header: {sorted(got)}:\n{result.stdout}")

    fixture.reset()
    write(fixture.root, "LICENSE", "The GPL, in spirit.\n")
    result = fixture.run()
    got = findings(result.stdout)
    require({(p, rule) for p, _, rule in got} == {("LICENSE", "H4")},
            f"a LICENSE that is not the GPL-2.0 text: {sorted(got)}:\n{result.stdout}")

    fixture.reset()
    write(fixture.root, "LICENSES/Apache-2.0.txt", "\n")
    result = fixture.run()
    got = findings(result.stdout)
    require({(p, rule) for p, _, rule in got} == {("LICENSES/Apache-2.0.txt", "H4")},
            f"an empty Apache-2.0 text: {sorted(got)}:\n{result.stdout}")


def test_exceptions(fixture: Fixture) -> None:
    mutate(fixture, "an exception whose header changed",
           "kernel/common/video/virtio_video.h",
           "/* SPDX-License-Identifier: GPL-2.0-or-later */\n",
           ("kernel/common/video/virtio_video.h", 1, "H5"))
    mutate(fixture, "a derived file that says the wrong kernel identifier",
           "darwin/derived/kernel_file.cpp",
           "/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */\n",
           ("darwin/derived/kernel_file.cpp", 1, "H5"))
    fixture.reset()
    (fixture.root / "darwin/derived/kernel_file.cpp").unlink()
    result = fixture.run()
    require(result.returncode == 1 and "H5" in result.stdout
            and "matches no file" in result.stdout,
            f"a stale exception was accepted:\n{result.stdout}")
    fixture.reset()
    write(fixture.root, "exceptions.tsv", EXCEPTIONS + "bad line\n")
    result = fixture.run()
    require(result.returncode == 2, f"a malformed exceptions file: {result.returncode}")
    fixture.reset()
    write(fixture.root, "exceptions.tsv",
          EXCEPTIONS + "darwin/x.cpp\tGPL-2.0-only\n")
    result = fixture.run()
    require(result.returncode == 2, f"an exception without a reason: {result.returncode}")


def test_git(fixture: Fixture) -> None:
    """In a Git work tree, tracked and new unignored files are checked."""
    fixture.reset()
    write(fixture.root, ".gitignore", "ignored/\n")
    write(fixture.root, "ignored/bad.cpp", "int ignored;\n")
    write(fixture.root, "darwin/untracked.cpp", "int untracked;\n")
    environment = dict(os.environ)
    environment.update(GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null")
    subprocess.run(["git", "init", "-q", str(fixture.root)], check=True,
                   env=environment, timeout=60)
    subprocess.run(["git", "-C", str(fixture.root), "add", "--", "darwin/a.cpp"],
                   check=True, env=environment, timeout=60)
    expect(fixture, "an untracked file in a work tree", ("darwin/untracked.cpp", 1, "H1"))
    # A tracked file that was deleted from the work tree is skipped.
    (fixture.root / "darwin/a.cpp").unlink()
    (fixture.root / "darwin/untracked.cpp").unlink()
    expect(fixture, "a deleted tracked file")


def test_tree(checker: Path, source_root: Path) -> None:
    result = subprocess.run(
        [sys.executable, "-B", str(checker), "--source-root", str(source_root)],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        timeout=CHECKER_WEDGE_SECONDS)
    require(result.returncode == 0, f"the tree has licence findings:\n{result.stdout}")
    print(result.stdout, end="")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--checker", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    options = parser.parse_args()
    checker = options.checker.resolve()
    source_root = options.source_root.resolve()
    with tempfile.TemporaryDirectory(prefix="uml-license-headers.") as scratch:
        fixture = Fixture(Path(scratch), checker, source_root)
        test_clean(fixture)
        test_headers(fixture)
        test_texts(fixture)
        test_exceptions(fixture)
        test_git(fixture)
    test_tree(checker, source_root)
    print("license headers: fixtures and tree pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
