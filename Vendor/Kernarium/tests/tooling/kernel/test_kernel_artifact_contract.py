#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the supported kernel artifact family, slice, and page matrix."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from types import ModuleType


SUPPORTED_IDENTITIES = {
    ("mainline-6.18", "arm64", 4096),
    ("mainline-6.18", "x86_64", 4096),
    ("android-6.18", "arm64", 4096),
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def load_artifact_module(path: Path) -> ModuleType:
    # The module imports its sibling contract_checks the way its callers do.
    sys.path.insert(0, str(path.parent))
    spec = importlib.util.spec_from_file_location("kernel_artifact_contract", path)
    require(spec is not None and spec.loader is not None, "cannot load artifact module")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def manifest(
    module: ModuleType,
    family: str,
    kernel_slice: str,
    page_size: int,
) -> dict:
    release_id = "contract-test"
    return {
        "schema_version": module.SCHEMA_VERSION,
        "release_id": release_id,
        "directory_name": (
            f"{family}-{kernel_slice}-{page_size // 1024}k-{release_id}"
        ),
        "family": family,
        "slice": kernel_slice,
        "published_at_utc": "2026-01-01T00:00:00Z",
        "release": "6.18.0-contract-test",
        "page_size": page_size,
        "elf_role": "darwin-uml",
        "source": {
            "repository": "https://example.invalid/linux.git",
            "branch": "contract-test",
            "base_commit": "0" * 40,
            "base_tree": "1" * 40,
            "final_tree": "2" * 40,
        },
        "abi": {
            "version": 1,
            "fingerprint": "0x0000000000000001",
        },
        "inventory": [
            {
                "path": path,
                "role": role,
                "format": file_format,
                "size": 1,
            }
            for path, (role, file_format) in sorted(module.PAYLOAD_SPECS.items())
        ],
    }


def require_accepted(
    module: ModuleType,
    family: str,
    kernel_slice: str,
    page_size: int,
) -> None:
    module.validate_manifest(manifest(module, family, kernel_slice, page_size))


def require_rejected(
    module: ModuleType,
    family: str,
    kernel_slice: str,
    page_size: int,
    diagnostic: str,
) -> None:
    try:
        module.validate_manifest(manifest(module, family, kernel_slice, page_size))
    except SystemExit as error:
        require(
            diagnostic in str(error),
            f"invalid identity was rejected for the wrong reason: {error}",
        )
    else:
        raise AssertionError(
            "invalid kernel artifact identity was accepted: "
            f"{family}, {kernel_slice}, {page_size}"
        )


def schema_identities(path: Path) -> set[tuple[str, str, int]]:
    schema = json.loads(path.read_text(encoding="utf-8"))
    # Every identity has the one page size the schema names.
    page_size = schema["properties"]["page_size"]["const"]
    identities: set[tuple[str, str, int]] = set()
    for branch in schema["oneOf"]:
        properties = branch["properties"]
        identities.add(
            (properties["family"]["const"], properties["slice"]["const"], page_size)
        )
    return identities


def config_values(kernel_slice: str) -> dict[str, str]:
    return {
        "CONFIG_64BIT": "y",
        "CONFIG_ARM64": "y",
        "CONFIG_UML": "y",
        "CONFIG_MODULES": "n",
        "CONFIG_PAGE_SHIFT": "12",
        "CONFIG_PAGE_SIZE_4KB": "y",
        "CONFIG_UML_X86_64_USERSPACE_ONLY": (
            "y" if kernel_slice == "x86_64" else "n"
        ),
        "CONFIG_BPF": "y",
        "CONFIG_BPF_SYSCALL": "y",
        "CONFIG_CGROUP_BPF": "y",
        "CONFIG_DEBUG_INFO_BTF": "y",
    }


SHT_PROGBITS = 1
SHT_STRTAB = 3
SHF_ALLOC = 2


def write_elf(path: Path, sections: list[tuple[str, int, int]]) -> None:
    """Write an ELF64 file whose named sections each hold 16 bytes."""
    names = b"\0"
    name_offsets = []
    for name, _, _ in sections:
        name_offsets.append(len(names))
        names += name.encode("ascii") + b"\0"
    names_name = len(names)
    names += b".shstrtab\0"
    data_offset = 64
    payload = b""
    headers = [struct.pack("<IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    for (_, kind, flags), name in zip(sections, name_offsets):
        headers.append(
            struct.pack(
                "<IIQQQQIIQQ",
                name, kind, flags, 0, data_offset + len(payload), 16, 0, 0, 1, 0,
            )
        )
        payload += b"\xa5" * 16
    headers.append(
        struct.pack(
            "<IIQQQQIIQQ",
            names_name, SHT_STRTAB, 0, 0, data_offset + len(payload),
            len(names), 0, 0, 1, 0,
        )
    )
    payload += names
    section_offset = data_offset + len(payload)
    header = bytearray(64)
    header[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<Q", header, 40, section_offset)
    struct.pack_into("<HHH", header, 58, 64, len(headers), len(headers) - 1)
    path.write_bytes(bytes(header) + payload + b"".join(headers))


def require_runtime_image(
    module: ModuleType,
    root: Path,
    label: str,
    sections: list[tuple[str, int, int]],
    diagnostic: str | None,
) -> None:
    image = root / f"{label}.elf"
    write_elf(image, sections)
    try:
        module.verify_runtime_image(image.read_bytes())
    except SystemExit as error:
        require(
            diagnostic is not None and diagnostic in str(error),
            f"runtime image {label} was rejected for the wrong reason: {error}",
        )
    else:
        require(diagnostic is None, f"runtime image {label} was accepted")


PT_LOAD = 1
PT_GNU_STACK = 0x6474E551
PF_X = 1
PF_W = 2
PF_R = 4
SHT_SYMTAB = 2
# The fixture's program headers: an R+X load, an R+W load and the GNU stack.
PROGRAM_HEADERS = (64, 120, 176)


def kernel_elf(imports: list[str]) -> bytearray:
    """Return an AArch64 ET_DYN kernel image that imports these names.

    Its loads align to the 16 KiB Darwin host page.

    Its .symtab also defines darwin_uml_defined_here, which is not an import.
    """
    names = bytearray(b"\0")
    symbols = bytearray(24)
    for name, section in [("darwin_uml_defined_here", 1)] + [
        (name, 0) for name in imports
    ]:
        symbols += struct.pack("<IBBHQQ", len(names), 0x10, 0, section, 0, 0)
        names += name.encode("ascii") + b"\0"
    section_names = b"\0.symtab\0.strtab\0.shstrtab\0"
    symtab = 64 + 56 * len(PROGRAM_HEADERS)
    strtab = symtab + len(symbols)
    shstrtab = strtab + len(names)
    section_table = shstrtab + len(section_names)
    image = bytearray(section_table + 64 * 4)
    image[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into(
        "<HHIQQQIHHHHHH", image, 16,
        3, 183, 1, 0, 64, section_table, 0, 64, 56, len(PROGRAM_HEADERS), 64, 4, 3,
    )
    for offset, (kind, flags, address, size, alignment) in zip(
        PROGRAM_HEADERS,
        (
            (PT_LOAD, PF_R | PF_X, 0, symtab, 0x4000),
            (PT_LOAD, PF_R | PF_W, 0x10000, symtab, 0x4000),
            (PT_GNU_STACK, PF_R | PF_W, 0, 0, 16),
        ),
    ):
        struct.pack_into(
            "<IIQQQQQQ", image, offset,
            kind, flags, 0, address, address, size, size, alignment,
        )
    image[symtab:section_table] = symbols + names + section_names
    for index, (name, kind, offset, size, link, entry_size) in enumerate(
        (
            (0, 0, 0, 0, 0, 0),
            (1, SHT_SYMTAB, symtab, len(symbols), 2, 24),
            (9, SHT_STRTAB, strtab, len(names), 0, 0),
            (17, SHT_STRTAB, shstrtab, len(section_names), 0, 0),
        )
    ):
        struct.pack_into(
            "<IIQQQQIIQQ", image, section_table + 64 * index,
            name, kind, 0, 0, offset, size, link, 0, 1, entry_size,
        )
    return image


def section_header(image: bytearray, index: int) -> int:
    return struct.unpack_from("<Q", image, 40)[0] + 64 * index


def write_bridge_policy(abi: Path, rows: list[str]) -> None:
    (abi / "import-policy.tsv").write_text(
        "\n".join(["symbol\tavailability\trationale", *rows]) + "\n",
        encoding="utf-8",
    )


def require_kernel_elf(
    module: ModuleType,
    path: Path,
    image: bytes,
    common_root: Path,
    kernel_slice: str,
    diagnostic: str | None,
) -> None:
    path.write_bytes(image)
    try:
        module.verify_elf(path, common_root, kernel_slice)
    except SystemExit as error:
        require(
            diagnostic is not None and diagnostic in str(error),
            f"kernel ELF was rejected for the wrong reason: {error}",
        )
    else:
        require(diagnostic is None, f"kernel ELF was accepted, expected: {diagnostic}")


def verify_kernel_elf_contract(module: ModuleType, root: Path) -> None:
    """Pin the kernel ELF's geometry checks and its bridge import policy."""
    abi = root / "common/abi"
    abi.mkdir(parents=True)
    (abi / "darwin_uml_fixture.h").write_text(
        "int darwin_uml_everywhere(void);\n"
        "void darwin_uml_arm64_only(void);\n",
        encoding="utf-8",
    )
    policy = ["darwin_uml_arm64_only\tarm64-only\tfixture"]
    write_bridge_policy(abi, policy)
    common = root / "common"
    path = root / "vmlinux"
    everywhere, arm64 = "darwin_uml_everywhere", "darwin_uml_arm64_only"
    active = [everywhere, arm64]

    def check(
        image: bytes,
        diagnostic: str | None,
        kernel_slice: str = "arm64",
    ) -> None:
        require_kernel_elf(module, path, image, common, kernel_slice, diagnostic)

    # Each slice imports exactly its active symbols.
    check(kernel_elf(active), None)
    check(kernel_elf([everywhere]), None, "x86_64")
    for imports, kernel_slice, missing, extra in (
        (active, "x86_64", [], [arm64]),
        ([everywhere], "arm64", [arm64], []),
        ([*active, "darwin_uml_undeclared"], "arm64", [], ["darwin_uml_undeclared"]),
    ):
        check(
            kernel_elf(imports),
            f"kernel bridge imports differ (missing={missing}, extra={extra})",
            kernel_slice,
        )
    check(kernel_elf(active), "unsupported kernel slice", "riscv64")

    # A load aligned to the 4 KiB guest page is enough; a smaller one is not.
    small_alignment = kernel_elf(active)
    for offset in PROGRAM_HEADERS[:2]:
        struct.pack_into("<Q", small_alignment, offset + 48, 0x1000)
    check(small_alignment, None)

    def mutated(layout: str, offset: int, value: int) -> bytearray:
        image = kernel_elf(active)
        struct.pack_into(layout, image, offset, value)
        return image

    load, data, stack = PROGRAM_HEADERS
    not_elf64 = "kernel is not a little-endian ELF64 image"
    not_et_dyn = "kernel is not an AArch64 ET_DYN image"
    unseparated = "kernel lacks separated executable and writable load segments"
    for layout, offset, value, diagnostic in (
        ("<B", 4, 1, not_elf64),
        ("<B", 5, 2, not_elf64),
        ("<B", 6, 0, not_elf64),
        ("<H", 16, 2, not_et_dyn),
        ("<H", 18, 62, not_et_dyn),
        ("<H", 54, 32, "ELF program-header entry size is malformed"),
        ("<H", 56, 1000, "ELF program-header table is outside the file"),
        ("<Q", load + 40, 1, "ELF program header 0 has file size above memory size"),
        ("<Q", data + 8, 0x40000, "ELF segment 1 is outside the file"),
        ("<I", stack + 4, PF_R | PF_W | PF_X, "kernel GNU stack is executable"),
        ("<I", data + 4, PF_R | PF_W | PF_X,
         "ELF load segment 1 is writable and executable"),
        ("<Q", load + 48, 0x800, "ELF load segment 0 has invalid alignment 0x800"),
        ("<Q", load + 48, 0x6000, "ELF load segment 0 has invalid alignment 0x6000"),
        ("<Q", data + 16, 0x10010, "ELF load segment 1 violates alignment congruence"),
        ("<I", data, 0, unseparated),
        ("<I", data + 4, PF_R, unseparated),
    ):
        check(mutated(layout, offset, value), diagnostic)

    symbols = section_header(kernel_elf(active), 1)
    strings = section_header(kernel_elf(active), 2)
    unterminated = kernel_elf(active)
    string_offset, string_size = struct.unpack_from("<QQ", unterminated, strings + 24)
    unterminated[string_offset + string_size - 1] = ord("x")
    for image, diagnostic in (
        (mutated("<I", symbols + 4, 1), "ELF contains no symbol table"),
        (mutated("<Q", symbols + 56, 16), "ELF symbol table 1 is malformed"),
        (mutated("<Q", symbols + 32, 25), "ELF symbol table 1 is malformed"),
        (mutated("<I", symbols + 40, 9),
         "ELF symbol table 1 has an invalid string table"),
        (unterminated, "ELF symbol name is unterminated"),
    ):
        check(image, diagnostic)

    # Host code must not import the process-wide sigprocmask or the variadic
    # fcntl, in any Mach-O spelling.
    for symbol, description in (
        ("sigprocmask", "the process-wide Darwin sigprocmask"),
        ("_sigprocmask$UNIX2003", "the process-wide Darwin sigprocmask"),
        ("fcntl", "the variadic Darwin fcntl"),
        ("_fcntl$NOCANCEL", "the variadic Darwin fcntl"),
    ):
        check(kernel_elf([*active, symbol]), f"the Linux image imports {description}")
    check(kernel_elf([*active, "pthread_sigmask"]), None)

    # Kernel builds run the module's command line on the image they built. The
    # copy in the fixture directory reads the fixture declarations and policy.
    command_line = abi / "bridge_imports.py"
    shutil.copyfile(
        Path(module.__file__).resolve().parents[1] / "abi/bridge_imports.py",
        command_line,
    )

    def run(*arguments: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-B", str(command_line), *arguments],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    for kernel_slice, symbols in (
        ("arm64", active),
        ("x86_64", [everywhere]),
    ):
        printed = run("--print-expected", "--slice", kernel_slice)
        require(
            printed.returncode == 0 and printed.stdout.split() == sorted(symbols),
            f"{kernel_slice} expected imports printed wrongly: {printed}",
        )
    for imports, diagnostic in (
        (active, None),
        ([everywhere], "kernel bridge imports differ"),
        ([*active, "_fcntl"], "the Linux image imports the variadic Darwin fcntl"),
    ):
        path.write_bytes(kernel_elf(imports))
        result = run("--kernel-binary", str(path), "--slice", "arm64")
        require(
            (result.returncode == 0) == (diagnostic is None)
            and (diagnostic is None or diagnostic in result.stderr),
            f"bridge import command line misjudged {imports}: {result.stderr}",
        )

    # The policy is a three-column table of declared conditional symbols. A
    # symbol exempt for 4 KiB pages is a retired availability: no kernel is.
    for row in (
        "darwin_uml_everywhere\tarm64-only",
        "darwin_uml_everywhere\t16k-only\tfixture",
        "darwin_uml_everywhere\t4k-only\tfixture",
        "darwin_uml_everywhere\tarm64-only\t",
        "darwin_uml_arm64_only\tarm64-only\tagain",
        "uml_everywhere\tarm64-only\tfixture",
    ):
        write_bridge_policy(abi, [*policy, row])
        check(kernel_elf(active), "ABI import policy is malformed at line 3")
    write_bridge_policy(abi, [*policy, "darwin_uml_undeclared\tarm64-only\tfixture"])
    check(kernel_elf(active), "ABI import policy names undeclared bridge symbols")
    (abi / "import-policy.tsv").write_text(
        "symbol\tavailability\n" + "\n".join(policy) + "\n", encoding="utf-8"
    )
    check(kernel_elf(active), "ABI import policy is malformed")
    (abi / "import-policy.tsv").write_text("", encoding="utf-8")
    check(kernel_elf(active), "ABI import policy is malformed")
    write_bridge_policy(abi, policy)
    (abi / "darwin_uml_fixture.h").unlink()
    check(kernel_elf(active), "shared ABI declarations are missing")


def write_config(path: Path, values: dict[str, str]) -> None:
    lines = [
        f"# {symbol} is not set" if value == "n" else f"{symbol}={value}"
        for symbol, value in sorted(values.items())
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def require_config_accepted(
    module: ModuleType,
    root: Path,
    kernel_slice: str,
    values: dict[str, str],
) -> None:
    path = root / f"{kernel_slice}-accepted.config"
    write_config(path, values)
    module.verify_config(path, kernel_slice)


def require_config_rejected(
    module: ModuleType,
    root: Path,
    kernel_slice: str,
    values: dict[str, str],
    symbol: str,
    wanted: str,
) -> None:
    observed = values[symbol]
    path = root / f"{kernel_slice}-{symbol}.config"
    write_config(path, values)
    try:
        module.verify_config(path, kernel_slice)
    except SystemExit as error:
        diagnostic = f"kernel config {symbol}={observed}, expected {wanted}"
        require(
            diagnostic in str(error),
            f"invalid kernel config was rejected for the wrong reason: {error}",
        )
    else:
        raise AssertionError(
            f"invalid {kernel_slice} kernel config accepted {symbol}={observed}"
        )


def verify_config_policy_contract(module: ModuleType, root: Path) -> None:
    """A product's lists need its required symbols at y and its forbidden ones off."""
    required = root / "required.txt"
    forbidden = root / "forbidden.txt"
    required.write_text(
        "# SPDX-License-Identifier: GPL-2.0-or-later\n"
        "# A comment and a blank line are not symbols.\n"
        "\n"
        "CONFIG_NEEDED\n"
        "  CONFIG_PADDED  \n",
        encoding="utf-8",
    )
    forbidden.write_text("# comment\nCONFIG_BANNED\nCONFIG_ABSENT\n", encoding="utf-8")

    def check(values: dict[str, str], diagnostic: str | None) -> None:
        config = root / "policy.config"
        write_config(config, values)
        try:
            module.verify_config_policy(config, required, forbidden)
        except SystemExit as error:
            require(
                diagnostic is not None and diagnostic in str(error),
                f"config policy rejected a kernel for the wrong reason: {error}",
            )
        else:
            require(diagnostic is None, f"config policy accepted: {diagnostic}")

    good = {"CONFIG_NEEDED": "y", "CONFIG_PADDED": "y", "CONFIG_BANNED": "n"}
    check(good, None)
    check({**good, "CONFIG_NEEDED": "n"}, "kernel config CONFIG_NEEDED=n, expected y")
    check({**good, "CONFIG_PADDED": "m"}, "kernel config CONFIG_PADDED=m, expected y")
    check(
        {key: value for key, value in good.items() if key != "CONFIG_NEEDED"},
        "kernel config CONFIG_NEEDED=n, expected y",
    )
    check({**good, "CONFIG_BANNED": "y"}, "kernel config CONFIG_BANNED=y, expected n")
    check({**good, "CONFIG_BANNED": "m"}, "kernel config CONFIG_BANNED=m, expected n")
    check({**good, "CONFIG_ABSENT": "y"}, "kernel config CONFIG_ABSENT=y, expected n")
    try:
        module.verify_config_policy(root / "policy.config", required, root / "missing.txt")
    except SystemExit as error:
        require("missing or unsafe" in str(error), f"a missing list failed oddly: {error}")
    else:
        raise AssertionError("config policy accepted a missing list")


def verify_bundle_page_size_contract(module: ModuleType, root: Path) -> None:
    """Verifying a bundle checks its manifest, and its page size, first."""
    for kernel_slice in ("arm64", "x86_64"):
        value = manifest(module, "mainline-6.18", kernel_slice, 16384)
        bundle = root / value["directory_name"]
        bundle.mkdir()
        (bundle / "kernel-artifact.json").write_text(
            json.dumps(value), encoding="utf-8"
        )
        try:
            module.verify_artifact(bundle, root, root / "kernel.env", root / "abi.env")
        except SystemExit as error:
            require(
                "kernel page size is unsupported" in str(error),
                f"a 16 KiB bundle was rejected for the wrong reason: {error}",
            )
        else:
            raise AssertionError(f"a 16 KiB {kernel_slice} bundle was verified")


DESCRIPTOR_PREFIX = "UML_KERNEL_ARTIFACT_"
# Each descriptor field, as the bundle's manifest above spells it, and a value
# that differs from it in the same format.
DESCRIPTOR_FIELDS = {
    "SCHEMA": ("3", "4"),
    "RELEASE_ID": ("contract-test", "other-release"),
    "DIRECTORY_NAME": ("mainline-6.18-arm64-4k-contract-test", "other-bundle"),
    "FAMILY": ("mainline-6.18", "android-6.18"),
    "SLICE": ("arm64", "x86_64"),
    "RELEASE": ("6.18.0-contract-test", "6.18.1-other"),
    "PAGE_SIZE": ("4096", "16384"),
    "FINAL_TREE": ("2" * 40, "3" * 40),
    "ABI_VERSION": ("1", "2"),
    "ABI_FINGERPRINT": ("0x0000000000000001", "0x0000000000000002"),
}


def write_descriptor(path: Path, fields: dict[str, str]) -> None:
    """Write an accepted-*.env descriptor: counts bare, everything else quoted."""
    rows = ["# SPDX-License-Identifier: GPL-2.0-or-later"]
    for key, value in fields.items():
        text = value if value.isdigit() else f"'{value}'"
        rows.append(f"{DESCRIPTOR_PREFIX}{key}={text}")
    path.write_text("\n".join(rows) + "\n", encoding="utf-8")


def verify_descriptor_contract(module: ModuleType, root: Path) -> None:
    """An accepted descriptor must repeat every identity field of its bundle."""
    value = manifest(module, "mainline-6.18", "arm64", 4096)
    abi = root / "descriptor-abi.env"
    abi.write_text(
        "DARWIN_UML_ABI_VERSION=1\n"
        "DARWIN_UML_ABI_FINGERPRINT='0x0000000000000001'\n",
        encoding="utf-8",
    )
    descriptor = root / "accepted.env"
    matching = {key: expected for key, (expected, _) in DESCRIPTOR_FIELDS.items()}
    require(
        matching["DIRECTORY_NAME"] == value["directory_name"],
        "the descriptor fixture does not describe the manifest fixture",
    )

    def check(fields: dict[str, str], diagnostic: str | None) -> None:
        write_descriptor(descriptor, fields)
        try:
            module.verify_compatible_identity(
                module.validate_manifest(value),
                abi,
                "mainline-6.18",
                "arm64",
                "6.18",
                descriptor,
            )
        except SystemExit as error:
            require(
                diagnostic is not None and diagnostic in str(error),
                f"a descriptor was rejected for the wrong reason: {error}",
            )
        else:
            require(diagnostic is None, f"a descriptor was accepted: {diagnostic}")

    check(matching, None)
    # A drifted field names itself and both values.
    for key, (expected, drifted) in DESCRIPTOR_FIELDS.items():
        check(
            {**matching, key: drifted},
            f"{DESCRIPTOR_PREFIX}{key}={drifted}, but the bundle manifest has {expected}",
        )
        check(
            {name: item for name, item in matching.items() if name != key},
            f"keys differ (missing=['{DESCRIPTOR_PREFIX}{key}'], extra=[])",
        )
    check(
        {**matching, "NOTE": "x"},
        f"keys differ (missing=[], extra=['{DESCRIPTOR_PREFIX}NOTE'])",
    )

    # The command line a configure runs: the bundle's directory name is its
    # basename, and only the manifest is read before the descriptor.
    bundle = root / value["directory_name"]
    bundle.mkdir()
    (bundle / "kernel-artifact.json").write_text(json.dumps(value), encoding="utf-8")
    verifier = Path(module.__file__).resolve().parent / "verify-kernel-artifact.py"

    def run(*arguments: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable, "-B", str(verifier),
                "--artifact", str(bundle), "--common-root", str(root),
                "--abi-metadata", str(abi), *arguments,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    compatible = (
        "--compatible", "--expected-family", "mainline-6.18",
        "--expected-slice", "arm64", "--expected-release-series", "6.18",
        "--descriptor", str(descriptor),
    )
    write_descriptor(descriptor, {**matching, "FINAL_TREE": "3" * 40})
    drifted = run(*compatible)
    require(
        drifted.returncode != 0
        and f"{DESCRIPTOR_PREFIX}FINAL_TREE={'3' * 40}, but the bundle manifest has "
        f"{'2' * 40}" in drifted.stderr,
        f"a drifted descriptor did not fail the verifier: {drifted}",
    )
    # With a matching descriptor the check passes, and the bundle, which holds
    # only its manifest, fails the payload inventory that follows it.
    write_descriptor(descriptor, matching)
    inventory = run(*compatible)
    require(
        inventory.returncode != 0
        and "artifact tree differs from its fixed contract" in inventory.stderr
        and "accepted descriptor" not in inventory.stderr,
        f"a matching descriptor did not reach the inventory check: {inventory}",
    )
    tracked = run("--kernel-metadata", str(root / "kernel.env"), "--descriptor", str(descriptor))
    require(
        tracked.returncode == 2 and "takes no --descriptor" in tracked.stderr,
        f"tracked verification accepted a descriptor: {tracked}",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-module", type=Path, required=True)
    parser.add_argument("--schema", type=Path, required=True)
    arguments = parser.parse_args()
    module = load_artifact_module(arguments.artifact_module.resolve())

    require(
        schema_identities(arguments.schema.resolve()) == SUPPORTED_IDENTITIES,
        "artifact JSON schema exposes the wrong identity matrix",
    )
    for identity in SUPPORTED_IDENTITIES:
        require_accepted(module, *identity)
    # Only the current schema is a manifest: the previous one, a schema 3
    # manifest with the previous one's patch count, and a boolean are not.
    retired = manifest(module, "mainline-6.18", "arm64", 4096)
    retired["schema_version"] = 2
    patch_count = manifest(module, "mainline-6.18", "arm64", 4096)
    patch_count["source"]["patch_count"] = 1
    boolean = manifest(module, "mainline-6.18", "arm64", 4096)
    boolean["schema_version"] = True
    for value, diagnostic in (
        (retired, "artifact manifest schema version is unsupported"),
        (boolean, "artifact manifest schema version is unsupported"),
        (patch_count, "source keys differ"),
    ):
        try:
            module.validate_manifest(value)
        except SystemExit as error:
            require(diagnostic in str(error), f"manifest rejected oddly: {error}")
        else:
            raise AssertionError(f"a manifest was accepted, expected: {diagnostic}")

    require_rejected(
        module,
        "android-6.18",
        "x86_64",
        4096,
        "x86_64 kernel slice requires the mainline-6.18 family",
    )
    # The retired 16 KiB profile is no artifact identity at all.
    for family, kernel_slice in (
        ("mainline-6.18", "arm64"),
        ("mainline-6.18", "x86_64"),
        ("android-6.18", "arm64"),
    ):
        require_rejected(
            module, family, kernel_slice, 16384, "kernel page size is unsupported"
        )

    with tempfile.TemporaryDirectory(prefix="uml-kernel-config-contract-") as raw:
        root = Path(raw)
        verify_bundle_page_size_contract(module, root)
        verify_descriptor_contract(module, root)
        for kernel_slice in ("arm64", "x86_64"):
            expected = config_values(kernel_slice)
            require_config_accepted(module, root, kernel_slice, expected)
            for symbol in (
                "CONFIG_BPF",
                "CONFIG_BPF_SYSCALL",
                "CONFIG_CGROUP_BPF",
                "CONFIG_DEBUG_INFO_BTF",
            ):
                invalid = dict(expected)
                invalid[symbol] = "n" if expected[symbol] == "y" else "y"
                require_config_rejected(
                    module,
                    root,
                    kernel_slice,
                    invalid,
                    symbol,
                    expected[symbol],
                )

        text = (".text", SHT_PROGBITS, SHF_ALLOC)
        btf = (".BTF", SHT_PROGBITS, SHF_ALLOC)
        require_runtime_image(module, root, "btf", [text, btf], None)
        require_runtime_image(
            module,
            root,
            "dwarf",
            [text, btf, (".debug_info", SHT_PROGBITS, 0)],
            "kernel image carries DWARF sections: .debug_info",
        )
        require_runtime_image(
            module,
            root,
            "missing-btf",
            [text],
            "kernel config enables BTF but the image has no .BTF contents",
        )
        require_runtime_image(
            module,
            root,
            "unallocated-btf",
            [text, (".BTF", SHT_PROGBITS, 0)],
            "kernel .BTF section is not allocated",
        )
        verify_kernel_elf_contract(module, root)
        verify_config_policy_contract(module, root)

    print("kernel artifact family/slice/page contract verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
