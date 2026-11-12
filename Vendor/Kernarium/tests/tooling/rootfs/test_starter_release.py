#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the starter release manifest, input pins and producer options."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
STARTER = HERE.parents[2] / "tools/rootfs/starter"
sys.path.insert(0, str(STARTER))


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


GPT = load("gpt", STARTER / "gpt.py")
FIXTURES = load("test_starter_images", HERE / "test_starter_images.py")
PINS = load("debootstrap_pins", STARTER / "debootstrap-pins.py")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run(*arguments: str, check: bool = True, **keywords):
    return subprocess.run(arguments, check=check, text=True,
                          capture_output=True, **keywords)


class ManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory(prefix="uml-starter-rel-")
        self.addCleanup(self.directory.cleanup)
        self.output = Path(self.directory.name, "release")
        self.output.mkdir()

    def image(self, name: str) -> None:
        FIXTURES.write_gpt_ext4_fixture(self.output / name, GPT)
        (self.output / f"{name}.packages.tsv").write_text("pkg\t1\n")

    def manifest(self, *arguments: str, check: bool = True):
        return run(sys.executable, "-B", str(STARTER / "write-manifest.py"),
                   "--output", str(self.output), *arguments, check=check)

    def load_manifest(self) -> dict:
        return json.loads((self.output / "release-manifest.json").read_text())

    def test_the_default_release_records_its_five_products(self) -> None:
        for name in ("uml-boot-base-aarch64.cpio.gz",
                     "uml-boot-base-x86_64.cpio.gz"):
            (self.output / name).write_bytes(name.encode())
        for name in ("alpine-3.24.2-minimal.img", "debian-13-minimal.img",
                     "debian-13-plasma-wayland.img"):
            self.image(name)
        products = ["boot-base-aarch64", "boot-base-x86_64", "alpine",
                    "debian-13-minimal", "debian-13-plasma-wayland"]
        self.manifest(
            *(a for p in products for a in ("--product", p)),
            "--buildroot-version", "1", "--buildroot-sha256", "a",
            "--busybox-version", "2", "--busybox-sha256", "b",
            "--alpine-version", "3.24.2", "--alpine-sha256", "c",
            "--debian-suite", "trixie", "--debian-version", "13.7")
        manifest = self.load_manifest()
        self.assertEqual(sorted(manifest["artifacts"]), [
            "alpine-3.24.2-minimal.img", "debian-13-minimal.img",
            "debian-13-plasma-wayland.img", "uml-boot-base-aarch64.cpio.gz",
            "uml-boot-base-x86_64.cpio.gz"])
        self.assertEqual(sorted(manifest["sources"]),
                         ["alpine", "buildroot", "busybox", "debian"])
        self.assertEqual(manifest["sources"]["debian"],
                         {"suite": "trixie", "version": "13.7"})
        entry = manifest["artifacts"]["debian-13-minimal.img"]
        self.assertEqual(sorted(entry), [
            "allocated_size", "disk", "package_inventory", "sha256", "size"])
        self.assertEqual(sorted(entry["package_inventory"]),
                         ["path", "sha256", "size"])

    def armhf_release(self, audit: list[str]) -> list[str]:
        self.image("debian-13-minimal-armhf.img")
        (self.output / "debian-13-minimal-armhf.img.elf.tsv").write_text(
            "".join(line + "\n" for line in audit))
        pins = Path(self.directory.name, "pins.json")
        pins.write_text(json.dumps({
            "suite": "trixie", "component": "main", "architecture": "armhf",
            "release": {"date": "Sat, 12 Sep 2026 07:55:41 UTC"},
            "downloads": {"packages": 97, "total_bytes": 123},
        }))
        return [
            "--product", "debian-13-minimal-armhf",
            "--debian-suite", "trixie", "--debian-version", "13.7",
            "--debian-armhf-pins", str(pins), "--armhf-include", "a,b",
            "--debian-mirror", "http://mirror", "--debootstrap-version",
            "debootstrap 1", "--emulator-version", "qemu-arm 10",
        ]

    def test_an_armhf_only_release_records_its_pins_and_audit(self) -> None:
        arguments = self.armhf_release([
            "usr/bin/a\t32\tarm\tdyn\teabi5-hard\t-\t0x10000\t1\t4k\tok",
            "usr/bin/b\t32\tarm\tdyn\teabi5-hard\t-\t0x1000\t2\t4k\tok",
        ])
        self.manifest(*arguments)
        manifest = self.load_manifest()
        self.assertEqual(sorted(manifest["sources"]),
                         ["debian", "debian_armhf"])
        source = manifest["sources"]["debian_armhf"]
        self.assertEqual(source["include"], ["a", "b"])
        self.assertEqual(source["downloads"], {"packages": 97,
                                               "total_bytes": 123})
        entry, = manifest["artifacts"].values()
        self.assertEqual(entry["architecture"], "armhf")
        audit = entry["elf_audit"]
        self.assertEqual(sorted(audit), ["files", "path", "sha256", "size"])
        self.assertEqual(audit["path"], "debian-13-minimal-armhf.img.elf.tsv")
        self.assertEqual(audit["files"], 2)
        inventory = entry["package_inventory"]
        self.assertEqual(inventory["columns"], [
            "package", "version", "architecture", "size", "sha256"])
        self.assertEqual(inventory["packages"], 97)

    def test_a_failed_audit_row_stops_the_manifest(self) -> None:
        arguments = self.armhf_release([
            "usr/bin/a\t32\tarm\tdyn\teabi5-hard\t-\t0x1000\t1\t4k\t"
            "interpreter-missing:/lib/ld.so"])
        result = self.manifest(*arguments, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("failed the ELF audit", result.stderr)

    def test_a_product_needs_its_own_pins(self) -> None:
        self.image("debian-13-minimal-armhf.img")
        result = self.manifest("--product", "debian-13-minimal-armhf",
                               "--debian-suite", "trixie",
                               "--debian-version", "13.7", check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("--debian-armhf-pins is required", result.stderr)


class PinTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory(prefix="uml-pins-")
        self.addCleanup(self.directory.cleanup)
        base = Path(self.directory.name)
        self.root = base / "root"
        self.cache = base / "cache"
        lists = self.root / "var/lib/apt/lists"
        lists.mkdir(parents=True)
        (self.root / "var/lib/dpkg").mkdir(parents=True)
        self.cache.mkdir()
        self.debs = {
            "libfoo1_1%3a1.0-1_armhf.deb": b"foo deb bytes",
            "tool_2.0_all.deb": b"tool deb bytes",
        }
        for name, data in self.debs.items():
            (self.cache / name).write_bytes(data)
        status = (
            "Package: libfoo1\nStatus: install ok installed\n"
            "Architecture: armhf\nMulti-Arch: same\nVersion: 1:1.0-1\n\n"
            "Package: tool\nStatus: install ok installed\n"
            "Architecture: all\nVersion: 2.0\n\n")
        (self.root / "var/lib/dpkg/status").write_text(status)
        self.index = (
            "Package: libfoo1\nVersion: 1:1.0-1\nArchitecture: armhf\n"
            f"Size: {len(self.debs['libfoo1_1%3a1.0-1_armhf.deb'])}\n"
            "SHA256: "
            f"{sha256(self.debs['libfoo1_1%3a1.0-1_armhf.deb'])}\n\n"
            "Package: tool\nVersion: 2.0\nArchitecture: all\n"
            f"Size: {len(self.debs['tool_2.0_all.deb'])}\n"
            f"SHA256: {sha256(self.debs['tool_2.0_all.deb'])}\n\n")
        self.write_archive(self.index)

    def write_archive(self, index: str, unpacked_lie: str | None = None):
        lists = self.root / "var/lib/apt/lists"
        (lists / "m_dists_trixie_main_binary-armhf_Packages").write_text(index)
        unpacked = unpacked_lie if unpacked_lie is not None else index
        packed_sha, packed_size = sha256(b"packed"), 6
        release = (
            "Origin: Debian\nVersion: 13.7\nCodename: trixie\n"
            "Date: Sat, 12 Sep 2026 07:55:41 UTC\nSHA256:\n"
            f" {sha256(unpacked.encode())} {len(unpacked.encode())} "
            "main/binary-armhf/Packages\n"
            f" {packed_sha} {packed_size} main/binary-armhf/Packages.xz\n")
        (lists / "m_dists_trixie_Release").write_text(release)
        escaped = "\n".join(
            "- " + line if line.startswith("-") else line
            for line in release.rstrip("\n").splitlines())
        (lists / "m_dists_trixie_InRelease").write_text(
            "-----BEGIN PGP SIGNED MESSAGE-----\nHash: SHA512\n\n"
            + escaped + "\n-----BEGIN PGP SIGNATURE-----\n\nxx\n"
            "-----END PGP SIGNATURE-----\n")

    def pin(self, check: bool = True):
        base = Path(self.directory.name)
        return run(sys.executable, "-B", str(STARTER / "debootstrap-pins.py"),
                   "--root", str(self.root), "--cache", str(self.cache),
                   "--architecture", "armhf", "--suite", "trixie",
                   "--inventory", str(base / "inventory.tsv"),
                   "--pins", str(base / "pins.json"), check=check)

    def test_the_inventory_and_pins_follow_the_verified_inputs(self) -> None:
        self.pin()
        base = Path(self.directory.name)
        rows = [line.split("\t") for line in
                (base / "inventory.tsv").read_text().splitlines()]
        foo = self.debs["libfoo1_1%3a1.0-1_armhf.deb"]
        tool = self.debs["tool_2.0_all.deb"]
        self.assertEqual(rows, [
            ["libfoo1:armhf", "1:1.0-1", "armhf", str(len(foo)),
             sha256(foo)],
            ["tool", "2.0", "all", str(len(tool)), sha256(tool)],
        ])
        pins = json.loads((base / "pins.json").read_text())
        self.assertEqual(pins["release"]["date"],
                         "Sat, 12 Sep 2026 07:55:41 UTC")
        self.assertEqual(pins["release"]["package_index"]["path"],
                         "main/binary-armhf/Packages.xz")
        self.assertEqual(pins["downloads"]["packages"], 2)
        self.assertEqual(pins["downloads"]["package_bytes"],
                         len(foo) + len(tool))
        self.assertEqual(
            pins["downloads"]["total_bytes"],
            len(foo) + len(tool) + 6
            + (self.root / "var/lib/apt/lists/m_dists_trixie_InRelease"
               ).stat().st_size)

    def test_an_unverified_package_stops_the_pins(self) -> None:
        (self.cache / "tool_2.0_all.deb").write_bytes(b"swapped")
        result = self.pin(check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match the verified index", result.stderr)

    def test_a_stray_or_missing_download_stops_the_pins(self) -> None:
        (self.cache / "stray_1_armhf.deb").write_bytes(b"x")
        self.assertIn("downloaded but not installed",
                      self.pin(check=False).stderr)
        (self.cache / "stray_1_armhf.deb").unlink()
        (self.cache / "tool_2.0_all.deb").unlink()
        self.assertIn("tool_2.0_all.deb was not downloaded",
                      self.pin(check=False).stderr)

    def test_an_index_the_release_does_not_name_stops_the_pins(self) -> None:
        self.write_archive(self.index, unpacked_lie=self.index + "\n# other\n")
        self.assertIn("package index does not match the release",
                      self.pin(check=False).stderr)

    def test_a_half_installed_package_stops_the_pins(self) -> None:
        status = self.root / "var/lib/dpkg/status"
        status.write_text(status.read_text().replace(
            "install ok installed", "install ok half-configured", 1))
        self.assertIn("not installed", self.pin(check=False).stderr)


class ProducerOptionTests(unittest.TestCase):
    def producer(self, *arguments: str):
        return run("sh", str(STARTER / "build-all.sh"), *arguments,
                   check=False)

    def test_help_names_the_products(self) -> None:
        result = self.producer("--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("debian-13-minimal-armhf", result.stdout)

    def test_bad_product_options_fail_before_any_work(self) -> None:
        arguments = ("--work", "/nonexistent/work", "--output",
                     "/nonexistent/out")
        for options, message in (
            (("--product", "debian-13"), "unknown product: debian-13"),
            (("--product", "alpine", "--product", "alpine"),
             "product named twice: alpine"),
            (("--product",), "--product requires a name"),
        ):
            result = self.producer(*arguments, *options)
            self.assertEqual(result.returncode, 1, options)
            self.assertIn(message, result.stderr)

    def test_the_producer_shell_scripts_parse(self) -> None:
        for name in ("build-all.sh", "armhf-run.sh", "armhf-verify.sh"):
            with self.subTest(script=name):
                run("sh", "-n", str(STARTER / name))

    def test_the_armhf_runner_refuses_a_missing_command_and_a_user(self) -> None:
        runner = str(STARTER / "armhf-run.sh")
        result = run("sh", runner, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("Usage:", result.stderr)
        result = run("sh", runner, "--help", check=False)
        self.assertEqual(result.returncode, 0)
        self.assertIn("private binfmt_misc", result.stdout)
        if os.geteuid() != 0:
            result = run("sh", runner, "/nonexistent", "/bin/true",
                         check=False)
            self.assertEqual(result.returncode, 1)
            self.assertIn("run as root", result.stderr)


if __name__ == "__main__":
    unittest.main()
