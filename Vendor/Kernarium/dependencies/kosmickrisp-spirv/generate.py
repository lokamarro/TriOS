#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate and check KosmicKrisp's prebuilt device-kernel SPIR-V.

The Apple Mesa build keeps LLVM out of its production closure, so Mesa's
``kosmickrisp: embed LLVM-free build shaders`` topic carries ``libkk.spv`` and
``libpoly.spv`` as prebuilt ``mesa_clc`` output instead of compiling the
OpenCL sources. This script owns that compile and its record:

``generate``
    Ship a clean Mesa checkout's ``HEAD`` to the pinned Debian build host,
    build ``mesa_clc`` there from that tree, compile both blobs, copy them to
    ``--output`` and rewrite ``record.tsv``. It needs SSH to the host and
    nothing else: the host must already carry the packages ``toolchain.tsv``
    pins, which the build step verifies and never installs.
``verify``
    The full comparison. Regenerate from a checkout of the committed Mesa
    series and require blobs byte-identical to the checkout's and a record
    identical to ``record.tsv``.
``check``
    The cheap guard, from the checked-in series alone: the embedded blobs are
    the recorded ones, the recorded ``KERNEL()`` names are exactly each blob's
    entry points, every input a topic touches still has its recorded content,
    and the Mesa pin and the pinned packages are the recorded ones.
``build``
    The Debian half of ``generate``; it is not run directly.

``record.tsv`` is generated output. Regenerate it; never edit it by hand.
"""

from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import os
import platform
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path

sys.dont_write_bytecode = True

HERE = Path(__file__).resolve().parent
DEFAULT_ROOT = HERE.parents[1]
RECORD = Path("dependencies/kosmickrisp-spirv/record.tsv")
TOOLCHAIN = Path("dependencies/kosmickrisp-spirv/toolchain.tsv")
MANIFEST = Path("dependencies/manifest.tsv")
DEPENDENCY = "mesa"
EMBED_SUBJECT = "kosmickrisp: embed LLVM-free build shaders"
DEFAULT_REMOTE_ROOT = "uml-kosmickrisp-spirv"
OBJECT_ID = re.compile(r"^[0-9a-f]{40}$")
ZERO_ID = "0" * 40
SPIRV_MAGIC = 0x07230203
OP_ENTRY_POINT = 15
KERNEL_DEFINITION = re.compile(r"\bKERNEL(?:2D|3D)?\s*\([^)]*\)\s*([A-Za-z_]\w*)\s*\(")
SHADER_FILES = re.compile(r"^(\w+)\s*=\s*files\(([^)]*)\)", re.MULTILINE)
QUOTED = re.compile(r"'([^']+)'")

# The Meson options that build mesa_clc and nothing else from a Mesa tree.
MESON_OPTIONS = (
    "--buildtype=release",
    "--wrap-mode=nodownload",
    "-Dmesa-clc=enabled",
    "-Dinstall-mesa-clc=true",
    "-Dllvm=enabled",
    "-Dspirv-tools=enabled",
    "-Dplatforms=",
    "-Dvulkan-drivers=",
    "-Dgallium-drivers=",
    "-Dopengl=false",
    "-Dglx=disabled",
    "-Degl=disabled",
    "-Dgbm=disabled",
    "-Dgles1=disabled",
    "-Dgles2=disabled",
    "-Dzstd=disabled",
    "-Dexpat=disabled",
    "-Dvalgrind=disabled",
    "-Dlibunwind=disabled",
    "-Dlmsensors=disabled",
    "-Dbuild-tests=false",
)

# The clang arguments Mesa's meson.build calls cl_args for a release build:
# the release build defines NDEBUG, so the libcl assert() the kernels use
# compiles to nothing. -fmacro-prefix-map entries for the source and build
# roots are added per build.
CL_ARGUMENTS = (
    "-cl-std=cl2.0",
    "-D__OPENCL_VERSION__=200",
    "-DHAVE___BUILTIN_FFS",
    "-DHAVE___BUILTIN_CLZ",
    "-DNDEBUG",
)


@dataclass(frozen=True)
class Output:
    """One blob and the mesa_clc rule the embed topic replaced."""

    name: str
    path: str
    meson: str
    variable: str
    includes: tuple[str, ...]

    @property
    def directory(self) -> str:
        return str(Path(self.path).parent)


OUTPUTS = (
    Output(
        name="libkk",
        path="src/kosmickrisp/libkk/libkk.spv",
        meson="src/kosmickrisp/libkk/meson.build",
        variable="libkk_shader_files",
        includes=(
            "include",
            "src/compiler/libcl",
            "src/poly/cl",
            "src/kosmickrisp/libkk",
            "src",
            "src/kosmickrisp/libkk/shaders",
        ),
    ),
    Output(
        name="libpoly",
        path="src/poly/cl/libpoly.spv",
        meson="src/poly/cl/meson.build",
        variable="libpoly_shader_files",
        includes=("include", "src/compiler/libcl", "src/poly/cl", "src"),
    ),
)


class SpirvError(Exception):
    pass


def fail(message: str) -> None:
    raise SpirvError(message)


# --------------------------------------------------------------------------
# Shared helpers


def run(
    command: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    stdin: object = subprocess.DEVNULL,
    timeout: float = 600,
    capture: bool = True,
) -> str:
    try:
        result = subprocess.run(
            command,
            cwd=cwd,
            env=env,
            stdin=stdin,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.PIPE if capture else None,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        fail(f"timed out after {timeout:.0f} s: {shlex.join(command)}")
    except OSError as error:
        fail(f"cannot run {command[0]}: {error}")
    if result.returncode != 0:
        detail = ""
        if capture:
            detail = (result.stderr or b"").decode(errors="replace").strip()
            detail = "\n".join(detail.splitlines()[-40:])
        fail(f"{shlex.join(command)} exited {result.returncode}\n{detail}".rstrip())
    return (result.stdout or b"").decode(errors="replace") if capture else ""


def load_patch_series():
    """Import this repository's series reader; the Debian half never needs it."""
    sys.path.insert(0, str(DEFAULT_ROOT / "scripts"))
    import patch_series

    return patch_series


def git_blob_id(data: bytes) -> str:
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def entry_points(data: bytes, label: str) -> list[str]:
    """Return the OpEntryPoint names of a little-endian SPIR-V module."""
    if len(data) < 20 or len(data) % 4:
        fail(f"{label}: not a SPIR-V module ({len(data)} bytes)")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if words[0] != SPIRV_MAGIC:
        fail(f"{label}: bad SPIR-V magic {words[0]:#010x}")
    names = []
    index = 5
    while index < len(words):
        count, opcode = words[index] >> 16, words[index] & 0xFFFF
        if count == 0 or index + count > len(words):
            fail(f"{label}: truncated SPIR-V instruction at word {index}")
        if opcode == OP_ENTRY_POINT:
            raw = struct.pack(f"<{count - 3}I", *words[index + 3 : index + count])
            names.append(raw.split(b"\0", 1)[0].decode())
        index += count
    return names


def kernels_in(texts: list[str]) -> list[str]:
    names = []
    for text in texts:
        names.extend(KERNEL_DEFINITION.findall(text))
    return sorted(names)


def shader_files(meson_text: str, variable: str, label: str) -> list[str]:
    for name, body in SHADER_FILES.findall(meson_text):
        if name == variable:
            files = QUOTED.findall(body)
            if not files:
                fail(f"{label}: {variable} names no files")
            return files
    fail(f"{label}: no '{variable} = files(...)' list")
    return []


def source_digest(base: str, inputs: dict[str, str]) -> str:
    text = f"mesa {base}\n" + "".join(
        f"{blob} {path}\n" for path, blob in sorted(inputs.items())
    )
    return hashlib.sha256(text.encode()).hexdigest()


def read_tsv(path: Path, header: tuple[str, ...]) -> list[dict[str, str]]:
    try:
        lines = [
            line
            for line in path.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")
        ]
    except OSError as error:
        fail(f"cannot read {path}: {error}")
    reader = csv.DictReader(lines, delimiter="\t", quoting=csv.QUOTE_NONE)
    if tuple(reader.fieldnames or ()) != header:
        fail(f"{path}: header is not {' '.join(header)}")
    rows = list(reader)
    for row in rows:
        if None in row or None in row.values():
            fail(f"{path}: malformed row {row}")
    return rows


TOOLCHAIN_HEADER = ("kind", "name", "version", "note")


def toolchain_digest(path: Path) -> str:
    """SHA-256 over the pinned suite and package versions, not the notes."""
    pins = "".join(
        f"{row['kind']}\t{row['name']}\t{row['version']}\n"
        for row in read_tsv(path, TOOLCHAIN_HEADER)
        if row["kind"] in ("suite", "package")
    )
    return hashlib.sha256(pins.encode()).hexdigest()


def read_toolchain(path: Path) -> tuple[str, dict[str, str]]:
    suite = None
    packages: dict[str, str] = {}
    for row in read_tsv(path, TOOLCHAIN_HEADER):
        if row["kind"] == "suite":
            suite = row["name"]
        elif row["kind"] == "package":
            packages[row["name"]] = row["version"]
        elif row["kind"] != "unused":
            fail(f"{path}: unknown row kind {row['kind']}")
    if suite is None or not packages:
        fail(f"{path}: needs a suite row and package rows")
    return suite, packages


# --------------------------------------------------------------------------
# The record


RECORD_HEADER = ("kind", "output", "name", "value")
RECORD_PREAMBLE = """\
# SPDX-License-Identifier: GPL-2.0-or-later
# Generated by dependencies/kosmickrisp-spirv/generate.py; never edit by hand.
# For each prebuilt blob: its Git blob ID and SHA-256, the KERNEL() entry
# points of its OpenCL sources, and the Git blob ID of every in-tree input
# mesa_clc read (its depfile), at the head of the Mesa series. The source row
# is a SHA-256 over the Mesa pin and those inputs. libpoly is a function
# library with no kernels, so its inputs and bytes are its whole check.
"""


@dataclass
class Record:
    toolchain: str
    base: str
    blobs: dict[str, str]
    sha256: dict[str, str]
    sources: dict[str, str]
    inputs: dict[str, dict[str, str]]
    kernels: dict[str, list[str]]

    def text(self) -> str:
        rows = [
            ("toolchain", "-", TOOLCHAIN.name, self.toolchain),
            ("base", "-", DEPENDENCY, self.base),
        ]
        for output in OUTPUTS:
            name = output.name
            rows.append(("blob", name, output.path, self.blobs[name]))
            rows.append(("sha256", name, output.path, self.sha256[name]))
            rows.append(("source", name, "inputs", self.sources[name]))
            rows.extend(("kernel", name, kernel, "-") for kernel in self.kernels[name])
            rows.extend(
                ("input", name, path, blob)
                for path, blob in sorted(self.inputs[name].items())
            )
        lines = ["\t".join(RECORD_HEADER)] + ["\t".join(row) for row in rows]
        return RECORD_PREAMBLE + "\n".join(lines) + "\n"


def read_record(path: Path) -> Record:
    names = {output.name for output in OUTPUTS}
    record = Record("", "", {}, {}, {}, {n: {} for n in names}, {n: [] for n in names})
    for row in read_tsv(path, RECORD_HEADER):
        kind, output, name, value = (row[key] for key in RECORD_HEADER)
        if kind == "toolchain":
            record.toolchain = value
        elif kind == "base":
            record.base = value
        elif output not in names:
            fail(f"{path}: unknown output {output}")
        elif kind == "blob":
            record.blobs[output] = value
        elif kind == "sha256":
            record.sha256[output] = value
        elif kind == "source":
            record.sources[output] = value
        elif kind == "kernel":
            record.kernels[output].append(name)
        elif kind == "input":
            if name in record.inputs[output]:
                fail(f"{path}: duplicate input {output} {name}")
            record.inputs[output][name] = value
        else:
            fail(f"{path}: unknown row kind {kind}")
    for output in OUTPUTS:
        for field in (record.blobs, record.sha256, record.sources):
            if output.name not in field:
                fail(f"{path}: {output.name} is incomplete")
        if not record.inputs[output.name]:
            fail(f"{path}: {output.name} records no inputs")
    if not record.toolchain or not OBJECT_ID.match(record.base):
        fail(f"{path}: toolchain or base row is missing")
    return record


# --------------------------------------------------------------------------
# The cheap guard: the checked-in series alone


def manifest_series(root: Path) -> tuple[str, list[Path]]:
    for row in read_tsv(
        root / MANIFEST,
        ("name", "version", "products", "repository", "commit", "source_paths",
         "patch_series"),
    ):
        if row["name"] != DEPENDENCY:
            continue
        layers = []
        for layer in row["patch_series"].split("+"):
            if layer.startswith("rootfs-vulkan:"):
                series = root / "tools/rootfs/vulkan" / layer.split(":", 1)[1]
            else:
                series = root / "dependencies" / layer
            layers.append(series.parent)
        return row["commit"], layers
    fail(f"{root / MANIFEST}: no {DEPENDENCY} row")
    return "", []


@dataclass
class FileDiff:
    old: str
    new: str
    new_id: str | None
    created: bool
    deleted: bool
    literal: bytes | None


def decode_literal(lines: list[str], label: str) -> bytes:
    """Decode the forward half of a 'GIT binary patch' literal."""
    if not lines or not lines[0].startswith("literal "):
        fail(f"{label}: binary patch is not a literal")
    size = int(lines[0].split()[1])
    packed = bytearray()
    for line in lines[1:]:
        if not line:
            break
        length = line[0]
        if "A" <= length <= "Z":
            count = ord(length) - ord("A") + 1
        elif "a" <= length <= "z":
            count = ord(length) - ord("a") + 27
        else:
            fail(f"{label}: malformed binary patch line")
        try:
            packed += base64.b85decode(line[1:])[:count]
        except ValueError as error:
            fail(f"{label}: malformed binary patch line: {error}")
    try:
        data = zlib.decompress(bytes(packed))
    except zlib.error as error:
        fail(f"{label}: binary patch does not inflate: {error}")
    if len(data) != size:
        fail(f"{label}: binary patch holds {len(data)} bytes, not {size}")
    return data


def parse_patch(path: Path) -> tuple[str, list[FileDiff]]:
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    lines = text.split("\n")
    subject = ""
    for line in lines:
        if line.startswith("Subject: "):
            subject = line[len("Subject: ") :].removeprefix("[PATCH] ")
            break
    try:
        start = lines.index("---")
    except ValueError:
        fail(f"{path.name}: no '---' separator")
    diffs: list[FileDiff] = []
    current: FileDiff | None = None
    index = start
    while index < len(lines):
        line = lines[index]
        if line.startswith("diff --git a/"):
            old, _, new = line[len("diff --git a/") :].partition(" b/")
            current = FileDiff(old, new, None, False, False, None)
            diffs.append(current)
        elif current is not None and current.literal is None:
            if line.startswith("new file mode "):
                current.created = True
            elif line.startswith("deleted file mode "):
                current.deleted = True
            elif line.startswith("rename from "):
                current.old = line[len("rename from ") :]
            elif line.startswith("rename to "):
                current.new = line[len("rename to ") :]
            elif line.startswith("index ") and ".." in line:
                current.new_id = line.split()[1].split("..", 1)[1]
            elif line == "GIT binary patch":
                end = index + 1
                while end < len(lines) and lines[end]:
                    end += 1
                current.literal = decode_literal(
                    lines[index + 1 : end], f"{path.name}: {current.new}"
                )
                index = end
        index += 1
    return subject, diffs


def check(root: Path) -> None:
    problems: list[str] = []
    record = read_record(root / RECORD)
    toolchain = toolchain_digest(root / TOOLCHAIN)
    if record.toolchain != toolchain:
        problems.append(
            f"the pins in {TOOLCHAIN} changed since the blobs were generated "
            f"(record {record.toolchain}, file {toolchain})"
        )
    base, layers = manifest_series(root)
    if record.base != base:
        problems.append(f"the Mesa pin is {base}, but the blobs were generated at {record.base}")

    patch_series = load_patch_series()

    # Final content of every path the series touches, in application order.
    final: dict[str, tuple[str, str]] = {}
    writers: dict[str, list[tuple[str, FileDiff]]] = {}
    for directory in layers:
        try:
            series = patch_series.read_series(directory)
        except patch_series.SeriesError as error:
            fail(str(error))
        for patch in series.patches:
            subject, diffs = parse_patch(patch)
            for diff in diffs:
                writers.setdefault(diff.new, []).append((subject, diff))
                if diff.old != diff.new:
                    final[diff.old] = (ZERO_ID, patch.name)
                if diff.new_id is None:
                    # A pure rename keeps the content it had under its old name.
                    previous = final.get(diff.old)
                    if previous is None:
                        problems.append(f"{patch.name}: {diff.new} has no index line")
                        continue
                    final[diff.new] = (previous[0], patch.name)
                else:
                    final[diff.new] = (ZERO_ID if diff.deleted else diff.new_id, patch.name)

    for output in OUTPUTS:
        name = output.name
        changes = writers.get(output.path, [])
        if len(changes) != 1 or changes[0][0] != EMBED_SUBJECT:
            problems.append(
                f"{output.path} must be written only by the '{EMBED_SUBJECT}' topic; "
                f"written by {[subject for subject, _ in changes]}"
            )
            continue
        diff = changes[0][1]
        if not diff.created or diff.literal is None:
            problems.append(f"{output.path}: the embed topic must add it as a binary literal")
            continue
        blob = git_blob_id(diff.literal)
        if blob != diff.new_id:
            problems.append(f"{output.path}: literal hashes to {blob}, index line says {diff.new_id}")
        if blob != record.blobs[name]:
            problems.append(
                f"{output.path}: the series embeds {blob}, the record names {record.blobs[name]}"
            )
        digest = hashlib.sha256(diff.literal).hexdigest()
        if digest != record.sha256[name]:
            problems.append(f"{output.path}: SHA-256 {digest} is not the recorded {record.sha256[name]}")
        points = entry_points(diff.literal, output.path)
        if sorted(points) != sorted(record.kernels[name]):
            missing = sorted(set(record.kernels[name]) - set(points))
            extra = sorted(set(points) - set(record.kernels[name]))
            problems.append(
                f"{output.path}: KERNEL() names and entry points differ; "
                f"missing from the blob {missing}, not in the sources {extra}"
            )
        for path, recorded in sorted(record.inputs[name].items()):
            current = final.get(path)
            if current is not None and current[0] != recorded:
                problems.append(
                    f"{name} input {path} is {current[0]} after {current[1]}, "
                    f"but the blob was generated from {recorded}"
                )
        if source_digest(record.base, record.inputs[name]) != record.sources[name]:
            problems.append(f"{name}: the recorded source digest does not match its inputs")

    if problems:
        fail(
            "the prebuilt KosmicKrisp SPIR-V does not match the Mesa series:\n  "
            + "\n  ".join(problems)
            + "\nregenerate the blobs with dependencies/kosmickrisp-spirv/generate.py generate,"
            + " amend the embed topic and regenerate the series"
        )


# --------------------------------------------------------------------------
# The Debian half


def clean_environment() -> dict[str, str]:
    return {
        "PATH": "/usr/bin:/bin",
        "HOME": os.environ.get("HOME", "/nonexistent"),
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "CC": "gcc",
        "CXX": "g++",
        "TMPDIR": os.environ.get("TMPDIR", "/tmp"),
    }


def os_release() -> dict[str, str]:
    values = {}
    for line in Path("/etc/os-release").read_text().splitlines():
        key, _, value = line.partition("=")
        values[key] = value.strip('"')
    return values


def dpkg_version(package: str) -> str | None:
    result = subprocess.run(
        ["dpkg-query", "-W", "-f=${Status}\t${Version}", package],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        stdin=subprocess.DEVNULL,
        check=False,
    )
    status, _, version = result.stdout.decode().partition("\t")
    if result.returncode != 0 or not status.endswith(" installed"):
        return None
    return version


def build(source: Path, work: Path, output_dir: Path, toolchain: Path) -> None:
    suite, packages = read_toolchain(toolchain)
    if platform.system() != "Linux" or platform.machine() != "aarch64":
        fail("the build step runs on arm64 Debian")
    codename = os_release().get("VERSION_CODENAME")
    if codename != suite:
        fail(f"the build host runs Debian {codename}, not the pinned {suite}")
    wrong = []
    for package, version in packages.items():
        installed = dpkg_version(package)
        if installed != version:
            wrong.append(f"{package} {installed or 'not installed'} (pinned {version})")
    if wrong:
        fail("the build host's toolchain is not the pinned one:\n  " + "\n  ".join(wrong))

    if work.exists() or output_dir.exists():
        fail("the build and output directories must not exist yet")
    output_dir.mkdir(parents=True)
    # The compiler runs in the source directory.
    output_dir = output_dir.resolve()
    source = source.resolve()
    environment = clean_environment()
    run(["meson", "setup", str(work), str(source), *MESON_OPTIONS], env=environment,
        timeout=900)
    run(["ninja", "-C", str(work), "src/compiler/clc/mesa_clc"], env=environment,
        timeout=3600)
    work = work.resolve()
    compiler = work / "src/compiler/clc/mesa_clc"
    prefix_maps = (f"-fmacro-prefix-map={source}/=", f"-fmacro-prefix-map={work}/=")
    for output in OUTPUTS:
        files = shader_files((source / output.meson).read_text(), output.variable,
                             output.meson)
        run(
            [str(compiler), "-o", str(output_dir / f"{output.name}.spv"),
             "--depfile", str(output_dir / f"{output.name}.d"),
             *(f"{output.directory}/{name}" for name in files), "--",
             *(f"-I{include}" for include in output.includes),
             *prefix_maps, *CL_ARGUMENTS],
            cwd=source,
            env=environment,
            timeout=900,
        )
    observed = [f"package\t{package}\t{dpkg_version(package)}" for package in packages]
    for tool in (["gcc", "--version"], ["meson", "--version"], ["ninja", "--version"]):
        observed.append(f"tool\t{tool[0]}\t{run(tool, env=environment).splitlines()[0]}")
    observed.append(f"host\tos\t{os_release().get('PRETTY_NAME')}")
    observed.append(f"host\tkernel\t{platform.release()}")
    (output_dir / "observed.tsv").write_text("\n".join(observed) + "\n")


# --------------------------------------------------------------------------
# The Mac half


def git(checkout: Path, *arguments: str) -> str:
    return run(["git", "-C", str(checkout), *arguments]).strip()


def remote(host: str, command: str, *, stdin: object = subprocess.DEVNULL,
           timeout: float = 600, capture: bool = True) -> str:
    return run(["ssh", "-o", "BatchMode=yes", host, command], stdin=stdin,
               timeout=timeout, capture=capture)


def depfile_inputs(path: Path, label: str) -> list[str]:
    text = path.read_text().replace("\\\n", " ")
    _, _, body = text.partition(":")
    inputs = []
    for entry in body.split():
        if entry.startswith("/"):
            # Only the pinned clang's OpenCL headers live outside the tree.
            if not entry.startswith("/usr/lib/llvm-"):
                fail(f"{label}: unexpected out-of-tree input {entry}")
            continue
        inputs.append(os.path.normpath(entry))
    return sorted(set(inputs))


def compile_remotely(checkout: Path, host: str, remote_root: str, keep: bool,
                     destination: Path) -> None:
    if git(checkout, "status", "--porcelain", "--untracked-files=no"):
        fail(f"{checkout}: the checkout has uncommitted changes")
    run_dir = remote(
        host,
        f"mkdir -p {shlex.quote(remote_root)} && "
        f"CDPATH= cd -- {shlex.quote(remote_root)} && mktemp -d \"$PWD/run.XXXXXX\"",
    ).strip()
    if not run_dir.startswith("/"):
        fail(f"{host}: could not create a run directory")
    quoted = shlex.quote(run_dir)
    try:
        for local, name in ((Path(__file__), "generate.py"),
                            (DEFAULT_ROOT / TOOLCHAIN, "toolchain.tsv")):
            with local.open("rb") as stream:
                remote(host, f"cat > {quoted}/{name}", stdin=stream)
        archive = subprocess.Popen(
            ["git", "-C", str(checkout), "archive", "--format=tar", "HEAD"],
            stdout=subprocess.PIPE, stdin=subprocess.DEVNULL,
        )
        try:
            remote(host, f"mkdir {quoted}/source && tar -xf - -C {quoted}/source",
                   stdin=archive.stdout, timeout=1200)
        finally:
            archive.stdout.close()
            if archive.wait(timeout=60) != 0:
                fail(f"{checkout}: git archive failed")
        remote(
            host,
            f"python3 -B {quoted}/generate.py build --source {quoted}/source "
            f"--work {quoted}/build --output {quoted}/output "
            f"--toolchain {quoted}/toolchain.tsv",
            timeout=5400,
        )
        with tempfile.TemporaryFile() as bundle:
            result = subprocess.run(
                ["ssh", "-o", "BatchMode=yes", host, f"tar -cf - -C {quoted}/output ."],
                stdin=subprocess.DEVNULL, stdout=bundle, stderr=subprocess.PIPE,
                timeout=600, check=False,
            )
            if result.returncode != 0:
                fail(f"{host}: could not fetch the outputs: {result.stderr.decode().strip()}")
            bundle.seek(0)
            with tarfile.open(fileobj=bundle) as tar:
                for member in tar.getmembers():
                    if member.isdir():
                        continue
                    if not member.isfile() or "/" in member.name.removeprefix("./"):
                        fail(f"{host}: unexpected output entry {member.name}")
                    data = tar.extractfile(member).read()
                    (destination / member.name.removeprefix("./")).write_bytes(data)
    finally:
        if not keep:
            remote(host, f"rm -rf {quoted}")


def generate(checkout: Path, host: str, remote_root: str, keep: bool,
             output: Path, record_path: Path) -> Record:
    checkout = checkout.resolve()
    if output.exists() and any(output.iterdir()):
        fail(f"{output}: the output directory is not empty")
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="uml-kosmickrisp-spirv.") as scratch:
        fetched = Path(scratch)
        compile_remotely(checkout, host, remote_root, keep, fetched)
        base, _ = manifest_series(DEFAULT_ROOT)
        record = Record(toolchain_digest(DEFAULT_ROOT / TOOLCHAIN), base,
                        {}, {}, {}, {}, {})
        for item in OUTPUTS:
            blob = fetched / f"{item.name}.spv"
            if not blob.is_file():
                fail(f"{host} produced no {blob.name}")
            data = blob.read_bytes()
            files = shader_files(git(checkout, "show", f"HEAD:{item.meson}"),
                                 item.variable, item.meson)
            kernels = kernels_in(
                [git(checkout, "show", f"HEAD:{item.directory}/{name}") for name in files]
            )
            points = entry_points(data, blob.name)
            if sorted(points) != kernels:
                fail(f"{blob.name}: entry points {sorted(points)} are not the KERNEL() "
                     f"names {kernels}")
            inputs = depfile_inputs(fetched / f"{item.name}.d", item.name)
            for required in (item.meson, *(f"{item.directory}/{n}" for n in files)):
                if required not in inputs:
                    inputs.append(required)
            record.inputs[item.name] = {
                path: git(checkout, "rev-parse", f"HEAD:{path}") for path in inputs
            }
            record.blobs[item.name] = git_blob_id(data)
            record.sha256[item.name] = hashlib.sha256(data).hexdigest()
            record.kernels[item.name] = kernels
            record.sources[item.name] = source_digest(base, record.inputs[item.name])
            shutil.copyfile(blob, output / blob.name)
        shutil.copyfile(fetched / "observed.tsv", output / "observed.tsv")
    record_path.write_text(record.text())
    return record


def verify(checkout: Path, host: str, remote_root: str, keep: bool) -> None:
    checkout = checkout.resolve()
    patch_series = load_patch_series()
    _, layers = manifest_series(DEFAULT_ROOT)
    tree = patch_series.read_series(layers[-1]).tree
    head_tree = git(checkout, "rev-parse", "HEAD^{tree}")
    if head_tree != tree:
        fail(f"{checkout}: HEAD is tree {head_tree}, not the committed series tree {tree}")
    with tempfile.TemporaryDirectory(prefix="uml-kosmickrisp-spirv-verify.") as scratch:
        scratch_path = Path(scratch)
        record_path = scratch_path / "record.tsv"
        generate(checkout, host, remote_root, keep, scratch_path / "output", record_path)
        problems = []
        for item in OUTPUTS:
            generated = (scratch_path / "output" / f"{item.name}.spv").read_bytes()
            committed = subprocess.run(
                ["git", "-C", str(checkout), "show", f"HEAD:{item.path}"],
                stdout=subprocess.PIPE, stdin=subprocess.DEVNULL, check=True,
            ).stdout
            if generated != committed:
                problems.append(
                    f"{item.path}: the series embeds {git_blob_id(committed)}, the "
                    f"generator produces {git_blob_id(generated)}"
                )
        if record_path.read_text() != (DEFAULT_ROOT / RECORD).read_text():
            problems.append(f"{RECORD} differs from the regenerated record")
        if problems:
            fail("regeneration does not reproduce the committed SPIR-V:\n  "
                 + "\n  ".join(problems))
    check(DEFAULT_ROOT)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("generate", "verify"):
        command = commands.add_parser(name)
        command.add_argument("--source", type=Path, required=True,
                             help="clean Mesa topic checkout whose HEAD is compiled")
        command.add_argument("--host", required=True,
                             help="SSH host running arm64 Debian with the pinned packages")
        command.add_argument("--remote-root", default=DEFAULT_REMOTE_ROOT,
                             help="work directory on the host, relative to its home")
        command.add_argument("--keep-remote", action="store_true",
                             help="keep the host's run directory for inspection")
        if name == "generate":
            command.add_argument("--output", type=Path, required=True,
                                 help="empty directory that receives both blobs")
    guard = commands.add_parser("check")
    guard.add_argument("--root", type=Path, default=DEFAULT_ROOT,
                       help="repository root holding the series and record")
    remote_build = commands.add_parser("build")
    for option in ("--source", "--work", "--output", "--toolchain"):
        remote_build.add_argument(option, type=Path, required=True)
    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "check":
            check(arguments.root.resolve())
        elif arguments.command == "build":
            build(arguments.source, arguments.work, arguments.output, arguments.toolchain)
        elif arguments.command == "generate":
            record = generate(arguments.source, arguments.host, arguments.remote_root,
                              arguments.keep_remote, arguments.output,
                              DEFAULT_ROOT / RECORD)
            for item in OUTPUTS:
                print(f"{item.path}\t{record.blobs[item.name]}\t{record.sha256[item.name]}")
        else:
            verify(arguments.source, arguments.host, arguments.remote_root,
                   arguments.keep_remote)
    except SpirvError as error:
        print(f"generate.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
