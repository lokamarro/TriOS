#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check how the Mesa port generator handles more than one host platform.

The converter drops a generated source whose command differs between the host
platforms it merges. The generator converts each platform alone, keeps the
first platform's generator under its name, adds the others as `<name>_<arch>`,
and has each library that uses one select by arch. This test drives those
steps on synthetic Android.bp files, without the converter, and checks that
the tracked platform description covers the platforms the port names.

Soong accepts a select only on a property whose Go type is
proptools.Configurable. The converter writes one on any property that differs
between platforms, so the generator moves the values of a property Soong does
not make configurable into an arch block. The test pins the properties Soong's
own structs declare, checks the generator's tables against them, drives the
move on synthetic modules, and refuses the committed port if it still selects
a property outside the pinned list.

Meson compiles every file with -D_FILE_OFFSET_BITS=64 and the converter does
not carry that over, so the generator adds it for each arch whose platform has
four-byte pointers. The test drives that on synthetic platforms and modules,
and refuses a committed port with an arm module that would be built without
it, since the 32-bit Venus renderer then mmaps a DRM offset above 4 GiB with a
truncated off_t."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import sys
import re
import tempfile
import tomllib
from pathlib import Path
from types import SimpleNamespace


def fail(message: str) -> None:
    raise SystemExit(f"Mesa port generator fixture: {message}")


def load(source_root: Path):
    tools = source_root / "android/lineage-24/tools"
    sys.path.insert(0, str(tools))
    path = tools / "generate-mesa-venus-port.py"
    spec = importlib.util.spec_from_file_location("generate_mesa_venus_port", path)
    if spec is None or spec.loader is None:
        fail("cannot load the generator")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def genrule(name: str, size: str, tools: str = "gen_tool") -> str:
    return f'''genrule {{
    name: "{name}",
    out: [
        "{name}.out",
    ],
    tools: [
        "{tools}",
    ],
    cmd: "$(location {tools}) in.xml {size} > $(location {name}.out)",
}}
'''


def consumer(kind: str, extra: str = "") -> str:
    return f'''{kind} {{
    name: "consumer",
    generated_headers: [
        "marshal.h",
        "other.h",
    ],
    srcs: [
        ":table.c",
        "a.c",
    ],
{extra}}}
'''


def write(root: Path, relative: str, text: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def blocks(generator, root: Path, relative: str):
    bp = generator.Blueprint(root / relative)
    return {
        (relative, generator.block_name(block)): block
        for block in bp.blocks
        if generator.block_name(block) is not None
    }


def variants_for(generator, scratch: Path, sizes: dict[str, str]) -> dict:
    out = {}
    for platform, size in sizes.items():
        root = scratch / platform
        write(
            root, "gen/Android.bp",
            genrule("marshal.h", size) + "\n" + genrule("table.c", size)
            + "\n" + genrule("same.c", "common"),
        )
        out[platform] = blocks(generator, root, "gen/Android.bp")
    return out


# The property structs of build/soong at 5ea23647 (the LineageOS lineage-24.0
# workspace the Android build uses), per module-type group: the properties
# whose type is proptools.Configurable, the only ones blueprint lets a select
# set (build/blueprint/proptools/unpack.go, selectOnNonConfigurablePropertyError),
# and the other properties tagged arch_variant, the only ones an arch block may
# set. The cc module types read these four structs; genrule reads the last two.
SOONG_STRUCTS = {
    ("cc", "cc/compiler.go", "BaseCompilerProperties"): (
        ["cflags", "cppflags", "device_first_generated_headers", "exclude_srcs",
         "generated_headers", "include_dirs", "local_include_dirs", "srcs"],
        ["asflags", "clang_asflags", "clang_cflags", "conlyflags",
         "exclude_generated_sources", "generated_sources", "instruction_set",
         "rtti", "tidy_disabled_srcs", "tidy_timeout_srcs"],
    ),
    ("cc", "cc/linker.go", "BaseLinkerProperties"): (
        ["export_header_lib_headers", "header_libs", "ldflags", "shared_libs",
         "static_libs", "whole_static_libs"],
        ["allow_undefined_symbols", "exclude_runtime_libs",
         "export_generated_headers", "export_shared_lib_headers",
         "export_static_lib_headers", "group_static_libs", "host_ldlibs",
         "ignore_max_page_size", "no_crt_pad_segment", "no_libcrt", "nocrt",
         "runtime_libs", "system_shared_libs"],
    ),
    ("cc", "cc/library.go", "FlagExporterProperties"): (
        ["export_cflags", "export_include_dirs"],
        ["export_system_include_dirs"],
    ),
    ("cc", "cc/cc.go", "BaseProperties"): (
        ["min_sdk_version"],
        ["clang", "installable", "optimize_for_size"],
    ),
    ("genrule", "genrule/genrule.go", "generatorProperties"): (
        ["cmd", "common_os_srcs", "device_common_srcs", "device_first_srcs",
         "host_first_srcs", "host_second_srcs", "srcs"],
        ["exclude_srcs"],
    ),
    ("genrule", "genrule/genrule.go", "genRuleProperties"): ([], ["out"]),
}
CONFIGURABLE = {
    group: frozenset(n for (g, _, _), (conf, _) in SOONG_STRUCTS.items() if g == group for n in conf)
    for group in ("cc", "genrule")
}
ARCH_VARIANT = {
    group: frozenset(n for (g, _, _), (_, av) in SOONG_STRUCTS.items() if g == group for n in av)
    for group in ("cc", "genrule")
}
GROUPS = {
    "cc_binary": "cc", "cc_defaults": "cc", "cc_library_headers": "cc",
    "cc_library_shared": "cc", "cc_library_static": "cc", "cc_object": "cc",
    "genrule": "genrule",
}

PORT = SimpleNamespace(
    host_platforms=["p64", "p32"], platform_arch={"p64": "arm64", "p32": "arm"}
)


def check_split(generator, scratch: Path) -> None:
    variants = variants_for(generator, scratch / "variants", {"p64": "8", "p32": "4"})
    merged = scratch / "merged"
    # The converter keeps one platform's copy of a generator it merges wrongly
    # and drops another one, so the merged output has marshal.h with the last
    # platform's command and no table.c.
    write(merged, "gen/Android.bp", genrule("marshal.h", "4") + "\n" + genrule("same.c", "common"))
    write(merged, "lib/Android.bp", consumer("cc_library_static") + "\n" + '''cc_library_static {
    name: "bystander",
    srcs: [
        "b.c",
    ],
}
''')
    blueprints = generator.load(merged)
    count = generator.split_arch_genrules(merged, blueprints, variants, PORT)
    if count != 2:
        fail(f"two generators differ between the platforms, not {count}")
    for bp in blueprints:
        bp.save()
    gen = (merged / "gen/Android.bp").read_text()
    for wanted in (
        'name: "marshal.h",', 'name: "marshal.h_arm",', 'name: "table.c",',
        'name: "table.c_arm",', "in.xml 8 >", "in.xml 4 >",
    ):
        if wanted not in gen:
            fail(f"the generators lack {wanted}:\n{gen}")
    if gen.count('name: "same.c"') != 1 or "same.c_arm" in gen:
        fail("a generator that is the same on both platforms must stay as it is")
    names = [generator.block_name(b) for b in generator.Blueprint(merged / "gen/Android.bp").blocks]
    if sorted(names) != sorted(
        ["marshal.h", "marshal.h_arm", "table.c", "table.c_arm", "same.c"]
    ):
        fail(f"each generator is a module of its own: {names}")
    lib = (merged / "lib/Android.bp").read_text()
    for wanted in (
        '''    generated_headers: [
        "other.h",
    ],''',
        '''    srcs: [
        "a.c",
    ],''',
        '''    arch: {
        arm: {
            generated_headers: [
                "marshal.h_arm",
            ],
            srcs: [
                ":table.c_arm",
            ],
        },
        arm64: {
            generated_headers: [
                "marshal.h",
            ],
            srcs: [
                ":table.c",
            ],
        },
    },''',
    ):
        if wanted not in lib:
            fail(f"the consumer lacks:\n{wanted}\nin\n{lib}")
    if lib.count("arch: {") != 1:
        fail("only the library that uses a split generator gains an arch block")


def check_unchanged(generator, scratch: Path) -> None:
    variants = variants_for(generator, scratch / "same-variants", {"p64": "8", "p32": "8"})
    merged = scratch / "same-merged"
    write(merged, "gen/Android.bp", genrule("marshal.h", "8") + "\n" + genrule("table.c", "8")
          + "\n" + genrule("same.c", "common"))
    write(merged, "lib/Android.bp", consumer("cc_library_static"))
    before = {p: p.read_text() for p in merged.rglob("Android.bp")}
    blueprints = generator.load(merged)
    if generator.split_arch_genrules(merged, blueprints, variants, PORT) != 0:
        fail("platforms that agree split nothing")
    for bp in blueprints:
        if bp.save():
            fail("platforms that agree must leave every file as it was")
    if {p: p.read_text() for p in merged.rglob("Android.bp")} != before:
        fail("platforms that agree must leave every file as it was")


def check_refusals(generator, scratch: Path) -> None:
    variants = variants_for(generator, scratch / "refuse-variants", {"p64": "8", "p32": "4"})

    def split(consumer_text: str, name: str):
        merged = scratch / name
        write(merged, "gen/Android.bp", genrule("marshal.h", "4"))
        write(merged, "lib/Android.bp", consumer_text)
        blueprints = generator.load(merged)
        try:
            generator.split_arch_genrules(merged, blueprints, variants, PORT)
        except SystemExit as error:
            return str(error)
        fail(f"{name} must be refused")
        return ""

    # A reference inside a field that already carries a select cannot be moved.
    message = split('''cc_library_static {
    name: "consumer",
    srcs: [
        "a.c",
    ] + select(arch(), {
        "arm": [
            ":table.c",
        ],
        default: [],
    }),
}
''', "conditional")
    if "conditional srcs" not in message:
        fail(f"a reference in a conditional list is refused: {message}")
    message = split(consumer("cc_defaults"), "defaults")
    if "arch block cannot carry" not in message:
        fail(f"a module that cannot take an arch block is refused: {message}")


def check_soong_tables(generator) -> None:
    for group in CONFIGURABLE:
        if CONFIGURABLE[group] & ARCH_VARIANT[group]:
            fail(f"{group}: a property is both configurable and plain in the pinned list")
    if dict(generator.SOONG_CONFIGURABLE) != CONFIGURABLE:
        fail("the generator's SOONG_CONFIGURABLE differs from the pinned Soong list")
    if dict(generator.SOONG_ARCH_VARIANT) != ARCH_VARIANT:
        fail("the generator's SOONG_ARCH_VARIANT differs from the pinned Soong list")
    if dict(generator.SELECT_GROUP) != GROUPS:
        fail("the generator's SELECT_GROUP differs from the pinned module types")
    # The property that failed the first Soong run: a plain list, not configurable.
    if "conlyflags" in CONFIGURABLE["cc"] or "conlyflags" not in ARCH_VARIANT["cc"]:
        fail("cc conlyflags is a plain arch_variant list in Soong")
    if "cppflags" not in CONFIGURABLE["cc"] or "srcs" not in CONFIGURABLE["cc"]:
        fail("cc cppflags and srcs are configurable in Soong")


LIFT_INPUT = '''cc_defaults {
    name: "defs",
    conlyflags: [
        "-DA",
    ] + select(arch(), {
        "arm64": [
            "-DB",
        ],
        default: [],
    }),
    cppflags: [
        "-DC",
    ] + select(arch(), {
        "arm64": [
            "-DD",
        ],
        default: [],
    }),
}

cc_library_static {
    name: "lib",
    srcs: [
        "a.c",
    ] + select(arch(), {
        "arm64": [
            "b.c",
        ],
        "arm": [
            "c.c",
        ],
        default: [],
    }),
    asflags: [
    ] + select(arch(), {
        "arm": [
            "-x",
        ],
        default: ["-y"],
    }),
    conlyflags: [
        "-DE",
    ] + select(arch(), {
        "arm": [
            "-DF",
        ],
        default: [],
    }),
    arch: {
        arm: {
            srcs: [
                ":gen_arm",
            ],
        },
        arm64: {
            srcs: [
                ":gen",
            ],
        },
    },
}
'''


def check_lift(generator, scratch: Path) -> None:
    root = scratch / "lift"
    write(root, "Android.bp", LIFT_INPUT)
    blueprints = generator.load(root)
    if generator.lift_arch_selects(blueprints, PORT) != 3:
        fail("three selects are on properties Soong does not make configurable")
    for bp in blueprints:
        bp.save()
    text = (root / "Android.bp").read_text()
    defaults, library = text.split("\ncc_library_static {\n")
    for wanted in (
        '''    conlyflags: [
        "-DA",
    ],
    cppflags: [
        "-DC",
    ] + select(arch(), {
        "arm64": [
            "-DD",
        ],
        default: [],
    }),
    arch: {
        arm64: {
            conlyflags: [
                "-DB",
            ],
        },
    },
}''',
    ):
        if wanted not in defaults:
            fail(f"the defaults lack:\n{wanted}\nin\n{defaults}")
    for wanted in (
        '''    srcs: [
        "a.c",
    ] + select(arch(), {''',
        # The empty list before a lifted select goes with it.
        '''    conlyflags: [
        "-DE",
    ],
    arch: {
        arm: {
            asflags: [
                "-x",
            ],
            conlyflags: [
                "-DF",
            ],
            srcs: [
                ":gen_arm",
            ],
        },
        arm64: {
            asflags: [
                "-y",
            ],
            srcs: [
                ":gen",
            ],
        },
    },
}''',
    ):
        if wanted not in library:
            fail(f"the library lacks:\n{wanted}\nin\n{library}")
    if "asflags: [\n    ]" in library:
        fail("a lifted select leaves no empty list behind")
    if library.count("arch: {") != 1:
        fail("lifted values join the arch block the module already has")
    # What remains selects only configurable properties.
    if generator.audit_selects(generator.load(root)) != 2:
        fail("the cppflags and srcs selects stay")
    again = generator.load(root)
    if generator.lift_arch_selects(again, PORT) != 0 or any(bp.save() for bp in again):
        fail("lifting is idempotent")


def check_lift_refusals(generator, scratch: Path) -> None:
    def lift(name: str, text: str, audit: bool = False):
        root = scratch / name
        write(root, "Android.bp", text)
        blueprints = generator.load(root)
        try:
            if audit:
                generator.audit_selects(blueprints)
            else:
                generator.lift_arch_selects(blueprints, PORT)
        except SystemExit as error:
            return str(error)
        fail(f"{name} must be refused")
        return ""

    def module(kind: str, field: str, extra: str = "") -> str:
        return f'''{kind} {{
    name: "m",
    {field}: [
        "x",
    ] + select(arch(), {{
        "arm64": [
            "y",
        ],
        default: [],
    }}),
{extra}}}
'''

    message = lift("plain", module("genrule", "tools"))
    if "neither makes configurable nor lets an arch block set" not in message:
        fail(f"a select on a property outside both lists is refused: {message}")
    message = lift("kind", module("filegroup", "srcs"))
    if "neither makes configurable" not in message:
        fail(f"a module type the tables do not name is refused: {message}")
    message = lift("nested", '''cc_library_static {
    name: "m",
    target: {
        android: {
            cflags: [
                "x",
            ] + select(arch(), {
                default: [],
            }),
        },
    },
}
''')
    if "cannot read" not in message:
        fail(f"a select below the top level is refused: {message}")
    message = lift("bare", '''cc_library_static {
    name: "m",
    srcs: select(arch(), {
        default: [],
    }),
}
''')
    if "cannot read" not in message:
        fail(f"a select with no list before it is refused: {message}")
    message = lift("other-arch", module("cc_defaults", "conlyflags").replace("arm64", "x86"))
    if "no host platform's arch" not in message:
        fail(f"a select on an arch no host platform has is refused: {message}")
    message = lift("audit", module("cc_defaults", "conlyflags"), audit=True)
    if "still selects conlyflags" not in message:
        fail(f"a select left on conlyflags fails the audit: {message}")
    message = lift("arch-shape", module(
        "cc_defaults", "conlyflags", "    arch: {\n        arm: { cflags: [\"x\"] },\n    },\n"))
    if "arch block" not in message:
        fail(f"an arch block the tool did not write is refused: {message}")


LARGE_FILE_FLAG = "-D_FILE_OFFSET_BITS=64"
FLAG_FIELDS = ("cflags", "conlyflags", "cppflags")


def platforms_toml(**pointers: dict[str, int] | None) -> str:
    """A platform description whose platforms give void* the sizes named, or
    nothing for the converter's eight-byte default."""
    text = ""
    for name, sizes in pointers.items():
        text += f'[[platform]]\nname = "{name}"\n'
        for language in ("c", "cpp"):
            text += f"[platform.{language}]\ncompiler_id = \"clang\"\n"
            if sizes:
                text += f"[platform.{language}.sizeof.sizes]\n"
                text += "".join(f'"{spelling}" = {size}\n' for spelling, size in sizes.items())
    return text


def check_large_file_arches(generator, scratch: Path) -> None:
    both = SimpleNamespace(host_platforms=["p64", "p32"],
                           platform_arch={"p64": "arm64", "p32": "arm"})
    four = {"void*": 4, "void *": 4}

    def arches(port, **pointers) -> list[str]:
        path = scratch / "large-file.toml"
        path.write_text(platforms_toml(**pointers))
        return generator.large_file_arches(path, port)

    if arches(both, p64=None, p32=four) != ["arm"]:
        fail("a platform with four-byte pointers needs the large-file define for its arch")
    if arches(SimpleNamespace(host_platforms=["p64"], platform_arch={}), p64=None) != []:
        fail("a 64-bit port needs no large-file define, and its output must not change")
    for label, port, pointers in (
        ("the two spellings of void* disagreeing", both,
         {"p64": None, "p32": {"void*": 4, "void *": 8}}),
        ("a pointer that is neither four nor eight bytes", both,
         {"p64": None, "p32": {"void*": 2, "void *": 2}}),
        ("a 32-bit platform without an arch", SimpleNamespace(
            host_platforms=["p32"], platform_arch={}), {"p32": four}),
        ("a host platform the description lacks", both, {"p64": None}),
    ):
        try:
            arches(port, **pointers)
        except SystemExit:
            continue
        fail(f"{label} must be refused")


def check_large_file_scope(generator, scratch: Path) -> None:
    """scope() gives every compiled module the defaults, and the defaults carry
    the define for the 32-bit arches alone."""
    sixty_four = """
cc_defaults {
    name: "venus_kernarium_compat_args",
    vendor: true,
    cflags: [
        "-Wno-gnu-pointer-arith",
    ],
    cppflags: [
        "-Wno-gnu-pointer-arith",
    ],
}
"""
    for label, arches in (("64", []), ("32", ["arm"])):
        root = scratch / f"scope-{label}"
        write(root, "Android.bp",
              'cc_library_shared {\n    name: "lib",\n    srcs: [\n        "a.c",\n    ],\n}\n'
              'cc_library_static {\n    name: "helper",\n    srcs: [\n        "b.c",\n    ],\n}\n')
        blueprints = generator.load(root)
        generator.scope(root, blueprints, {"lib"}, arches)
        text = blueprints[0].text()
        defaults = text[text.index("\ncc_defaults {"):]
        if arches:
            wanted = sixty_four[: sixty_four.rindex("}")] + (
                "    arch: {\n        arm: {\n            cflags: [\n"
                f'                "{LARGE_FILE_FLAG}",\n'
                "            ],\n        },\n    },\n}\n")
        else:
            wanted = sixty_four
        if defaults.strip() != wanted.strip():
            fail(f"the {label}-bit defaults differ from the expected block:\n{defaults}")
        for module in ("lib", "venus_helper"):
            block = next(b for b in blueprints[0].blocks if generator.block_name(b) == module)
            if generator.list_items(block, "defaults") != ["venus_kernarium_compat_args"]:
                fail(f"{module} must use the compat defaults")


def effective_flags(generator, blocks_by_name: dict, block: list[str], arch: str) -> list[str]:
    """The compile flags Soong gives the module for one arch: its own and its
    defaults', the arch block's, and the select branch the arch takes."""
    flags: list[str] = []
    selects = generator.selects(block)
    plain, arches = generator.take_arch(block)
    for field in FLAG_FIELDS:
        flags += generator.list_items(plain, field)
        flags += arches.get(arch, {}).get(field, [])
        for choice in selects:
            if choice.field == field:
                flags += choice.branches.get(arch, choice.branches.get("default", []))
    for name in generator.list_items(plain, "defaults"):
        flags += effective_flags(generator, blocks_by_name, blocks_by_name[name], arch)
    return flags


def check_committed_large_file(generator, root: Path) -> None:
    """Every module the committed port compiles carries Meson's large-file
    define for arm, and the 64-bit modules are as the port had them."""
    blocks_by_name = {
        generator.block_name(b): b for bp in generator.load(root) for b in bp.blocks
        if generator.block_name(b) is not None
    }
    compiled = [
        name for name, block in sorted(blocks_by_name.items())
        if generator.block_kind(block) in ("cc_library_shared", "cc_library_static", "cc_binary")
    ]
    if len(compiled) < 30:
        fail(f"the committed port compiles only {len(compiled)} modules; the check would prove little")
    for name in compiled:
        arm = effective_flags(generator, blocks_by_name, blocks_by_name[name], "arm")
        if LARGE_FILE_FLAG not in arm:
            fail(f"{name} is built for arm without {LARGE_FILE_FLAG}: its mmap and lseek would "
                 "take a 32-bit off_t and fail on a DRM offset above 4 GiB")
        arm64 = effective_flags(generator, blocks_by_name, blocks_by_name[name], "arm64")
        if LARGE_FILE_FLAG in arm64:
            fail(f"{name} gains {LARGE_FILE_FLAG} for arm64, which changes the 64-bit port")


def committed_port_files(source_root: Path, scratch: Path) -> Path:
    """The Android.bp files the committed port series adds, written under scratch."""
    series = source_root / "android/lineage-24/third_party/mesa-venus/series"
    patch = (series / "Mesa-Venus-generated-Soong-port.patch").read_text()
    root = scratch / "committed"
    count = 0
    for section in patch.split("\ndiff --git ")[1:]:
        lines = section.splitlines()
        target = next((l for l in lines if l.startswith("+++ ")), "")
        if not target.endswith("Android.bp"):
            continue
        if not any(l == "--- /dev/null" for l in lines):
            fail(f"{target} is not a new file, so the patch cannot restate it whole")
        body = [l[1:] for l in lines[lines.index("--- /dev/null") + 3:] if l.startswith("+")]
        write(root, target.removeprefix("+++ b/"), "\n".join(body) + "\n")
        count += 1
    if count < 50:
        fail(f"the committed port adds {count} Android.bp files")
    return root


def check_committed_port(generator, source_root: Path, scratch: Path) -> None:
    root = committed_port_files(source_root, scratch)
    kept: list[tuple[str, str, str]] = []
    for bp in generator.load(root):
        for block in bp.blocks:
            kind = generator.block_kind(block)
            for choice in generator.selects(block):
                group = GROUPS.get(kind)
                where = f"{bp.path.relative_to(root)} {generator.block_name(block)}"
                if group is None or choice.field not in CONFIGURABLE[group]:
                    fail(f"{where} selects {choice.field}, which Soong does not make configurable")
                kept.append((kind, choice.field, where))
    if not kept:
        fail("the committed port selects nothing; the fixture would pass without checking")
    # Any select is a first of its line, and no other property carries one.
    text = "\n".join(bp.text() for bp in generator.load(root))
    if len(re.findall(r"select\(", text)) != len(kept):
        fail("a select in the committed port was not parsed")
    # The one flag the arm64 platform alone sets reaches the arch block.
    defaults = [
        b for bp in generator.load(root) for b in bp.blocks
        if generator.block_name(b) == "venus_aosp_mesa_venus_c_project_args"
    ]
    if len(defaults) != 1 or "            conlyflags: [\n                \"-DHAVE_UINT128\",\n" not in "\n".join(defaults[0]):
        fail("-DHAVE_UINT128 belongs in an arm64 arch block of the C defaults")
    check_committed_large_file(generator, root)


def check_host_platforms(generator) -> None:
    ok = {
        "config": [{"platforms": {"host_platforms": ["p64", "p32"]}}],
        "kernarium": {"platform_arch": {"p64": "arm64", "p32": "arm"}},
    }
    platforms, arches = generator.host_platforms(ok)
    if platforms != ["p64", "p32"] or arches != {"p64": "arm64", "p32": "arm"}:
        fail(f"host platforms: {platforms} {arches}")
    single = {"config": [{"platforms": {"host_platforms": ["p64"]}}]}
    if generator.host_platforms(single)[0] != ["p64"]:
        fail("one platform needs no arch map")
    for label, table in (
        ("a missing arch map", {"config": ok["config"]}),
        ("a shared arch", {**ok, "kernarium": {"platform_arch": {"p64": "arm", "p32": "arm"}}}),
        ("an arch for another platform", {**ok, "kernarium": {"platform_arch": {"p64": "arm64"}}}),
        ("no host platform", {"config": [{"platforms": {"host_platforms": []}}]}),
        ("two configs", {"config": ok["config"] * 2}),
    ):
        try:
            generator.host_platforms(table)
        except SystemExit:
            continue
        fail(f"{label} must be refused")


def check_tracked_platforms(source_root: Path) -> None:
    mesa = source_root / "android/lineage-24/third_party/mesa-venus"
    port = tomllib.loads((mesa / "soong-port.toml").read_text())
    platforms = tomllib.loads((mesa / "soong-platforms.toml").read_text())
    defined = {platform["name"]: platform for platform in platforms["platform"]}
    config = port["config"][0]["platforms"]
    for name in config["host_platforms"] + config["build_platforms"]:
        if name not in defined:
            fail(f"soong-platforms.toml does not define {name}")
    if set(port["kernarium"]["platform_arch"]) != set(config["host_platforms"]):
        fail("every host platform needs its Soong arch in [kernarium.platform_arch]")
    for name in config["host_platforms"]:
        for language in ("c", "cpp"):
            if "toolchain" in defined[name][language]:
                fail(f"{name} must not name a toolchain: the converter would download it")
    arm = defined["android_uml_arm"]
    for language in ("c", "cpp"):
        if arm[language]["compiles"]["fails"].get("__uint128_t") is not True:
            fail("a 32-bit ARM target has no __uint128_t")
        if arm[language]["sizeof"]["sizes"]["void*"] != 4:
            fail("a 32-bit ARM target has four-byte pointers")
        if "sizeof" in defined["android_uml_arm64"][language]:
            fail("the 64-bit platform takes the converter's eight-byte defaults")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    generator = load(args.source_root)
    check_host_platforms(generator)
    check_tracked_platforms(args.source_root)
    check_soong_tables(generator)
    with tempfile.TemporaryDirectory(prefix="mesa-port-generator.") as name:
        scratch = Path(name)
        check_split(generator, scratch)
        check_unchanged(generator, scratch)
        check_refusals(generator, scratch)
        check_lift(generator, scratch)
        check_lift_refusals(generator, scratch)
        check_large_file_arches(generator, scratch)
        check_large_file_scope(generator, scratch)
        check_committed_port(generator, args.source_root, scratch)
    print("Mesa port generator fixture passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
