#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that the public C API check follows the headers.

The checker takes the API from the public headers, so the test runs it on
fixture headers to show what it reads, on the real headers against the
compiler's own list of their functions, and on real `nm -gU` output of
archives that define too much, too little or the API twice.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path


TIMEOUT_SECONDS = 120

# Every way a header can mention a name without declaring a function.
FIXTURE_HEADER = r'''/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FIXTURE_H
#define FIXTURE_H
#define FIXTURE_MACRO(value) \
	uml_emulator_in_a_macro(value)
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* uml_emulator_in_a_comment(void); */
// uml_emulator_in_a_line_comment(void);
struct uml_emulator_table {
	int (*open)(void *context);
	int uml_emulator_member(int);
};
typedef int (*uml_emulator_callback)(void *context);
typedef int uml_emulator_typedef_function(int);
enum uml_emulator_kind { UML_EMULATOR_ONE = 1 };
extern int (*uml_emulator_pointer)(int);
static const char uml_emulator_text[] = "uml_emulator_in_a_string(void);";

int uml_emulator_first(const struct uml_emulator_table *table,
		       struct uml_emulator_table **result);
void
uml_emulator_second(struct uml_emulator_table *table);
static inline int uml_emulator_inline_body(int value)
{
	if (value)
		return 1;
	return 0;
}
size_t other_prefix_function(void);

#ifdef __cplusplus
}
#endif

#endif
'''
FIXTURE_FUNCTIONS = ["other_prefix_function", "uml_emulator_first",
                     "uml_emulator_second"]

BAD_HEADERS = {
    "unbalanced": "void uml_emulator_open(void) {\n",
    "no functions": "struct uml_emulator { int value; };\n",
}


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=False,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=TIMEOUT_SECONDS,
    )


def listed(checker: Path, headers: Path) -> tuple[int, list[str]]:
    result = run([sys.executable, "-B", str(checker),
                  "--headers", str(headers), "--list"])
    return result.returncode, result.stdout.split()


def compiler_functions(headers: Path) -> list[str]:
    """The functions the compiler finds declared in the headers themselves."""
    include = headers.parent
    names: set[str] = set()
    for header in sorted(headers.rglob("*.h")):
        dump = run(["xcrun", "clang", "-fsyntax-only", "-x", "c",
                    "-I", str(include), "-Xclang", "-ast-dump", str(header)])
        if dump.returncode != 0:
            raise SystemExit(f"clang rejected {header}:\n{dump.stdout}")
        in_header = False
        for line in dump.stdout.splitlines():
            location = re.search(r"<([^:>,\s]+):\d+:\d+", line)
            if location and location.group(1) not in ("line", "col"):
                in_header = Path(location.group(1)).resolve().is_relative_to(
                    headers)
            declared = re.search(
                r"FunctionDecl 0x[0-9a-f]+ (?:prev 0x[0-9a-f]+ )?<[^>]*> "
                r"(?:line|col):\d+(?::\d+)? (?:implicit |used |referenced )*"
                r"(\w+) '", line)
            if declared and in_header:
                names.add(declared.group(1))
    return sorted(names)


def build_archive(root: Path, members: list[list[str]]) -> Path:
    """An archive with one member per list of functions it defines."""
    objects = []
    for index, functions in enumerate(members):
        source = root / f"member{index}.c"
        source.write_text(
            "".join(f"void {name}(void) {{}}\n" for name in functions),
            encoding="utf-8")
        object_path = root / f"member{index}.o"
        built = run(["xcrun", "clang", "-c", str(source),
                     "-o", str(object_path)])
        if built.returncode != 0:
            raise SystemExit(f"could not compile the fixture:\n{built.stdout}")
        objects.append(str(object_path))
    archive = root / "libapi.a"
    archive.unlink(missing_ok=True)
    archived = run(["xcrun", "libtool", "-static", "-o", str(archive),
                    *objects])
    if archived.returncode != 0:
        raise SystemExit(f"could not archive the fixture:\n{archived.stdout}")
    defined = run(["xcrun", "nm", "-gU", str(archive)])
    if defined.returncode != 0:
        raise SystemExit(f"could not read the fixture:\n{defined.stdout}")
    output = root / "global-defined.txt"
    output.write_text(defined.stdout, encoding="utf-8")
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checker", type=Path, required=True)
    parser.add_argument("--headers", type=Path, required=True)
    options = parser.parse_args()
    options.checker = options.checker.resolve()
    options.headers = options.headers.resolve()
    failures: list[str] = []

    def expect(label: str, condition: bool, detail: str = "") -> None:
        if not condition:
            failures.append(f"{label}: {detail}".rstrip(": "))

    with tempfile.TemporaryDirectory(prefix="uml-public-api-") as directory:
        root = Path(directory)

        fixture = root / "fixture" / "uml"
        fixture.mkdir(parents=True)
        (fixture / "fixture.h").write_text(FIXTURE_HEADER, encoding="utf-8")
        status, names = listed(options.checker, fixture)
        expect("fixture header", status == 0 and names == FIXTURE_FUNCTIONS,
               f"status {status}, read {names}")

        for label, text in BAD_HEADERS.items():
            bad = root / label.replace(" ", "-") / "uml"
            bad.mkdir(parents=True)
            (bad / "bad.h").write_text(text, encoding="utf-8")
            status, _ = listed(options.checker, bad)
            expect(f"{label} header", status != 0, "was accepted")
        status, _ = listed(options.checker, root / "missing")
        expect("missing header directory", status != 0, "was accepted")

        status, names = listed(options.checker, options.headers)
        expect("public headers", status == 0 and bool(names),
               f"status {status}")
        expected = compiler_functions(options.headers)
        expect("public headers against the compiler", names == expected,
               f"read {names}, the compiler declares {expected}")

        def check(label: str, members: list[list[str]],
                  expected_status: int, message: str) -> None:
            defined = build_archive(root, members)
            checked = run([sys.executable, "-B", str(options.checker),
                           "--headers", str(options.headers),
                           "--defined", str(defined)])
            if (checked.returncode != expected_status
                    or message not in checked.stdout):
                failures.append(
                    f"{label}: status {checked.returncode}, expected "
                    f"{expected_status} with {message!r}:\n{checked.stdout}")

        others = ["uml_internal_symbol", "main_helper"]
        check("exact API", [names, others], 0, "")
        check("missing function", [names[1:], others], 1,
              f"public API symbol _{names[0]} has 0 definitions")
        check("undeclared function", [names, ["uml_emulator_not_declared"]],
              1, "unexpected public API symbol: _uml_emulator_not_declared")
        check("defined twice", [names, [names[0]]], 1,
              f"public API symbol _{names[0]} has 2 definitions")

    if failures:
        raise SystemExit("\n".join(failures))
    print("the public API check follows the headers and refuses archives "
          "that define more, less or twice the API")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
