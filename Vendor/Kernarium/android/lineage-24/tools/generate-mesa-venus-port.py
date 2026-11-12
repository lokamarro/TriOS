#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the Kernarium Mesa port inside the materialized checkout.

The port carries two generated topics, and each has a subcommand here.

`protocol` writes the Venus protocol codec Mesa's Venus driver includes. The
guest build generates it out of tree through `-Dvenus-protocol-dir`, which
Soong cannot express, so the port carries the driver half of the codec at
`src/virtio/venus-protocol`. It runs venus-protocol's own generator from a
clean replay of the pinned protocol series against the registry of the pinned
Vulkan-Headers source, and refuses either input unless its tree is the one
`third_party/mesa-venus/metadata.env` records. The first line names both trees
instead of the replay commit, so the same pins always reproduce the same
bytes. `--check` compares instead of writing.

`soong` builds the Android.bp files. AOSP builds Mesa through
`meson2hermetic`, the upstream Meson fork that transpiles a meson build into
Android.bp files. This tool runs that converter with the checked project
configuration, repairs the converter's output where Soong needs more than
Meson does, drops every module the installed modules do not need, and then
scopes every remaining module name so the port cannot collide with the AOSP
`external/mesa3d` project, whose module names are otherwise identical and
global at the Make layer. The port builds the Venus Vulkan HAL and the Virgl
EGL and GLES libraries. It also deletes Mesa's legacy Android.mk makefiles:
Soong refuses a makefile under a denylisted directory, and the Android.bp
files alone describe the port.

The port builds for each host platform `soong-port.toml` names, one per
instruction set. The converter merges the platforms' differences into `select`
expressions, but it drops a generated source whose command differs between
platforms, such as the OpenGL dispatch code that bakes in the pointer size, and
Soong generates a source once for every instruction set. This tool therefore
converts each platform alone as well, keeps the first platform's generator as
the module the dropped name still means, adds the others as
`<name>_<arch>` modules, and has every library that uses one select its own
instruction set's generator through an `arch` block.

Soong accepts a `select` only on a property it declares configurable, and the
converter writes one on any property that differs between platforms. The tool
keeps a `select` on a property Soong makes configurable and moves the values
of every other property that Soong lets an `arch` block set into one. It
refuses a `select` on a property whose configurability it has not been told,
rather than leave Soong to reject the port: SOONG_CONFIGURABLE and
SOONG_ARCH_VARIANT list what Soong's own property structs declare.

Meson also compiles every C and C++ file with `-D_FILE_OFFSET_BITS=64`
(`Compiler.get_largefile_args`, returned by `CLikeCompiler.get_always_args` and
added by `Backend.generate_basic_compiler_args` in the pinned converter's Meson),
but the converter carries over only the project's and each target's own
arguments. Without it a 32-bit build keeps a 32-bit `off_t`, and the Venus
renderer's `mmap` of a DRM offset above 4 GiB fails with `EINVAL`. The tool adds
the define, once, to the defaults every compiled module uses, for each Soong
arch whose platform has four-byte pointers. A 64-bit arch already has a 64-bit
`off_t`, and Meson's define changes nothing there.

The converter is a builder toolchain input pinned by `third_party/meson2hermetic`:
an upstream commit and a local series. This tool refuses a converter checkout
whose tree is not the recorded final tree. The platform description it feeds
the converter is tracked beside the port configuration, so the same pins always
reproduce the same port.

Run each step on the maintained topic branch and commit its result into its
own topic: `protocol` into "Mesa Venus pair the generated protocol headers",
`soong` into "Mesa Venus generated Soong port". Then regenerate the series
with `regenerate-mesa-venus-patches.py`. Never edit the generated headers or
Android.bp files by hand.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path
from typing import NamedTuple

import pinned_source
# pinned_source puts the contract checks on sys.path.
from contract_checks import parse_assignments  # noqa: E402

PREFIX = "venus_"
REF_FIELDS = {
    "defaults",
    "header_libs",
    "static_libs",
    "whole_static_libs",
    "shared_libs",
    "generated_headers",
    "generated_sources",
    "libs",
    "required",
    "host_required",
    "target_required",
    "tools",
    "export_header_lib_headers",
    "export_static_lib_headers",
    "export_shared_lib_headers",
    "export_generated_headers",
    "default_applicable_licenses",
}
TOOL_FIELDS = {"tools", "tool_files"}
NAME_LINE = re.compile(r'^(\s*)name:\s*"([^"]+)"(,?)\s*$')
LIST_OPEN = re.compile(r"^\s*([a-z_0-9]+):\s*\[\s*$")
LIST_CLOSE = re.compile(r"^\s*\],?\s*$")
INLINE_LIST = re.compile(r'^(\s*)([a-z_0-9]+):\s*\[(.*)\](,?)\s*$')
QUOTED = re.compile(r'"([^"]*)"')
BLOCK_END = re.compile(r"^\}\s*$")
COMPAT = "venus_kernarium_compat_args"
COMPAT_TYPES = ("cc_library_shared", "cc_library_static", "cc_binary")
COMPAT_BLOCK = """
cc_defaults {
    name: "%s",
    vendor: true,
    cflags: [
%s    ],
    cppflags: [
%s    ],
%s}
"""
COMPAT_FLAGS = [
    "-Wno-gnu-pointer-arith",
]
# What Meson's compilers return from get_largefile_args() for every C and C++
# compile, which the converter does not carry over.
LARGE_FILE_FLAG = "-D_FILE_OFFSET_BITS=64"
BLOCK_TYPE = re.compile(r"^([a-z_0-9]+)\s*\{\s*$")


def checkout_files(root: Path, name: str) -> list[Path]:
    return [
        path
        for path in sorted(root.rglob(name))
        if ".git" not in path.parts
    ]


def bp_files(root: Path) -> list[Path]:
    return checkout_files(root, "Android.bp")


def remove_makefiles(root: Path) -> None:
    """Delete the checkout's legacy Android.mk makefiles."""
    makefiles = checkout_files(root, "Android.mk")
    for path in makefiles:
        path.unlink()
    print(f"removed {len(makefiles)} Android.mk files")


def block_tools(block: list[str]) -> set[str]:
    tools: set[str] = set()
    field: str | None = None
    for line in block:
        opened = LIST_OPEN.match(line)
        if opened is not None:
            field = opened.group(1)
            continue
        if LIST_CLOSE.match(line):
            field = None
            continue
        if field in TOOL_FIELDS:
            tools.update(QUOTED.findall(line))
    return {tool.lstrip(":") for tool in tools}


def rewrite_block(
    block: list[str], names: set[str], colon: re.Pattern[str], label: re.Pattern[str]
) -> list[str]:
    tools = block_tools(block) & names
    out: list[str] = []
    field: str | None = None
    for line in block:
        match = NAME_LINE.match(line)
        if match is not None and match.group(2) in names:
            line = f'{match.group(1)}name: "{PREFIX}{match.group(2)}"{match.group(3)}'
            out.append(line)
            continue
        opened = LIST_OPEN.match(line)
        if opened is not None:
            field = opened.group(1)
            out.append(line)
            continue
        if LIST_CLOSE.match(line):
            field = None
            out.append(line)
            continue
        if field in REF_FIELDS:
            line = QUOTED.sub(
                lambda m: f'"{PREFIX}{m.group(1)}"'
                if m.group(1) in names
                else m.group(0),
                line,
            )
        else:
            inline = INLINE_LIST.match(line)
            if inline is not None and inline.group(2) in REF_FIELDS:
                body = QUOTED.sub(
                    lambda m: f'"{PREFIX}{m.group(1)}"'
                    if m.group(1) in names
                    else m.group(0),
                    inline.group(3),
                )
                line = f"{inline.group(1)}{inline.group(2)}: [{body}]{inline.group(4)}"
        line = colon.sub(lambda m: ":" + PREFIX + m.group(1), line)
        line = label.sub(
            lambda m: f"$({m.group(1)} {PREFIX}{m.group(2)})"
            if m.group(2) in tools
            else m.group(0),
            line,
        )
        out.append(line)
    return out


# The converter's output for Mesa's GL path needs repairs that Soong requires
# and Meson does not. Each is a generic rule about the converter's output,
# never a Mesa source change, and runs before the names are scoped so the
# modules it adds are scoped with the rest.


class Blueprint:
    """One Android.bp file as top-level blocks of lines."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.original = path.read_text() if path.exists() else ""
        self.head: list[str] = []
        self.blocks: list[list[str]] = []
        current: list[str] | None = None
        for line in self.original.splitlines():
            if current is None and BLOCK_TYPE.match(line):
                current = [line]
                continue
            if current is None:
                if self.blocks:
                    self.blocks[-1].append(line)
                else:
                    self.head.append(line)
                continue
            current.append(line)
            if BLOCK_END.match(line):
                self.blocks.append(current)
                current = None
        if current is not None:
            raise SystemExit(f"generate-mesa-venus-port.py: {path} ends inside a block")

    def text(self) -> str:
        lines = list(self.head)
        for block in self.blocks:
            lines.extend(block)
        return "\n".join(lines) + "\n"

    def save(self) -> bool:
        updated = self.text()
        if updated == self.original:
            return False
        self.path.write_text(updated)
        return True


def block_kind(block: list[str]) -> str:
    match = BLOCK_TYPE.match(block[0])
    return match.group(1) if match is not None else ""


def block_name(block: list[str]) -> str | None:
    for line in block:
        match = NAME_LINE.match(line)
        if match is not None:
            return match.group(2)
    return None


def field_open(field: str) -> re.Pattern[str]:
    return re.compile(r"^    " + re.escape(field) + r":\s*\[(.*)$")


def list_items(block: list[str], field: str) -> list[str]:
    """The quoted items of a top-level list field, in order."""
    opener = field_open(field)
    items: list[str] = []
    inside = False
    for line in block:
        if not inside:
            match = opener.match(line)
            if match is None:
                continue
            rest = match.group(1)
            if "]" in rest:
                return QUOTED.findall(rest)
            inside = True
            continue
        if LIST_CLOSE.match(line):
            break
        items.extend(QUOTED.findall(line))
    return items


def set_list(block: list[str], field: str, items: list[str]) -> list[str]:
    """Replace, add or (when items is empty) remove a top-level list field."""
    opener = field_open(field)
    field_lines = [f"    {field}: ["] + [f'        "{item}",' for item in items] + ["    ],"]
    out: list[str] = []
    inside = False
    replaced = False
    for line in block:
        if inside:
            if LIST_CLOSE.match(line):
                inside = False
            continue
        match = opener.match(line)
        if match is not None:
            inside = "]" not in match.group(1)
            replaced = True
            if items:
                out.extend(field_lines)
            continue
        out.append(line)
    if not replaced and items:
        end = max(i for i, line in enumerate(out) if BLOCK_END.match(line))
        out[end:end] = field_lines
    return out


def load(root: Path) -> list[Blueprint]:
    return [Blueprint(path) for path in bp_files(root)]


def resolve_source(directory: Path, entry: str) -> str | None:
    """A source path as it exists below its module's directory.

    The converter writes some sources relative to the project instead of to
    the module; the shortest trailing part of the path that exists below the
    module's directory is the file meant."""
    if "*" in entry or (directory / entry).exists():
        return entry
    parts = entry.split("/")
    for index in range(1, len(parts)):
        candidate = "/".join(parts[index:])
        if (directory / candidate).exists():
            return candidate
    return None


def python_library_name(root: Path, directory: Path) -> str:
    relative = directory.relative_to(root).as_posix()
    return "pylib_" + re.sub(r"[^A-Za-z0-9]+", "_", relative)


def repair_python(root: Path, blueprints: list[Blueprint], license: str) -> int:
    """Give each converted Python generator the sources Soong accepts.

    Meson runs a generator script with its own directory, and those of the
    modules it imports, on the path, so they import as top-level modules and
    may sit anywhere in the tree next to their data. A python_binary_host
    takes only .py sources below its own directory. Its .py files in other
    directories, including those of a filegroup it lists, are therefore
    linked as one python_library_host per directory, whose modules import at
    top level, and a filegroup's .py files in its own directory become its
    sources; a
    data file among its sources is dropped, because the genrule that runs the
    generator is given the same files itself. A filegroup source that does
    not exist where the converter placed it is found below its directory, or
    dropped when it exists nowhere."""
    groups: dict[str, tuple[Path, list[str]]] = {}
    for bp in blueprints:
        for block in bp.blocks:
            if block_kind(block) == "filegroup":
                name = block_name(block)
                if name is not None:
                    groups[name] = (bp.path.parent, list_items(block, "srcs"))
    libraries: set[Path] = set()
    repaired = 0
    for bp in blueprints:
        directory = bp.path.parent
        for index, block in enumerate(bp.blocks):
            kind = block_kind(block)
            if kind not in ("python_binary_host", "filegroup"):
                continue
            srcs: list[str] = []
            links: list[str] = []
            for entry in list_items(block, "srcs"):
                if entry.startswith(":"):
                    group = groups.get(entry[1:])
                    if kind != "python_binary_host" or group is None or all(
                        source.endswith(".py") for source in group[1]
                    ):
                        srcs.append(entry)
                        continue
                    for source in group[1]:
                        found = resolve_source(group[0], source)
                        if found is None or not found.endswith(".py"):
                            continue
                        path = (group[0] / found).resolve()
                        if path.parent == directory.resolve():
                            srcs.append(path.name)
                        else:
                            libraries.add(path.parent)
                            links.append(python_library_name(root, path.parent))
                    continue
                found = resolve_source(directory, entry)
                if found is None:
                    continue
                if kind == "python_binary_host":
                    if not found.endswith(".py"):
                        continue
                    owner = (directory / found).resolve().parent
                    if owner != directory.resolve():
                        libraries.add(owner)
                        links.append(python_library_name(root, owner))
                        continue
                srcs.append(found)
            updated = set_list(block, "srcs", list(dict.fromkeys(srcs)))
            if links:
                merged = list_items(block, "libs")
                merged += [link for link in dict.fromkeys(links) if link not in merged]
                updated = set_list(updated, "libs", merged)
            if updated != block:
                bp.blocks[index] = updated
                repaired += 1
    for directory in sorted(libraries):
        path = directory / "Android.bp"
        bp = next((b for b in blueprints if b.path == path), None)
        if bp is None:
            bp = Blueprint(path)
            bp.head = [
                "package {",
                f'    default_applicable_licenses: ["{license}"],',
                "}",
            ]
            blueprints.append(bp)
        name = python_library_name(root, directory)
        if any(block_name(block) == name for block in bp.blocks):
            continue
        bp.blocks.append([
            "",
            "python_library_host {",
            f'    name: "{name}",',
            '    srcs: ["*.py"],',
            "}",
        ])
    return repaired


def repair_needed(blueprints: list[Blueprint]) -> int:
    """Enable a module the converter disabled although an enabled one links it.

    Meson can give a library and a test executable one name. The converter
    merges them into one disabled module that links itself; the library is
    what its users need, so the module is enabled and stops linking itself
    and every module that links it."""
    modules: dict[str, tuple[Blueprint, int, bool, set[str]]] = {}
    for bp in blueprints:
        for index, block in enumerate(bp.blocks):
            name = block_name(block)
            if name is None:
                continue
            enabled = not any(line.strip() == "enabled: false," for line in block)
            refs = set(QUOTED.findall("\n".join(
                line for line in block if NAME_LINE.match(line) is None
            )))
            modules[name] = (bp, index, enabled, refs)
    repaired = 0
    for name, (bp, index, enabled, _refs) in modules.items():
        if enabled:
            continue
        users = {
            other for other, module in modules.items()
            if other != name and module[2] and name in module[3]
        }
        if not users:
            continue
        drop = {name} | {
            other for other, module in modules.items()
            if other != name and name in module[3]
        }
        block = []
        for line in bp.blocks[index]:
            if line.strip() == "enabled: false,":
                continue
            quoted = re.match(r'^\s*"([^"]+)",?\s*$', line)
            if quoted is not None and quoted.group(1) in drop:
                continue
            block.append(line)
        bp.blocks[index] = block
        repaired += 1
    return repaired


def repair_nativebase(blueprints: list[Blueprint]) -> int:
    """Give a module that links libnativewindow the nativebase headers.

    libnativewindow's own headers include <nativebase/nativebase.h>, and a
    vendor module sees only what the LL-NDK library exports, which does not
    include those headers."""
    repaired = 0
    for bp in blueprints:
        for index, block in enumerate(bp.blocks):
            if "libnativewindow" not in list_items(block, "shared_libs"):
                continue
            headers = list_items(block, "header_libs")
            if "libnativebase_headers" in headers:
                continue
            bp.blocks[index] = set_list(
                block, "header_libs", headers + ["libnativebase_headers"]
            )
            repaired += 1
    return repaired


def repair_install(blueprints: list[Blueprint], gl_front_ends: set[str]) -> int:
    """Install each shared library where the loader that opens it looks.

    The converter installs every shared library in hw/, which is right for a
    HAL module such as vulkan.virtio. The Android EGL loader opens the EGL
    and GLES libraries from egl/, and the dynamic linker resolves a library
    another one links only on the namespace's search path, which holds the
    partition's library directory but not hw/ or egl/."""
    linked = {
        library
        for bp in blueprints
        for block in bp.blocks
        for library in list_items(block, "shared_libs")
    }
    repaired = 0
    for bp in blueprints:
        for index, block in enumerate(bp.blocks):
            if block_kind(block) != "cc_library_shared":
                continue
            name = block_name(block)
            if name in gl_front_ends:
                wanted = '"egl"'
            elif name in linked:
                wanted = None
            else:
                continue
            updated: list[str] = []
            for line in block:
                match = re.match(r'^(\s*)relative_install_path:\s*"[^"]*",\s*$', line)
                if match is None:
                    updated.append(line)
                elif wanted is not None:
                    updated.append(f"{match.group(1)}relative_install_path: {wanted},")
            if updated != block:
                bp.blocks[index] = updated
                repaired += 1
    return repaired


class Port:
    """The parts of soong-port.toml this tool reads; the converter reads the rest."""

    def __init__(self, config: Path, hal_module: str) -> None:
        table = tomllib.loads(config.read_text())
        license = table.get("copyright", {}).get("license_name")
        if not isinstance(license, str) or not license:
            raise SystemExit("generate-mesa-venus-port.py: the port names no license")
        self.license = license
        # The renamed targets keep their names unscoped: the HAL module that
        # ro.hardware.vulkan names, and the EGL and GLES libraries the Android
        # EGL loader resolves through ro.hardware.egl.
        self.keep = set(table.get("target_renames", {}).values())
        if hal_module not in self.keep:
            raise SystemExit(
                f"generate-mesa-venus-port.py: the port does not rename a target to {hal_module}"
            )
        self.gl_front_ends = self.keep - {hal_module}
        self.warning_exceptions = warning_exceptions(table)
        self.host_platforms, self.platform_arch = host_platforms(table)
        # Set once the platform description is known: the Soong arches whose
        # `off_t` is 32 bits without the large-file define.
        self.large_file_arches: list[str] = []


def host_platforms(table: dict[str, object]) -> tuple[list[str], dict[str, str]]:
    """The host platforms the port converts, and the Soong arch of each."""
    configs = table.get("config", [])
    if len(configs) != 1:
        raise SystemExit("generate-mesa-venus-port.py: the port needs exactly one [[config]]")
    platforms = list(configs[0].get("platforms", {}).get("host_platforms", []))
    if not platforms:
        raise SystemExit("generate-mesa-venus-port.py: the port names no host platform")
    arches = dict(table.get("kernarium", {}).get("platform_arch", {}))
    if len(platforms) > 1 and (
        set(arches) != set(platforms) or len(set(arches.values())) != len(arches)
    ):
        raise SystemExit(
            "generate-mesa-venus-port.py: [kernarium.platform_arch] must give each host "
            "platform its own Soong arch"
        )
    return platforms, arches


def pointer_size(platform: dict[str, object]) -> int:
    """The size of `void*` the platform description gives the converter, which
    answers eight bytes for any type it is not told."""
    sizes = []
    for language in ("c", "cpp"):
        table = platform.get(language, {})
        known = table.get("sizeof", {}).get("sizes", {}) if isinstance(table, dict) else {}
        sizes.extend(known.get(spelling, 8) for spelling in ("void*", "void *"))
    if len(set(sizes)) != 1:
        raise SystemExit(
            f"generate-mesa-venus-port.py: platform {platform.get('name')} gives void* "
            f"different sizes: {sorted(set(sizes))}"
        )
    return sizes[0]


def large_file_arches(platforms: Path, port: Port) -> list[str]:
    """The Soong arches of the host platforms with four-byte pointers, whose
    `off_t` stays 32 bits unless the large-file define is on the command line."""
    described = {
        platform["name"]: platform for platform in tomllib.loads(platforms.read_text())["platform"]
    }
    arches: list[str] = []
    for name in port.host_platforms:
        if name not in described:
            raise SystemExit(f"generate-mesa-venus-port.py: {platforms} does not define {name}")
        size = pointer_size(described[name])
        if size == 8:
            continue
        arch = port.platform_arch.get(name)
        if size != 4 or arch is None:
            raise SystemExit(
                f"generate-mesa-venus-port.py: platform {name} has {size}-byte pointers and "
                "needs [kernarium.platform_arch] to name its arch"
            )
        arches.append(arch)
    return arches


def repair(root: Path, blueprints: list[Blueprint], port: Port) -> None:
    python = repair_python(root, blueprints, port.license)
    needed = repair_needed(blueprints)
    nativebase = repair_nativebase(blueprints)
    installs = repair_install(blueprints, port.gl_front_ends)
    warnings = repair_warnings(blueprints, port.warning_exceptions)
    print(
        f"repaired {python} Python modules, {needed} disabled libraries, "
        f"{nativebase} nativewindow users, {installs} install paths and "
        f"{warnings} warning exceptions"
    )


FLAG_FIELDS = ("cflags", "conlyflags", "cppflags")


def git(directory: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(directory), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        raise SystemExit(
            f"generate-mesa-venus-port.py: git {' '.join(arguments)} failed in "
            f"{directory}: {result.stderr.strip()}"
        )
    return result.stdout.strip()


def verify_converter(android_root: Path, converter: Path) -> None:
    """Refuse any converter but the pinned upstream commit plus the series."""
    metadata = parse_assignments(
        android_root / "third_party/meson2hermetic/metadata.env",
        lambda message: SystemExit(f"generate-mesa-venus-port.py: {message}"),
    )
    expected = metadata["MESON2HERMETIC_FINAL_TREE"]
    checkout = converter.resolve().parent
    tree = git(checkout, "rev-parse", "HEAD^{tree}")
    if tree != expected:
        raise SystemExit(
            f"generate-mesa-venus-port.py: the converter at {checkout} has tree "
            f"{tree}, not the recorded {expected}; replay "
            "third_party/meson2hermetic/series onto its pinned commit"
        )
    if git(checkout, "status", "--porcelain", "--untracked-files=no"):
        raise SystemExit(
            f"generate-mesa-venus-port.py: the converter at {checkout} has local changes"
        )


def warning_exceptions(table: dict[str, object]) -> list[dict[str, object]]:
    """The port's per-module warning exceptions, which the converter ignores."""
    entries = table.get("kernarium", {}).get("warning_exception", [])
    for entry in entries:
        if not isinstance(entry.get("module"), str) or not entry.get("reason"):
            raise SystemExit(
                "generate-mesa-venus-port.py: a warning exception needs a module "
                "and a reason"
            )
        flags = [field for field in FLAG_FIELDS if field in entry]
        if not flags or set(entry) - {"module", "reason", *FLAG_FIELDS}:
            raise SystemExit(
                f"generate-mesa-venus-port.py: the warning exception for "
                f"{entry['module']} names no flags or an unknown field"
            )
    return entries


def repair_warnings(blueprints: list[Blueprint], exceptions: list[dict[str, object]]) -> int:
    """Give each module named by a warning exception its extra flags.

    Soong makes every warning an error in a project outside device/ and
    vendor/, which Meson's build of Mesa does not, so a warning Mesa's own
    build tolerates in one module needs an exception there."""
    repaired = 0
    for entry in exceptions:
        module = entry["module"]
        found = False
        for bp in blueprints:
            for index, block in enumerate(bp.blocks):
                if block_name(block) != module:
                    continue
                found = True
                for field in FLAG_FIELDS:
                    if field not in entry:
                        continue
                    items = list_items(bp.blocks[index], field)
                    items += [flag for flag in entry[field] if flag not in items]
                    bp.blocks[index] = set_list(bp.blocks[index], field, items)
                repaired += 1
        if not found:
            raise SystemExit(
                f"generate-mesa-venus-port.py: the warning exception names "
                f"{module}, which the port does not define"
            )
    return repaired


def convert(
    args: argparse.Namespace, config: Path | None = None, checkout: Path | None = None
) -> None:
    config = config or args.config
    checkout = checkout or args.checkout
    command = [
        sys.executable,
        str(args.converter),
        "convert",
        f"--config={config}",
        f"--platforms={args.platforms}",
        f"--project-dir={checkout}",
        f"--output-dir={checkout}",
        f"--dependencies={args.dependencies}",
    ]
    subprocess.run(command, cwd=checkout, check=True)


HOST_PLATFORMS = re.compile(r"^host_platforms\s*=\s*\[[^\]]*\]\s*$", re.MULTILINE)
TOP_LIST = re.compile(r"^    ([a-z_0-9]+):\s*\[\s*$")
ARCH_FIELDS = {"srcs", "generated_headers", "generated_sources", "export_generated_headers"}
Genrules = dict[tuple[str, str], list[str]]

# Soong accepts `select` only on a property whose Go type is
# proptools.Configurable, and `arch: {...}` only on one tagged arch_variant.
# Read from the property structs of build/soong at 5ea23647 (the LineageOS
# lineage-24.0 workspace the Android build uses):
#   cc:      cc/compiler.go BaseCompilerProperties, cc/linker.go
#            BaseLinkerProperties, cc/library.go FlagExporterProperties and
#            cc/cc.go BaseProperties;
#   genrule: genrule/genrule.go generatorProperties and genRuleProperties.
# Every other module type the port holds (filegroup, python, package, license)
# takes no select on a property the converter writes.
SELECT_GROUP = {
    "cc_binary": "cc",
    "cc_defaults": "cc",
    "cc_library_headers": "cc",
    "cc_library_shared": "cc",
    "cc_library_static": "cc",
    "cc_object": "cc",
    "genrule": "genrule",
}
SOONG_CONFIGURABLE = {
    "cc": frozenset({
        "cflags", "cppflags", "device_first_generated_headers", "exclude_srcs",
        "export_cflags", "export_header_lib_headers", "export_include_dirs",
        "generated_headers", "header_libs", "include_dirs", "ldflags",
        "local_include_dirs", "min_sdk_version", "shared_libs", "srcs",
        "static_libs", "whole_static_libs",
    }),
    "genrule": frozenset({
        "cmd", "common_os_srcs", "device_common_srcs", "device_first_srcs",
        "host_first_srcs", "host_second_srcs", "srcs",
    }),
}
SOONG_ARCH_VARIANT = {
    "cc": frozenset({
        "allow_undefined_symbols", "asflags", "clang", "clang_asflags",
        "clang_cflags", "conlyflags", "exclude_generated_sources",
        "exclude_runtime_libs", "export_generated_headers",
        "export_shared_lib_headers", "export_static_lib_headers",
        "export_system_include_dirs", "generated_sources", "group_static_libs",
        "host_ldlibs", "ignore_max_page_size", "installable", "instruction_set",
        "no_crt_pad_segment", "no_libcrt", "nocrt", "optimize_for_size", "rtti",
        "runtime_libs", "system_shared_libs", "tidy_disabled_srcs",
        "tidy_timeout_srcs",
    }),
    "genrule": frozenset({"exclude_srcs", "out"}),
}
SELECT_CLOSE = "    ] + select(arch(), {"
SELECT_END = "    }),"
BRANCH_OPEN = re.compile(r'^        (?:"([a-z0-9_]+)"|(default)): \[(.*)$')
ARCH_OPEN = "    arch: {"


class Select(NamedTuple):
    """A top-level list followed by `+ select(arch(), {...})`, as the converter
    writes it: `first` is the line closing the list, `last` the line closing
    the select, and `branches` maps each arch (or `default`) to its items."""

    field: str
    first: int
    last: int
    branches: dict[str, list[str]]


def selects(block: list[str]) -> list[Select]:
    found: list[Select] = []
    field: str | None = None
    index = 0
    while index < len(block):
        line = block[index]
        opened = TOP_LIST.match(line)
        if opened is not None:
            field = opened.group(1)
        if "select(" not in line:
            index += 1
            continue
        if line != SELECT_CLOSE or field is None:
            raise SystemExit(
                f"generate-mesa-venus-port.py: {block_name(block)} has a select this tool "
                f"cannot read: {line.strip()}"
            )
        branches: dict[str, list[str]] = {}
        end = index + 1
        while end < len(block) and block[end] != SELECT_END:
            branch = BRANCH_OPEN.match(block[end])
            if branch is None:
                raise SystemExit(
                    f"generate-mesa-venus-port.py: {block_name(block)} has a select branch "
                    f"this tool cannot read: {block[end].strip()}"
                )
            items = QUOTED.findall(branch.group(3))
            if "]" not in branch.group(3):
                end += 1
                while end < len(block) and block[end] != "        ],":
                    items.extend(QUOTED.findall(block[end]))
                    end += 1
            branches[branch.group(1) or "default"] = items
            end += 1
        if end >= len(block):
            raise SystemExit(
                f"generate-mesa-venus-port.py: {block_name(block)} has an unclosed select"
            )
        found.append(Select(field, index, end, branches))
        index = end + 1
    return found


def take_arch(block: list[str]) -> tuple[list[str], dict[str, dict[str, list[str]]]]:
    """Remove the block's `arch` field, as arch_block writes it, and return it."""
    if ARCH_OPEN not in block:
        return block, {}
    start = block.index(ARCH_OPEN)
    end = next((i for i in range(start + 1, len(block)) if block[i] == "    },"), None)
    if end is None:
        raise SystemExit(f"generate-mesa-venus-port.py: {block_name(block)} has an unclosed arch")
    arches: dict[str, dict[str, list[str]]] = {}
    arch: str | None = None
    field: str | None = None
    for line in block[start + 1:end]:
        if (m := re.match(r"^        ([a-z0-9_]+): \{$", line)) is not None:
            arch = m.group(1)
            arches.setdefault(arch, {})
        elif (m := re.match(r"^            ([a-z_0-9]+): \[$", line)) is not None and arch:
            field = m.group(1)
            arches[arch].setdefault(field, [])
        elif line == "            ]," and field:
            field = None
        elif line == "        }," and arch and field is None:
            arch = None
        elif (m := re.match(r'^                "([^"]*)",$', line)) is not None and arch and field:
            arches[arch][field].append(m.group(1))
        else:
            raise SystemExit(
                f"generate-mesa-venus-port.py: {block_name(block)} has an arch block "
                f"this tool cannot read: {line.strip()}"
            )
    return block[:start] + block[end + 1:], arches


def add_arch(block: list[str], moved: dict[str, dict[str, list[str]]]) -> list[str]:
    """Add per-arch field values to the block's `arch` field, making it if absent."""
    block, arches = take_arch(block)
    for arch in moved:
        for field, items in moved[arch].items():
            arches.setdefault(arch, {}).setdefault(field, []).extend(items)
    end = max(i for i, line in enumerate(block) if BLOCK_END.match(line))
    block[end:end] = arch_block(arches)
    return block


def lift_arch_selects(blueprints: list[Blueprint], port: Port) -> int:
    """Move the select on a property Soong does not make configurable into an
    `arch` block; keep one on a configurable property. Returns the number moved."""
    arches = set(port.platform_arch.values())
    lifted = 0
    for bp in blueprints:
        for index, block in enumerate(bp.blocks):
            found = selects(block)
            if not found:
                continue
            group = SELECT_GROUP.get(block_kind(block), "")
            lines = list(block)
            moved: dict[str, dict[str, list[str]]] = {}
            for choice in reversed(found):
                if choice.field in SOONG_CONFIGURABLE.get(group, frozenset()):
                    continue
                if choice.field not in SOONG_ARCH_VARIANT.get(group, frozenset()):
                    raise SystemExit(
                        f"generate-mesa-venus-port.py: {block_name(block)} selects "
                        f"{choice.field}, which Soong neither makes configurable nor lets an "
                        f"arch block set; add it to the tables with Soong's evidence"
                    )
                unknown = set(choice.branches) - arches - {"default"}
                if unknown:
                    raise SystemExit(
                        f"generate-mesa-venus-port.py: {block_name(block)} selects "
                        f"{choice.field} on {sorted(unknown)}, which is no host platform's arch"
                    )
                for arch in sorted(arches):
                    items = choice.branches.get(arch, choice.branches.get("default", []))
                    if items:
                        moved.setdefault(arch, {})[choice.field] = items
                start = choice.first
                if (m := TOP_LIST.match(lines[start - 1])) is not None and m.group(1) == choice.field:
                    start -= 1  # nothing precedes the select: drop the empty list too
                    lines[start:choice.last + 1] = []
                else:
                    lines[start:choice.last + 1] = ["    ],"]
                lifted += 1
            if moved:
                lines = add_arch(lines, moved)
            bp.blocks[index] = lines
    return lifted


def audit_selects(blueprints: list[Blueprint]) -> int:
    """Refuse a select on a property Soong does not make configurable."""
    count = 0
    for bp in blueprints:
        for block in bp.blocks:
            group = SELECT_GROUP.get(block_kind(block), "")
            for choice in selects(block):
                if choice.field not in SOONG_CONFIGURABLE.get(group, frozenset()):
                    raise SystemExit(
                        f"generate-mesa-venus-port.py: {block_name(block)} still selects "
                        f"{choice.field}, which Soong does not make configurable"
                    )
                count += 1
    return count


def platform_genrules(args: argparse.Namespace, port: Port) -> dict[str, Genrules]:
    """The genrule blocks the converter writes for each host platform alone.

    The converter drops a generated source whose command differs between the
    platforms it merges, so the merged output cannot say what each platform
    needs. A copy of the pristine checkout holds each platform's run."""
    variants: dict[str, Genrules] = {}
    with tempfile.TemporaryDirectory(prefix="mesa-venus-platforms-") as scratch:
        for platform in port.host_platforms:
            copy = Path(scratch) / platform
            shutil.copytree(
                args.checkout, copy, symlinks=True, ignore=shutil.ignore_patterns(".git")
            )
            text, count = HOST_PLATFORMS.subn(
                f'host_platforms = ["{platform}"]', args.config.read_text(), count=1
            )
            if count != 1:
                raise SystemExit(
                    "generate-mesa-venus-port.py: the port configuration has no "
                    "host_platforms line to narrow"
                )
            config = Path(scratch) / f"{platform}.toml"
            config.write_text(text)
            convert(args, config, copy)
            variants[platform] = {
                (bp.path.relative_to(copy).as_posix(), name): block
                for bp in load(copy)
                for block in bp.blocks
                if block_kind(block) == "genrule" and (name := block_name(block)) is not None
            }
    return variants


def retarget(block: list[str], name: str, new_name: str) -> list[str]:
    out = []
    for line in block:
        match = NAME_LINE.match(line)
        if match is not None and match.group(2) == name:
            line = f'{match.group(1)}name: "{new_name}"{match.group(3)}'
        out.append(line)
    return out


def trimmed(block: list[str]) -> list[str]:
    end = len(block)
    while end and not block[end - 1].strip():
        end -= 1
    return block[:end]


def top_list_end(block: list[str], start: int) -> tuple[int, bool]:
    """The line that closes the top-level list opened at `start`, and whether
    the field ends there: a list followed by a `select` does not."""
    for index in range(start + 1, len(block)):
        line = block[index]
        if line.startswith("    ]") and not line.startswith("     "):
            return index, line.strip() == "],"
    raise SystemExit("generate-mesa-venus-port.py: a list field is not closed")


def split_consumer(
    block: list[str], renames: dict[str, dict[str, str]]
) -> tuple[list[str], dict[str, dict[str, list[str]]]]:
    """Take the references to arch-specific generators out of a module's lists.

    Returns the module without them and, per arch, the fields that name that
    arch's generator instead."""
    moved: dict[str, dict[str, list[str]]] = {}
    out = list(block)
    for field in sorted({m.group(1) for line in block if (m := TOP_LIST.match(line))}):
        start = next(
            i for i, line in enumerate(out)
            if (m := TOP_LIST.match(line)) is not None and m.group(1) == field
        )
        end, plain = top_list_end(out, start)
        refs = [
            item for line in out[start + 1:end] for item in QUOTED.findall(line)
            if item.removeprefix(":") in renames
        ]
        if not plain:
            body = out[start:]
            for index in range(1, len(body)):
                if re.match(r"^    [a-z_0-9]+:", body[index]):
                    body = body[:index]
                    break
            if any(item.removeprefix(":") in renames for line in body for item in QUOTED.findall(line)):
                raise SystemExit(
                    f"generate-mesa-venus-port.py: {block_name(block)} names an arch-specific "
                    f"generator inside a conditional {field}"
                )
            continue
        if not refs:
            continue
        if field not in ARCH_FIELDS or block_kind(block) not in COMPAT_TYPES:
            raise SystemExit(
                f"generate-mesa-venus-port.py: {block_name(block)} names an arch-specific "
                f"generator in its {field}, which an arch block cannot carry"
            )
        items = list_items(out, field)
        for item in items:
            base = item.removeprefix(":")
            if base in renames:
                prefix = ":" if item.startswith(":") else ""
                for arch, name in renames[base].items():
                    moved.setdefault(arch, {}).setdefault(field, []).append(prefix + name)
        out = set_list(out, field, [i for i in items if i.removeprefix(":") not in renames])
    return out, moved


def arch_block(moved: dict[str, dict[str, list[str]]]) -> list[str]:
    lines = ["    arch: {"]
    for arch in sorted(moved):
        lines.append(f"        {arch}: {{")
        for field in sorted(moved[arch]):
            lines.append(f"            {field}: [")
            lines.extend(f'                "{item}",' for item in moved[arch][field])
            lines.append("            ],")
        lines.append("        },")
    lines.append("    },")
    return lines


def split_arch_genrules(
    root: Path, blueprints: list[Blueprint], variants: dict[str, Genrules], port: Port
) -> int:
    """Give every platform its own copy of a generator whose command differs.

    The first host platform keeps the generator's name; each other platform's
    copy is named `<name>_<arch>`. Libraries that use the generator select by
    arch. Returns the number of generators split."""
    default, *others = port.host_platforms
    shared = set(variants[default])
    for platform in others:
        shared &= set(variants[platform])
    differing = sorted(
        key for key in shared
        if len({tuple(variants[platform][key]) for platform in port.host_platforms}) > 1
    )
    files = {bp.path.relative_to(root).as_posix(): bp for bp in blueprints}
    renames: dict[str, dict[str, str]] = {}
    for path, name in differing:
        bp = files.get(path)
        if bp is None:
            raise SystemExit(f"generate-mesa-venus-port.py: the converter wrote no {path}")
        arch_names = {port.platform_arch[default]: name}
        group = [trimmed(variants[default][(path, name)])]
        for platform in others:
            arch = port.platform_arch[platform]
            arch_names[arch] = f"{name}_{arch}"
            group.append(retarget(trimmed(variants[platform][(path, name)]), name, arch_names[arch]))
        renames[name] = arch_names
        # A block is one module; it carries the blank lines that follow it.
        copies = [block + [""] for block in group]
        index = next((i for i, b in enumerate(bp.blocks) if block_name(b) == name), None)
        if index is None:
            if bp.blocks and bp.blocks[-1] and bp.blocks[-1][-1].strip():
                bp.blocks[-1].append("")
            bp.blocks.extend(copies)
        else:
            blanks = len(bp.blocks[index]) - len(trimmed(bp.blocks[index]))
            copies[-1] = copies[-1][:-1] + [""] * blanks
            bp.blocks[index:index + 1] = copies
    if not renames:
        return 0
    for bp in blueprints:
        for index, block in enumerate(bp.blocks):
            if block_kind(block) == "genrule":
                continue
            updated, moved = split_consumer(block, renames)
            if not moved:
                continue
            bp.blocks[index] = add_arch(updated, moved)
    return len(renames)


PROTOCOL_DIRECTORY = "src/virtio/venus-protocol"
PROTOCOL_HEADERS = "vn_protocol_driver*.h"


def pinned_checkout(path: Path, label: str, tree: str) -> Path:
    """A clean Git checkout root whose tree is the pinned one."""
    path = path.resolve()
    if not path.is_dir():
        raise SystemExit(f"generate-mesa-venus-port.py: the {label} is missing: {path}")
    if Path(git(path, "rev-parse", "--show-toplevel")).resolve() != path:
        raise SystemExit(
            f"generate-mesa-venus-port.py: the {label} is not a Git checkout root: {path}"
        )
    if git(path, "status", "--porcelain", "--untracked-files=all"):
        raise SystemExit(f"generate-mesa-venus-port.py: the {label} has local changes")
    actual = git(path, "rev-parse", "HEAD^{tree}")
    if actual != tree:
        raise SystemExit(
            f"generate-mesa-venus-port.py: the {label} at {path} has tree {actual}, "
            f"not the pinned {tree}"
        )
    return path


def generate_protocol(args: argparse.Namespace, metadata: dict[str, str]) -> None:
    """Write, or with --check compare, the port's Venus protocol codec."""
    target = args.checkout / PROTOCOL_DIRECTORY
    if target.is_symlink() or not target.is_dir():
        raise SystemExit(
            f"generate-mesa-venus-port.py: the checkout has no {PROTOCOL_DIRECTORY}"
        )
    for key in ("MESA_VENUS_PROTOCOL_TREE", "MESA_VENUS_VULKAN_HEADERS_TREE"):
        if key not in metadata:
            raise SystemExit(f"generate-mesa-venus-port.py: the metadata lacks {key}")
    protocol = pinned_checkout(
        args.protocol_source, "venus-protocol source", metadata["MESA_VENUS_PROTOCOL_TREE"]
    )
    headers = pinned_checkout(
        args.vulkan_headers,
        "Vulkan-Headers source",
        metadata["MESA_VENUS_VULKAN_HEADERS_TREE"],
    )
    banner = pinned_source.venus_protocol_banner(metadata)
    with tempfile.TemporaryDirectory(prefix="venus-protocol-") as temporary:
        output = Path(temporary) / "codec"
        output.mkdir()
        banner_file = Path(temporary) / "banner"
        # venus-protocol's banner template ends in a blank line.
        banner_file.write_text(banner + "\n\n")
        result = subprocess.run(
            [
                sys.executable,
                "-B",
                str(protocol / "vn_protocol.py"),
                "--outdir",
                str(output),
                "--registry",
                str(headers / "registry/vk.xml"),
                "--banner",
                str(banner_file),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode != 0:
            raise SystemExit(
                "generate-mesa-venus-port.py: the venus-protocol generator failed: "
                f"{(result.stderr or result.stdout).strip()}"
            )
        if git(protocol, "status", "--porcelain", "--untracked-files=all"):
            raise SystemExit(
                "generate-mesa-venus-port.py: the venus-protocol generator wrote into "
                "its source"
            )
        generated = {path.name: path.read_bytes() for path in output.glob(PROTOCOL_HEADERS)}
        main_header = generated.get("vn_protocol_driver.h", b"")
        if main_header.split(b"\n", 1)[0] != banner.encode():
            raise SystemExit(
                "generate-mesa-venus-port.py: the generator did not write the pinned "
                "banner into vn_protocol_driver.h"
            )
        existing = {path.name: path for path in target.glob(PROTOCOL_HEADERS)}
        stale = sorted(existing.keys() - generated.keys())
        changed = sorted(
            name for name, payload in generated.items()
            if name not in existing or existing[name].read_bytes() != payload
        )
        if args.check:
            if stale or changed:
                raise SystemExit(
                    "generate-mesa-venus-port.py: the checkout's protocol codec differs "
                    f"from the pinned generation: changed {changed}, stale {stale}"
                )
            print(f"the {len(generated)} protocol headers match the pinned generation")
            return
        for name in stale:
            existing[name].unlink()
        for name in changed:
            (target / name).write_bytes(generated[name])
    print(
        f"wrote {len(generated)} protocol headers: {len(changed)} changed, "
        f"{len(stale)} removed"
    )


def generate_soong(args: argparse.Namespace, metadata: dict[str, str]) -> None:
    args.config = args.android_root / "third_party/mesa-venus/soong-port.toml"
    if not args.config.is_file():
        raise SystemExit("generate-mesa-venus-port.py: the port configuration is missing")
    if args.platforms is None:
        args.platforms = args.android_root / "third_party/mesa-venus/soong-platforms.toml"
    if not args.platforms.is_file():
        raise SystemExit("generate-mesa-venus-port.py: the platform description is missing")
    if args.dependencies is None:
        args.dependencies = (
            args.android_root / "third_party/mesa-venus/soong-dependencies.toml"
        )
    if not args.dependencies.is_file():
        raise SystemExit("generate-mesa-venus-port.py: the dependency map is missing")
    port = Port(args.config, metadata.get("MESA_VENUS_HAL_MODULE", ""))
    port.large_file_arches = large_file_arches(args.platforms, port)
    verify_converter(args.android_root, args.converter)
    variants = platform_genrules(args, port) if len(port.host_platforms) > 1 else {}
    convert(args)
    remove_makefiles(args.checkout)
    blueprints = load(args.checkout)
    lifted = lift_arch_selects(blueprints, port)
    if lifted:
        print(f"moved {lifted} selects on properties Soong does not make configurable into arch blocks")
    if variants:
        split = split_arch_genrules(args.checkout, blueprints, variants, port)
        print(f"split {split} generators that differ between host platforms")
    repair(args.checkout, blueprints, port)
    blueprints = prune(blueprints, port.keep)
    scope(args.checkout, blueprints, port.keep, port.large_file_arches)
    kept = audit_selects(blueprints)
    if kept:
        print(f"kept {kept} selects on properties Soong makes configurable")
    saved = sum(1 for bp in blueprints if bp.save())
    print(f"wrote {saved} Android.bp files")


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate the Kernarium Mesa port")
    steps = parser.add_subparsers(dest="step", required=True)
    protocol = steps.add_parser(
        "protocol", help="write the paired Venus protocol codec"
    )
    protocol.add_argument(
        "--protocol-source",
        type=Path,
        required=True,
        help="clean venus-protocol replay at MESA_VENUS_PROTOCOL_TREE",
    )
    protocol.add_argument(
        "--vulkan-headers",
        type=Path,
        required=True,
        help="clean Vulkan-Headers checkout at MESA_VENUS_VULKAN_HEADERS_TREE",
    )
    protocol.add_argument(
        "--check",
        action="store_true",
        help="compare the checkout's codec with the generation; write nothing",
    )
    soong = steps.add_parser("soong", help="write the Android.bp files")
    soong.add_argument(
        "--converter",
        type=Path,
        required=True,
        help="builder-local meson2hermetic meson.py",
    )
    soong.add_argument(
        "--platforms",
        type=Path,
        help="platform description override; defaults to the port's own",
    )
    soong.add_argument(
        "--dependencies",
        type=Path,
        help="dependency map override; defaults to the port's own map",
    )
    for step in (protocol, soong):
        step.add_argument("--android-root", type=Path, required=True)
        step.add_argument(
            "--checkout",
            type=Path,
            required=True,
            help="materialized external/mesa3d-venus checkout",
        )
    args = parser.parse_args()
    args.android_root = args.android_root.resolve()
    args.checkout = args.checkout.resolve()
    metadata = parse_assignments(
        args.android_root / "third_party/mesa-venus/metadata.env",
        lambda message: SystemExit(f"generate-mesa-venus-port.py: {message}"),
    )
    if args.step == "protocol":
        generate_protocol(args, metadata)
    else:
        generate_soong(args, metadata)


LABEL = re.compile(r":([A-Za-z0-9_.+-]+)")
LOCATION = re.compile(r"\$\(locations?\s+([^)\s]+)\)")


def block_refs(block: list[str]) -> set[str]:
    """Every name a block could refer to: a quoted string, a `:name` label or a
    `$(location name)` in a genrule command. Any other string that happens to
    equal a module name only keeps that module."""
    text = "\n".join(line for line in block if NAME_LINE.match(line) is None)
    return (
        set(QUOTED.findall(text))
        | set(LABEL.findall(text))
        | set(LOCATION.findall(text))
    )


def prune(blueprints: list[Blueprint], keep: set[str]) -> list[Blueprint]:
    """Delete every module that no installed module needs.

    The converter describes each Meson target it meets, among them tools,
    tests, Windows module definitions and GLX tables the product never builds
    or links. A module stays when a kept module or a package declaration
    refers to it, directly or through another module that stays; a file left
    without modules is deleted."""
    modules: dict[str, set[str]] = {}
    roots = set(keep)
    for bp in blueprints:
        for block in bp.blocks:
            name = block_name(block)
            if name is None:
                roots |= block_refs(block)
            else:
                modules[name] = block_refs(block)
    missing = keep - modules.keys()
    if missing:
        raise SystemExit(
            f"generate-mesa-venus-port.py: the converter defined no {sorted(missing)[0]}"
        )
    reached: set[str] = set()
    pending = [name for name in roots if name in modules]
    while pending:
        name = pending.pop()
        if name not in reached:
            reached.add(name)
            pending.extend(ref for ref in modules[name] if ref in modules)
    kept: list[Blueprint] = []
    removed = 0
    for bp in blueprints:
        bp.blocks = [
            block for block in bp.blocks
            if (name := block_name(block)) is None or name in reached
        ]
        if any(block_name(block) is not None for block in bp.blocks):
            # A block carries the blank lines that follow it.
            while not bp.blocks[-1][-1].strip():
                bp.blocks[-1].pop()
            kept.append(bp)
        else:
            bp.path.unlink(missing_ok=True)
            removed += 1
    print(
        f"pruned {len(modules) - len(reached)} of {len(modules)} modules and "
        f"{removed} Android.bp files"
    )
    return kept


def compat_block(wide_offset_arches: list[str]) -> list[str]:
    """The defaults every compiled module uses: the AOSP-strictness flags, and
    Meson's large-file define for each arch that needs it."""
    flags = "".join(f'        "{flag}",\n' for flag in COMPAT_FLAGS)
    arch = "\n".join(arch_block({a: {"cflags": [LARGE_FILE_FLAG]} for a in sorted(wide_offset_arches)}))
    text = COMPAT_BLOCK % (COMPAT, flags, flags, arch + "\n" if wide_offset_arches else "")
    return text.split("\n")[:-1]


def scope(
    root: Path, blueprints: list[Blueprint], keep: set[str], wide_offset_arches: list[str]
) -> None:
    """Prefix every generated module name, and every reference to one.

    Each compiled module also gains the port's AOSP-strictness defaults, and
    for a 32-bit arch Meson's large-file define."""
    names = {
        name
        for bp in blueprints
        for block in bp.blocks
        if (name := block_name(block)) is not None and name not in keep
    }
    already = {name for name in names if name.startswith(PREFIX)}
    if already:
        raise SystemExit(
            f"generate-mesa-venus-port.py: module names are already scoped: "
            f"{sorted(already)[:3]}"
        )
    alternation = "|".join(re.escape(n) for n in sorted(names, key=len, reverse=True))
    colon = re.compile(r":(" + alternation + r")(?![A-Za-z0-9_.])")
    label = re.compile(r"\$\((locations?)\s+(" + alternation + r")\)")
    changed = 0
    for bp in blueprints:
        before = bp.text()
        bp.head = rewrite_block(bp.head, names, colon, label)
        for index, block in enumerate(bp.blocks):
            block = rewrite_block(block, names, colon, label)
            if block_kind(block) in COMPAT_TYPES:
                block = set_list(block, "defaults", list_items(block, "defaults") + [COMPAT])
            bp.blocks[index] = block
        changed += bp.text() != before
    top = next((bp for bp in blueprints if bp.path == root / "Android.bp"), None)
    if top is None:
        raise SystemExit("generate-mesa-venus-port.py: the port has no top-level Android.bp")
    if not any(block_name(block) == COMPAT for block in top.blocks):
        top.blocks.append(compat_block(wide_offset_arches))
    print(f"scoped {len(names)} module names in {changed} Android.bp files")


if __name__ == "__main__":
    main()
