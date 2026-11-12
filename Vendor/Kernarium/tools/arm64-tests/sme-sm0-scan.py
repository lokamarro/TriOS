#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Report the SVE words that shipped guest images execute outside streaming
mode in their SME code.

usage: sme-sm0-scan.py [--work DIR] [--report PREFIX] [--jobs N]
                       [--llvm-mc PATH] [--expect BINARY=ENC[,ENC...]]
                       INPUT [INPUT ...]

An INPUT is a directory tree, an EROFS or ext4 image, a GPT disk (an
Android release's guest image), an Android release directory, a .zst of
any of these, or one ELF file. The scan extracts every filesystem it finds,
descends into ZIP archives (APKs, APEXes) and the filesystem images and
Android boot-image ramdisks they hold, and analyses every AArch64 ELF file
with sme_sm0.py. Other machines (x86, x86-64, 32-bit Arm) are counted and
skipped; give the Steam image no INPUT.

Extraction needs fsck.erofs (with the image's decompressor), debugfs and
zstd on PATH; --work holds the extracted trees (default: a temporary
directory removed at exit). The report lists every SME function with its
inferred entry mode and every word its SM = 0 paths execute that is not
plain A64, in PREFIX.functions.tsv and PREFIX.words.tsv, and prints a
summary. --expect requires the ELF files whose path ends with BINARY to
execute exactly those SVE encodings (family and decision classes) at
SM = 0; the Chromium libyuv check is
--expect usr/lib/chromium/chromium=cntd_r_s_.

The exit status is 1 when a word at SM = 0 is a decision (classes sve and
streaming-only), an ELF file cannot be read, or an expectation fails, and
2 when an input cannot be extracted or holds no ELF file.
Unadvertised words (SME2 past the code's own feature check) are listed
for review.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import collections
import concurrent.futures
import gzip
import hashlib
import io
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sme_sm0  # noqa: E402

EROFS_MAGIC = 0xE0F5E1E2
EXT_MAGIC = 0xEF53
ZIP_SUFFIXES = (".apk", ".apex", ".capex", ".jar", ".zip")


class ScanError(RuntimeError):
    pass


def tool(name: str) -> str:
    found = shutil.which(name)
    if found is None:
        raise ScanError(f"{name} is not on PATH")
    return found


def run(command: list[str], timeout: int = 3600) -> None:
    command = [tool(command[0])] + command[1:]
    result = subprocess.run(command, stdin=subprocess.DEVNULL,
                            capture_output=True, text=True, timeout=timeout)
    if result.returncode != 0:
        raise ScanError(f"{' '.join(command)} failed: "
                        f"{result.stderr.strip()[-2000:]}")


def detect(head: bytes) -> str | None:
    """The container kind of a file from its first 4 KiB."""
    if head[:4] == b"\x7fELF":
        return "elf"
    if head[:4] == b"PK\x03\x04":
        return "zip"
    if head[:4] == b"\x28\xb5\x2f\xfd":
        return "zstd"
    if head[:2] == b"\x1f\x8b":
        return "gzip"
    if head[:8] == b"ANDROID!":
        return "bootimg"
    if head[:6] in (b"070701", b"070702"):
        return "cpio"
    if len(head) >= 520 and head[512:520] == b"EFI PART":
        return "gpt"
    if len(head) >= 1028 and struct.unpack_from("<I", head, 1024)[0] == \
            EROFS_MAGIC:
        return "erofs"
    if len(head) >= 1082 and struct.unpack_from("<H", head, 1080)[0] == \
            EXT_MAGIC:
        return "ext4"
    return None


class Extractor:
    """Turns inputs into trees of files and lists the ELF files in them,
    as (image, display path, source) items; the source is the file to read,
    under --work for anything extracted from an archive or a ramdisk."""

    def __init__(self, work: pathlib.Path):
        self.work = work
        self.count = 0
        self.skipped: collections.Counter = collections.Counter()

    def _new_dir(self, label: str) -> pathlib.Path:
        self.count += 1
        path = self.work / f"{self.count:04d}-{label}"
        path.mkdir(parents=True)
        return path

    def _extract_erofs(self, image: pathlib.Path, offset: int = 0
                       ) -> pathlib.Path:
        dest = self._new_dir("erofs")
        command = ["fsck.erofs", f"--extract={dest}", "--no-preserve"]
        if offset:
            command.append(f"--offset={offset}")
        run(command + [str(image)])
        return dest

    def _extract_ext4(self, image: pathlib.Path, offset: int = 0
                      ) -> pathlib.Path:
        dest = self._new_dir("ext4")
        spec = f"{image}?offset={offset}" if offset else str(image)
        # rdump warns that it cannot chown as an ordinary user; its exit
        # status says nothing either way, so check the tree instead.
        subprocess.run([tool("debugfs"), "-R", f"rdump / {dest}", spec],
                       stdin=subprocess.DEVNULL, capture_output=True,
                       timeout=3600)
        if not any(dest.iterdir()):
            raise ScanError(f"debugfs extracted nothing from {spec}")
        return dest

    def _decompress_zstd(self, path: pathlib.Path) -> pathlib.Path:
        dest = self._new_dir("zstd") / path.name.removesuffix(".zst")
        run(["zstd", "-q", "-d", "-f", str(path), "-o", str(dest)])
        return dest

    def items(self, image: str, path: pathlib.Path):
        """Yield (image, display, source) for the ELF files of an input."""
        if path.is_dir():
            manifest = path / "release-manifest.json"
            guest = path / "guest"
            if manifest.is_file() and guest.is_dir():
                disks = sorted(guest.glob("*.img")) or sorted(
                    guest.glob("*.img.zst"))
                for disk in disks:
                    yield from self.items(image, disk)
                return
            yield from self._tree(image, "", path)
            return
        with path.open("rb") as handle:
            head = handle.read(4096)
        kind = detect(head)
        if kind == "zstd":
            yield from self.items(image, self._decompress_zstd(path))
        elif kind == "gpt":
            yield from self._gpt(image, path)
        elif kind == "erofs":
            yield from self._tree(image, "", self._extract_erofs(path))
        elif kind == "ext4":
            yield from self._tree(image, "", self._extract_ext4(path))
        elif kind == "elf":
            yield image, path.name, str(path)
        else:
            raise ScanError(f"{path}: not a scannable input")

    def _gpt(self, image: str, disk: pathlib.Path):
        with disk.open("rb") as handle:
            handle.seek(512)
            header = handle.read(92)
            entries_lba, count, size = struct.unpack_from("<QII", header, 72)
            handle.seek(entries_lba * 512)
            table = handle.read(count * size)
            for index in range(count):
                entry = table[index * size:(index + 1) * size]
                if entry[:16] == bytes(16):
                    continue
                first, last = struct.unpack_from("<QQ", entry, 32)
                name = entry[56:128].decode("utf-16-le").rstrip("\0")
                offset = first * 512
                handle.seek(offset)
                head = handle.read(4096)
                kind = detect(head)
                label = f"{image}:{name}"
                if kind == "erofs":
                    yield from self._tree(label, "", self._extract_erofs(
                        disk, offset))
                elif kind == "ext4":
                    yield from self._tree(label, "", self._extract_ext4(
                        disk, offset))
                elif kind == "bootimg":
                    handle.seek(offset)
                    data = handle.read((last - first + 1) * 512)
                    yield from self._bootimg(label, "", data)
                else:
                    self.skipped[f"partition without files: {name}"] += 1

    def _bootimg(self, image: str, prefix: str, data: bytes):
        """The ramdisk of a boot image of header version 3 or 4."""
        kernel_size, ramdisk_size = struct.unpack_from("<II", data, 8)
        version = struct.unpack_from("<I", data, 40)[0]
        if version < 3:
            raise ScanError(f"{image}: boot image header version {version}")
        page = 4096
        start = page + (kernel_size + page - 1) // page * page
        ramdisk = data[start:start + ramdisk_size]
        yield from self._blob(image, prefix + "ramdisk", ramdisk)

    def _blob(self, image: str, display: str, data: bytes):
        kind = detect(data[:4096])
        if kind == "gzip":
            yield from self._blob(image, display, gzip.decompress(data))
        elif kind == "cpio":
            yield from self._cpio(image, display, data)
        elif kind == "elf":
            dest = self._new_dir("blob") / "file"
            dest.write_bytes(data)
            yield image, display, str(dest)
        elif kind is not None:
            self.skipped[f"{kind} blob"] += 1

    def _cpio(self, image: str, display: str, data: bytes):
        dest = self._new_dir("cpio")
        pos = 0
        while pos + 110 <= len(data):
            header = data[pos:pos + 110]
            if header[:6] not in (b"070701", b"070702"):
                break
            fields = [int(header[6 + 8 * i:14 + 8 * i], 16) for i in range(13)]
            mode, size, name_size = fields[1], fields[6], fields[11]
            name = data[pos + 110:pos + 110 + name_size - 1].decode(
                "utf-8", "replace")
            pos = (pos + 110 + name_size + 3) & ~3
            body = data[pos:pos + size]
            pos = (pos + size + 3) & ~3
            if name == "TRAILER!!!":
                break
            if mode & 0o170000 == 0o100000 and body[:4] == b"\x7fELF":
                target = dest / name.lstrip("/")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(body)
                yield image, f"{display}!/{name.lstrip('/')}", str(target)

    def _tree(self, image: str, prefix: str, root: pathlib.Path):
        for directory, directories, files in os.walk(root):
            directories.sort()
            for name in sorted(files):
                path = pathlib.Path(directory) / name
                if path.is_symlink() or not path.is_file():
                    continue
                relative = prefix + path.relative_to(root).as_posix()
                try:
                    with path.open("rb") as handle:
                        head = handle.read(4096)
                except OSError:
                    self.skipped["unreadable file"] += 1
                    continue
                kind = detect(head)
                if kind == "elf":
                    yield image, relative, str(path)
                elif kind == "zip" and name.endswith(ZIP_SUFFIXES):
                    yield from self._zip_guarded(image, relative, path)
                elif kind in ("erofs", "ext4"):
                    nested = (self._extract_erofs(path) if kind == "erofs"
                              else self._extract_ext4(path))
                    yield from self._tree(image, relative + "!/", nested)

    def _zip_guarded(self, image: str, display: str, source):
        """A ZIP archive's ELF files; an archive Python cannot read is
        counted and named, never silently passed over."""
        try:
            archive = zipfile.ZipFile(source)
            found = list(self._zip(image, display, archive))
        except (zipfile.BadZipFile, NotImplementedError, EOFError,
                OSError, ValueError) as error:
            self.skipped[f"unreadable archive {image}:{display} "
                         f"({error})"] += 1
            return
        yield from found

    def _zip(self, image: str, display: str, archive: zipfile.ZipFile):
        for info in archive.infolist():
            if info.is_dir() or info.file_size < 64:
                continue
            with archive.open(info) as member:
                head = member.read(4096)
            kind = detect(head)
            inner = f"{display}!/{info.filename}"
            if kind == "elf":
                dest = self._new_dir("member") / pathlib.PurePath(
                    info.filename).name
                with archive.open(info) as member, dest.open("wb") as out:
                    shutil.copyfileobj(member, out)
                yield image, inner, str(dest)
            elif kind == "zip":
                with archive.open(info) as member:
                    nested = io.BytesIO(member.read())
                yield from self._zip_guarded(image, inner, nested)
            elif kind in ("erofs", "ext4"):
                dest = self._new_dir("member") / pathlib.PurePath(
                    info.filename).name
                with archive.open(info) as member, dest.open("wb") as out:
                    shutil.copyfileobj(member, out)
                nested = (self._extract_erofs(dest) if kind == "erofs"
                          else self._extract_ext4(dest))
                yield from self._tree(image, inner + "!/", nested)


_TABLE = None


def analyse_file(path: str) -> dict:
    """The analysis of one ELF file, as plain data for the report."""
    global _TABLE
    data = pathlib.Path(path).read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    machine = sme_sm0.elf_machine(data)
    result = {"sha256": digest, "machine": machine, "functions": []}
    if machine != "aarch64":
        return result
    if _TABLE is None:
        _TABLE = sme_sm0.load_table()
    try:
        elf = sme_sm0.Elf(data)
    except (sme_sm0.NotAArch64, struct.error, IndexError) as error:
        result["error"] = str(error)
        return result
    analyzer = sme_sm0.Analyzer(elf, _TABLE)
    functions = analyzer.functions()
    result["data_like"] = sum(len(function.markers)
                              for function in analyzer.data_like)
    for function in functions:
        result["functions"].append({
            "name": function.name, "start": function.start,
            "end": function.end, "bounds": function.bounds,
            "kind": function.kind,
            "markers": sorted({kind for _a, kind in function.markers}),
            "reached": f"{function.reached_markers}/{len(function.markers)}",
            "calls": [name for name in function.calls
                      if name.startswith("__arm_")],
            "unresolved_jump": function.unresolved_jump,
            "sm0": [(item.address, item.word, item.classified.cls,
                     item.classified.encoding, item.classified.note,
                     sorted(item.states))
                    for item in function.sm0],
        })
    return result


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("inputs", nargs="+", type=pathlib.Path)
    parser.add_argument("--work", type=pathlib.Path)
    parser.add_argument("--report", type=pathlib.Path)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--llvm-mc")
    parser.add_argument("--expect", action="append", default=[],
                        metavar="BINARY=ENC[,ENC...]")
    arguments = parser.parse_args(argv)
    expectations = {}
    for item in arguments.expect:
        binary, _, encodings = item.partition("=")
        if not binary or not encodings:
            parser.error(f"--expect {item!r}: want BINARY=ENC[,ENC...]")
        expectations[binary] = frozenset(encodings.split(","))
    llvm_mc = sme_sm0.find_llvm_mc(arguments.llvm_mc)

    temporary = None
    work = arguments.work
    if work is None:
        temporary = tempfile.TemporaryDirectory(prefix="sme-sm0-scan-")
        work = pathlib.Path(temporary.name)
    work.mkdir(parents=True, exist_ok=True)
    try:
        extractor = Extractor(work)
        items = []
        for path in arguments.inputs:
            image = path.name or str(path)
            found = list(extractor.items(image, path.resolve()))
            if not found:
                raise ScanError(f"{path}: no ELF files")
            items.extend(found)
        results: dict[str, dict] = {}
        sources = sorted({source for _i, _d, source in items})
        with concurrent.futures.ProcessPoolExecutor(
                max_workers=max(1, arguments.jobs)) as pool:
            for source, result in zip(sources, pool.map(
                    analyse_file, sources, chunksize=16)):
                results[source] = result
    finally:
        if temporary is not None:
            temporary.cleanup()

    report = Report(items, results, extractor.skipped)
    words = {entry[1] for _i, _d, _r, function in report.functions()
             for entry in function["sm0"]}
    texts = sme_sm0.disassemble(words, llvm_mc)
    status = report.write(arguments.report, texts)
    status |= report.check(expectations)
    return status


class Report:
    def __init__(self, items, results, skipped):
        self.items = items
        self.results = results
        self.skipped = skipped

    def functions(self):
        for image, display, source in self.items:
            result = self.results[source]
            for function in result["functions"]:
                yield image, display, result, function

    def write(self, prefix: pathlib.Path | None, texts: dict[int, str]
              ) -> int:
        machines = collections.Counter()
        unique = {}
        for image, display, source in self.items:
            result = self.results[source]
            machines[(image, result["machine"])] += 1
            unique.setdefault(result["sha256"], result)
        function_rows = []
        word_rows = []
        by_class = collections.Counter()
        by_encoding = collections.Counter()
        sme_binaries = set()
        for image, display, result, function in self.functions():
            sme_binaries.add((image, display))
            counts = collections.Counter(entry[2] for entry in
                                         function["sm0"])
            function_rows.append([
                image, display, function["name"], f"{function['start']:#x}",
                f"{function['end']:#x}", function["bounds"],
                function["kind"], ",".join(function["markers"]),
                function["reached"],
                ",".join(function["calls"]),
                "yes" if function["unresolved_jump"] else "",
                ",".join(f"{k}:{v}" for k, v in sorted(counts.items()))])
            for address, word, cls, encoding, note, states in \
                    function["sm0"]:
                by_class[cls] += 1
                by_encoding[(cls, encoding)] += 1
                word_rows.append([
                    image, display, function["name"], function["kind"],
                    f"{address:#x}", f"{word:08x}", cls, encoding,
                    texts.get(word, ""), note,
                    "".join(str(state) for state in states)])
        if prefix is not None:
            prefix.parent.mkdir(parents=True, exist_ok=True)
            with open(f"{prefix}.functions.tsv", "w") as out:
                out.write("image\tpath\tfunction\tstart\tend\tbounds\t"
                          "entry\tmarkers\treached_markers\tsme_calls\t"
                          "unresolved_jump\t"
                          "sm0_words\n")
                for row in function_rows:
                    out.write("\t".join(row) + "\n")
            with open(f"{prefix}.words.tsv", "w") as out:
                out.write("image\tpath\tfunction\tentry\taddress\tword\t"
                          "class\tencoding\ttext\tnote\tsm_states\n")
                for row in word_rows:
                    out.write("\t".join(row) + "\n")
        images = sorted({image for image, _d, _s in self.items})
        print("SME code at PSTATE.SM = 0 in the scanned images")
        for image in images:
            line = ", ".join(f"{machine}: {count}" for (name, machine), count
                             in sorted(machines.items(),
                                       key=lambda item: str(item[0]))
                             if name == image)
            print(f"  {image}: ELF files {line}")
        for reason, count in sorted(self.skipped.items()):
            print(f"  skipped {count}: {reason}")
        print(f"  distinct ELF files: {len(unique)}; with SME code: "
              f"{len(sme_binaries)} paths, {len(function_rows)} functions")
        for (cls, encoding), count in sorted(by_encoding.items()):
            print(f"  SM=0 {cls:15} {encoding:24} {count:6}")
        data_like = sum(result.get("data_like", 0)
                        for result in unique.values())
        data_files = sum(1 for result in unique.values()
                         if result.get("data_like"))
        print(f"  SME-like words no function executes (data): {data_like} "
              f"in {data_files} files")
        errors = sorted({f"{image}:{display}: {self.results[source]['error']}"
                         for image, display, source in self.items
                         if "error" in self.results[source]})
        for error in errors:
            print(f"  unreadable ELF file {error}")
        decisions = [row for row in word_rows
                     if row[6] in sme_sm0.DECISION_CLASSES]
        review = [row for row in word_rows if row[6] == "unadvertised"]
        print(f"  SM=0 words outside the family that the guest traps: "
              f"{len(decisions)}")
        for row in decisions:
            print("    " + " | ".join(row[:5] + row[7:10]))
        if decisions:
            print("  each is a decision: extend the family or withhold SME "
                  "for that workload")
        print(f"  SM=0 words of features the profile lacks, behind the "
              f"code's own feature checks: {len(review)}")
        for row in review:
            print("    " + " | ".join(row[:5] + row[7:10]))
        return 1 if decisions or errors else 0

    def check(self, expectations: dict[str, frozenset]) -> int:
        status = 0
        for binary, expected in sorted(expectations.items()):
            seen = False
            found = set()
            for image, display, source in self.items:
                if not display.endswith(binary):
                    continue
                seen = True
                for function in self.results[source]["functions"]:
                    found.update(entry[3] for entry in function["sm0"]
                                 if entry[2] == "family" or
                                 entry[2] in sme_sm0.DECISION_CLASSES)
            if not seen:
                print(f"expectation {binary}: no such ELF file was scanned")
                status = 1
            elif found != expected:
                print(f"expectation {binary}: SM=0 SVE words "
                      f"{sorted(found) or 'none'}, expected "
                      f"{sorted(expected)}")
                status = 1
            else:
                print(f"expectation {binary}: SM=0 SVE words "
                      f"{sorted(found)} as expected")
        return status


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except ScanError as error:
        print(f"sme-sm0-scan.py: {error}", file=sys.stderr)
        sys.exit(2)
