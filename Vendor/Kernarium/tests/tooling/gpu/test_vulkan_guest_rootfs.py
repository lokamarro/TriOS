#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from contextlib import contextmanager
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import receipt_fixtures


REPO_ROOT = Path(__file__).resolve().parents[3]
METADATA = REPO_ROOT / "tools/rootfs/vulkan"
VERIFY = METADATA / "verify-inputs.py"
VERIFY_RECEIPT = METADATA / "verify-receipt.py"
RECEIPT = REPO_ROOT / "tests/integration/gpu/vulkan_cts_receipt.json"
SEALER_PATH = METADATA / "seal-in-vm.py"

SEALER_SPEC = importlib.util.spec_from_file_location(
    "vulkan_rootfs_sealer", SEALER_PATH
)
assert SEALER_SPEC is not None and SEALER_SPEC.loader is not None
sealer = importlib.util.module_from_spec(SEALER_SPEC)
sys.modules[SEALER_SPEC.name] = sealer
SEALER_SPEC.loader.exec_module(sealer)

VERIFY_SPEC = importlib.util.spec_from_file_location("vulkan_source_inputs", VERIFY)
assert VERIFY_SPEC is not None and VERIFY_SPEC.loader is not None
source_verifier = importlib.util.module_from_spec(VERIFY_SPEC)
VERIFY_SPEC.loader.exec_module(source_verifier)

IMPORTER_SPEC = importlib.util.spec_from_file_location(
    "vulkan_runtime_importer", METADATA / "install-runtime.py"
)
assert IMPORTER_SPEC is not None and IMPORTER_SPEC.loader is not None
importer = importlib.util.module_from_spec(IMPORTER_SPEC)
IMPORTER_SPEC.loader.exec_module(importer)


@contextmanager
def mesa_install_fixture(build_name: str = "mesa", wrap_mode: str = "nodownload"):
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary).resolve()
        source = root / "sources/mesa"
        build = root / "build" / build_name
        stage = root / "stage/mesa"
        info_root = build / "meson-info"
        source.mkdir(parents=True)
        info_root.mkdir(parents=True)
        protocol = build.parent / "venus-protocol"
        protocol.mkdir()
        info = {"error": False, "directories": {
            "source": str(source), "build": str(build), "info": str(info_root),
        }}
        (info_root / "meson-info.json").write_text(json.dumps(info))
        options = {**sealer.EXPECTED_MESA_BUILD_OPTIONS, "wrap_mode": wrap_mode,
                   "venus-protocol-dir": str(protocol)}
        (info_root / "intro-buildoptions.json").write_text(json.dumps([
            {"name": name, "value": value} for name, value in options.items()
        ]))
        introspection = {"installed": {}, "install_plan": {"targets": {}, "data": {}}}
        runtime = {}
        commands = []
        compile_updates = {}

        def installed_bytes(relative):
            payload = runtime[relative].read_bytes()
            # Stand in for Meson's install-time ELF RPATH transformation.
            return b"installed:" + payload if relative in sealer.MESA_ELF_STAGE_FILES else payload

        for relative in sealer.EXPECTED_MESA_STAGE_FILES:
            category = "data" if relative.endswith(".conf") else "targets"
            origin = (source if category == "data" else build) / relative
            origin.parent.mkdir(parents=True, exist_ok=True)
            origin.write_bytes(relative.encode())
            runtime[relative] = origin
            introspection["installed"][str(origin)] = "/" + relative
            introspection["install_plan"][category][str(origin)] = {"tag": "runtime"}
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(installed_bytes(relative))
        for relative, target in sealer.EXPECTED_MESA_STAGE_SYMLINKS.items():
            (stage / relative).symlink_to(target)

        def run(arguments, **_kwargs):
            command = [str(value) for value in arguments]
            commands.append(command)
            if command[:2] == ["meson", "compile"]:
                assert command == ["meson", "compile", "-C", str(build), "-j", "4"]
                for path, payload in compile_updates.items():
                    path.write_bytes(payload)
            else:
                assert command[:8] == ["meson", "install", "-C", str(build),
                                       "--no-rebuild", "--tags", "runtime", "--destdir"]
                canonical = Path(command[8])
                for relative in runtime:
                    target = canonical / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(installed_bytes(relative))
                for relative, target in sealer.EXPECTED_MESA_STAGE_SYMLINKS.items():
                    (canonical / relative).symlink_to(target)

        def output(arguments):
            command = [str(value) for value in arguments]
            commands.append(command)
            assert command == ["meson", "introspect", "--installed", "--install-plan", str(build)]
            return json.dumps(introspection)

        with patch.object(sealer, "run", side_effect=run), \
                patch.object(sealer, "output", side_effect=output):
            yield source, build, stage, info, introspection, runtime, commands, compile_updates


@contextmanager
def cts_build_fixture(include_opengl_cts: bool = False):
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary).resolve()
        source, build = root / "sources/VK-GL-CTS", root / "build/cts"
        source.mkdir(parents=True)
        build.mkdir(parents=True)
        cache = build / "CMakeCache.txt"
        cache.write_text("\n".join((
            f"CMAKE_HOME_DIRECTORY:INTERNAL={source}",
            f"CMAKE_CACHEFILE_DIR:INTERNAL={build}",
            "DE_CPU:UNINITIALIZED=DE_CPU_ARM_64",
            "DEQP_TARGET:STRING=default",
            "CMAKE_BUILD_TYPE:STRING=Release",
            "SELECTED_BUILD_TARGETS:STRING=" + ("deqp-vk;glcts" if include_opengl_cts else "deqp-vk"),
        )) + "\n")
        release = "vulkan-cts-1.4.6.2-2-g" + "a" * 40
        binaries = [build / "external/vulkancts/modules/vulkan/deqp-vk"]
        if include_opengl_cts:
            binaries.append(build / "external/openglcts/modules/glcts")
        for binary in binaries:
            binary.parent.mkdir(parents=True)
            binary.write_text("other binary string\n" + release + "\n")
        commands = []

        def output(arguments):
            commands.append(arguments)
            assert arguments[:3] == ("strings", "--all", "--")
            return Path(arguments[3]).read_text()

        def git(repository, *arguments):
            commands.append(("git", repository, *arguments))
            assert repository == source
            assert arguments == ("describe", "--tags", "--long", "--abbrev=40",
                                 "--match", "vulkan-cts-*", "HEAD")
            return release

        with patch.object(sealer, "output", side_effect=output), \
                patch.object(sealer, "git", side_effect=git):
            yield source, build, cache, binaries, release, commands


@contextmanager
def protocol_build_fixture():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary).resolve()
        source, sdk, build = root / "source", root / "sdk", root / "build/protocol"
        source.mkdir()
        (sdk / "registry").mkdir(parents=True)
        (sdk / "registry/vk.xml").write_text("pinned registry")
        info_root = build / "meson-info"
        info_root.mkdir(parents=True)
        info = {"error": False, "directories": {
            "source": str(source), "build": str(build), "info": str(info_root),
        }}
        options = {"wrap_mode": "nodownload", "vulkan-headers-dir": str(sdk)}
        (info_root / "meson-info.json").write_text(json.dumps(info))
        (info_root / "intro-buildoptions.json").write_text(json.dumps([
            {"name": name, "value": value} for name, value in options.items()
        ]))
        names = ("vn_protocol_driver.h", "vn_protocol_driver_command_buffer.h")
        for name in names:
            (build / name).write_text(name)
        commands = []

        def run(arguments, **_kwargs):
            command = [str(value) for value in arguments]
            commands.append(command)
            if command[:2] == ["meson", "setup"]:
                assert command[3:] == [str(source), "--wrap-mode=nofallback",
                                       f"-Dvulkan-headers-dir={sdk}"]
                for name in names:
                    (Path(command[2]) / name).write_text(name)
            else:
                assert command[:3] == ["meson", "compile", "-C"]
                assert command[4:] == ["-j", "4"]

        with patch.object(sealer, "run", side_effect=run):
            yield source, sdk, build, info, options, names, commands


class VulkanGuestRootfsTests(unittest.TestCase):
    def test_protocol_build_recomputes_all_headers_from_authenticated_inputs(self) -> None:
        with protocol_build_fixture() as fixture:
            source, sdk, build, _, _, names, commands = fixture
            generation = sealer.verify_protocol_build(source.parent, source, sdk, build)
            self.assertEqual(generation["registry_sha256"], sealer.sha256(sdk / "registry/vk.xml"))
            self.assertEqual(generation["driver_headers"], {
                name: sealer.sha256(build / name) for name in names
            })
            self.assertEqual([command[1] for command in commands], ["compile", "setup", "compile"])

    def test_protocol_build_rejects_foreign_configuration_before_compile(self) -> None:
        for field in ("source", "build", "info", "sdk", "wrap_mode"):
            with self.subTest(field=field), protocol_build_fixture() as fixture:
                source, sdk, build, info, options, _, commands = fixture
                if field in info["directories"]:
                    info["directories"][field] = "/"
                else:
                    options["vulkan-headers-dir" if field == "sdk" else field] = "/"
                (build / "meson-info/meson-info.json").write_text(json.dumps(info))
                (build / "meson-info/intro-buildoptions.json").write_text(json.dumps([
                    {"name": name, "value": value} for name, value in options.items()
                ]))
                with self.assertRaises(sealer.SealError):
                    sealer.verify_protocol_build(source.parent, source, sdk, build)
                self.assertEqual(commands, [])

    def test_protocol_build_rejects_modified_or_extra_generated_headers(self) -> None:
        for change in ("modified", "extra"):
            with self.subTest(change=change), protocol_build_fixture() as fixture:
                source, sdk, build, _, _, names, _ = fixture
                name = names[-1] if change == "modified" else "vn_protocol_driver_extra.h"
                (build / name).write_text("unverified generated bytes")
                with self.assertRaisesRegex(sealer.SealError, "header.* differs"):
                    sealer.verify_protocol_build(source.parent, source, sdk, build)

    def test_protocol_build_rejects_escaping_symlink_before_compile(self) -> None:
        with protocol_build_fixture() as fixture, tempfile.TemporaryDirectory() as outside:
            source, sdk, build, _, _, _, commands = fixture
            escaped = build.parent / "escaped"
            escaped.symlink_to(Path(outside).resolve(), target_is_directory=True)
            with self.assertRaisesRegex(sealer.SealError, "protocol build root is outside"):
                sealer.verify_protocol_build(source.parent, source, sdk, escaped)
            self.assertEqual(commands, [])

    def test_generated_patch_ownership_rejects_imports_and_repeated_binaries(self) -> None:
        def entry(path, destination=None):
            return f"diff --git a/{path} b/{path if destination is None else destination}\n"

        source_verifier.verify_generated_ownership(
            entry("src/kosmickrisp/libkk/libkk.spv") + entry("src/poly/cl/libpoly.spv"), "Mesa")
        source_verifier.verify_generated_ownership(entry("src/old.c", "src/new.c"), "Mesa")
        for path in ("libkk.spv", "test_spirv.hpp", "test_spirv.h"):
            with self.subTest(path=path), self.assertRaisesRegex(
                    source_verifier.VerificationError, "repeats a generated file"):
                source_verifier.verify_generated_ownership(entry(path) * 2, "Mesa")
        for payload in (entry("old.spv", "new.spv") + entry("old.spv"),
                        entry("old.spv", "new.spv") + entry("new.spv")):
            with self.subTest(payload=payload), self.assertRaisesRegex(
                    source_verifier.VerificationError, "repeats a generated file"):
                source_verifier.verify_generated_ownership(payload, "Mesa")
        for path, prefix in (("src/virtio/venus-protocol/vn_protocol_driver.h", "src/virtio/venus-protocol/"),
                             ("xmls/vk.xml", "xmls/vk.xml"),
                             ("include/vulkan/vulkan_core.h", "include/vulkan/")):
            for source, destination in ((path, path), (path, "src/renamed.h"),
                                        ("src/renamed.h", path)):
                with self.subTest(source=source, destination=destination), self.assertRaisesRegex(
                        source_verifier.VerificationError, "build-only generated input"):
                    source_verifier.verify_generated_ownership(
                        entry(source, destination), "protocol", (prefix,))
        for header in ('diff --git "a/xmls/vk.xml" "b/xmls/vk.xml"',
                       'diff --git "a/path with spaces" b/plain',
                       'diff --git a/plain "b/path with spaces"',
                       'diff --git a/path with spaces b/path with spaces',
                       r'diff --git "a/path\\name" "b/path\\name"',
                       "diff --git a/plain b/plain trailing", "diff --cc plain",
                       "diff --combined plain", "diff --git a/plain"):
            with self.subTest(header=header), self.assertRaisesRegex(
                    source_verifier.VerificationError, "unsupported or quoted diff header"):
                source_verifier.verify_generated_ownership(header + "\n", "protocol", ("xmls/vk.xml",))

    def test_cts_build_reads_the_release_without_rebuilding(self) -> None:
        for combined in (False, True):
            with self.subTest(combined=combined), cts_build_fixture(combined) as fixture:
                source, build, _, binaries, release, commands = fixture
                self.assertEqual(sealer.verify_cts_build(
                    source, build, include_opengl_cts=combined,
                ), release)
                self.assertEqual(len(commands), 1 + len(binaries))

    def test_cts_build_rejects_foreign_source_and_build_before_readback(self) -> None:
        for key in ("CMAKE_HOME_DIRECTORY:INTERNAL", "CMAKE_CACHEFILE_DIR:INTERNAL"):
            for replacement in ("relative/path", "/"):
                with self.subTest(key=key, replacement=replacement), cts_build_fixture() as fixture:
                    source, build, cache, _, _, commands = fixture
                    lines = cache.read_text().splitlines()
                    cache.write_text("\n".join(
                        key + "=" + replacement if line.startswith(key + "=") else line
                        for line in lines
                    ))
                    with self.assertRaisesRegex(sealer.SealError, "source/build"):
                        sealer.verify_cts_build(source, build, include_opengl_cts=False)
                    self.assertEqual(commands, [])

    def test_cts_build_requires_exact_unambiguous_cache_options(self) -> None:
        for replacement in (
            "# DEQP_TARGET:STRING=default",
            "DEQP_TARGET:STRING=default-extra",
            "DEQP_TARGET:STRING=default\nDEQP_TARGET:STRING=default",
            "DEQP_TARGET:STRING=other",
        ):
            with self.subTest(replacement=replacement), cts_build_fixture() as fixture:
                source, build, cache, _, _, commands = fixture
                cache.write_text(cache.read_text().replace("DEQP_TARGET:STRING=default", replacement))
                with self.assertRaises(sealer.SealError):
                    sealer.verify_cts_build(source, build, include_opengl_cts=False)
                self.assertEqual(commands, [])

    def test_cts_build_rejects_stale_or_ambiguous_binary_releases(self) -> None:
        with cts_build_fixture(True) as fixture:
            source, build, _, binaries, release, _ = fixture
            for binary in binaries:
                for invalid in ("missing\n", release + "-dirty\n",
                                release.replace("a" * 40, "b" * 40) + "\n",
                                release + "\n" + release.replace("-2-g", "-1-g") + "\n"):
                    with self.subTest(binary=binary.name, invalid=invalid):
                        binary.write_text(invalid)
                        with self.assertRaisesRegex(sealer.SealError, "release"):
                            sealer.verify_cts_build(source, build, include_opengl_cts=True)
                        binary.write_text(release + "\n")

    def test_pinned_sources_and_generated_series_verify(self) -> None:
        result = subprocess.run(
            (str(VERIFY),),
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("verified Vulkan guest source locks", result.stdout)

    def test_hand_edited_series_is_rejected_before_build(self) -> None:
        def unlisted(directory: Path) -> None:
            (directory / "stray.patch").write_bytes(
                next(directory.glob("*.patch")).read_bytes()
            )

        def not_exported(directory: Path) -> None:
            patch_file = next(directory.glob("*.patch"))
            patch_file.write_bytes(
                patch_file.read_bytes().replace(b"From 0000", b"From 1111", 1)
            )

        for name, tamper, message in (
            ("unlisted", unlisted, "patch files differ"),
            ("not-exported", not_exported, "not exported by"),
        ):
            with self.subTest(tamper=name), tempfile.TemporaryDirectory() as temporary:
                copied = Path(temporary) / "vulkan"
                shutil.copytree(METADATA, copied)
                tamper(copied / "patches/mesa")
                result = subprocess.run(
                    (
                        str(VERIFY),
                        "--metadata-root",
                        str(copied),
                        "--outer-root",
                        str(REPO_ROOT),
                    ),
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)

    def test_source_must_have_the_tree_its_series_records(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            repository = root / "work/sources/mesa"
            repository.mkdir(parents=True)

            def git(*arguments: str) -> str:
                return subprocess.run(
                    ("git", "-C", str(repository), *arguments),
                    check=True, text=True, stdout=subprocess.PIPE,
                ).stdout.strip()

            git("init", "--quiet")
            (repository / "file").write_text("base\n")
            git("add", "file")
            git("commit", "--quiet", "-m", "base")
            base = git("rev-parse", "HEAD")
            (repository / "file").write_text("patched\n")
            git("commit", "--quiet", "-am", "patched")
            series = root / "metadata/patches/mesa"
            series.mkdir(parents=True)
            row = {
                "component": "mesa",
                "repository": "https://example.invalid/mesa.git",
                "base_commit": base,
                "base_tree": git("rev-parse", "HEAD~1^{tree}"),
                "release_ref": "-",
                "release_object": "-",
                "series": "patches/mesa/series.tsv",
            }
            for tree, accepted in ((git("rev-parse", "HEAD^{tree}"), True),
                                   (row["base_tree"], False)):
                (series / "series.tsv").write_text(f"tree\t{tree}\n")
                with self.subTest(accepted=accepted):
                    if accepted:
                        record, _ = sealer.verify_source(root / "work", root / "metadata", row)
                        self.assertEqual(record["patched_tree"], tree)
                    else:
                        with self.assertRaisesRegex(sealer.SealError, "final tree changed"):
                            sealer.verify_source(root / "work", root / "metadata", row)

    def test_downstream_scope_is_explicit_and_not_conformance(self) -> None:
        readme = (METADATA / "README.md").read_text(encoding="utf-8")
        self.assertIn("intentionally downstream", readme)
        self.assertIn("KosmicKrisp", readme)
        self.assertIn("ordinary Vulkan ICD", " ".join(readme.split()))
        self.assertIn("Khronos conformance claim", readme)

    def test_protocol_header_check_covers_every_generated_header(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            generated = root / "build"
            canonical = root / "canonical"
            generated.mkdir()
            canonical.mkdir(parents=True)
            names = ("vn_protocol_driver.h", "vn_protocol_driver_device.h",
                     "vn_protocol_driver_info.h", "vn_protocol_driver_command_buffer.h")
            for name in names:
                (generated / name).write_text(name)
                (canonical / name).write_text(name)
            sealer.verify_protocol_headers(generated, canonical)
            (generated / names[-1]).write_text("stale command codec")
            with self.assertRaisesRegex(sealer.SealError, "header differs"):
                sealer.verify_protocol_headers(generated, canonical)
            (generated / names[-1]).unlink()
            with self.assertRaisesRegex(sealer.SealError, "inventory differs"):
                sealer.verify_protocol_headers(generated, canonical)

    def test_bundled_vulkan_must_be_the_pinned_sdk_release(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            mesa = root / "mesa"
            sdk = root / "sdk"
            files = {
                "registry/vk.xml": "registry",
                "include/vulkan/vulkan_core.h": "core",
                "include/vulkan/vulkan.h": "umbrella",
                "include/vk_video/vulkan_video_codec_h264std.h": "video",
            }
            for relative, text in files.items():
                (sdk / relative).parent.mkdir(parents=True, exist_ok=True)
                (sdk / relative).write_text(text)
            # Mesa keeps the registry elsewhere and carries one header of its own.
            for relative, text in files.items():
                target = relative.replace("registry/vk.xml", "src/vulkan/registry/vk.xml")
                (mesa / target).parent.mkdir(parents=True, exist_ok=True)
                (mesa / target).write_text(text)
            (mesa / "include/vulkan/vk_android_native_buffer.h").write_text("mesa only")
            (sdk / "include/vulkan/vulkan_core.hpp").write_text("not a C header")
            sealer.verify_bundled_vulkan(mesa, sdk)
            (mesa / "include/vulkan/vulkan_core.h").write_text("older release")
            with self.assertRaisesRegex(sealer.SealError, "different Vulkan release"):
                sealer.verify_bundled_vulkan(mesa, sdk)
            (mesa / "include/vulkan/vulkan_core.h").write_text("core")
            (mesa / "src/vulkan/registry/vk.xml").write_text("older registry")
            with self.assertRaisesRegex(sealer.SealError, "different Vulkan release"):
                sealer.verify_bundled_vulkan(mesa, sdk)
            (mesa / "src/vulkan/registry/vk.xml").write_text("registry")
            (mesa / "include/vk_video/vulkan_video_codec_h264std.h").unlink()
            with self.assertRaisesRegex(sealer.SealError, "does not bundle"):
                sealer.verify_bundled_vulkan(mesa, sdk)

    def test_protocol_header_check_rejects_symlinks_and_empty_sets(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            generated = root / "build"
            canonical = root / "canonical"
            generated.mkdir()
            canonical.mkdir(parents=True)
            with self.assertRaisesRegex(sealer.SealError, "inventory differs"):
                sealer.verify_protocol_headers(generated, canonical)
            name = "vn_protocol_driver.h"
            (generated / name).write_text("codec")
            (canonical / name).symlink_to(generated / name)
            with self.assertRaisesRegex(sealer.SealError, "header differs"):
                sealer.verify_protocol_headers(generated, canonical)

    def test_mesa_binding_accepts_canonical_install_and_build_override(self) -> None:
        for name, wrap_mode in (("mesa", "nodownload"), ("mesa-custom", "nofallback")):
            with self.subTest(build=name), mesa_install_fixture(name, wrap_mode) as fixture:
                source, build, stage, _, _, runtime, commands, _ = fixture
                argv = sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")
                self.assertEqual(argv[:5], ["meson", "setup", str(build), str(source),
                                            f"--wrap-mode={wrap_mode}"])
                self.assertEqual([command[1] for command in commands],
                                 ["compile", "introspect", "install"])
                library = "usr/lib/aarch64-linux-gnu/libvulkan_virtio.so"
                self.assertNotEqual(runtime[library].read_bytes(), (stage / library).read_bytes())

    def test_mesa_binding_rejects_wrong_source_build_or_info_directory(self) -> None:
        for name in ("source", "build", "info"):
            with self.subTest(directory=name), mesa_install_fixture() as fixture:
                source, build, stage, info, _, _, commands, _ = fixture
                other = build.parent / "other"
                other.mkdir()
                info["directories"][name] = str(other)
                (build / "meson-info/meson-info.json").write_text(json.dumps(info))
                with self.assertRaisesRegex(sealer.SealError, f"Mesa {name} directory differs"):
                    sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")
                self.assertEqual(commands, [])

    def test_mesa_binding_rejects_online_build_before_compile(self) -> None:
        with mesa_install_fixture() as fixture:
            source, build, stage, _, _, _, commands, _ = fixture
            options = {**sealer.EXPECTED_MESA_BUILD_OPTIONS, "wrap_mode": "default",
                       "venus-protocol-dir": str(build.parent / "venus-protocol")}
            (build / "meson-info/intro-buildoptions.json").write_text(json.dumps([
                {"name": name, "value": value} for name, value in options.items()
            ]))
            with self.assertRaisesRegex(sealer.SealError, "offline wrap mode"):
                sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")
            self.assertEqual(commands, [])

    def test_mesa_binding_rejects_stale_runtime_stage(self) -> None:
        with mesa_install_fixture() as fixture:
            source, build, stage, _, _, _, _, _ = fixture
            (stage / "usr/lib/aarch64-linux-gnu/libvulkan_virtio.so").write_bytes(b"old driver")
            with self.assertRaisesRegex(sealer.SealError, "staged runtime differs"):
                sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")

    def test_mesa_binding_rejects_missing_or_foreign_protocol_before_compile(self) -> None:
        for value in (None, "relative", "/"):
            with self.subTest(value=value), mesa_install_fixture() as fixture:
                source, build, stage, _, _, _, commands, _ = fixture
                path = build / "meson-info/intro-buildoptions.json"
                options = json.loads(path.read_text())
                for option in options:
                    if option["name"] == "venus-protocol-dir":
                        option["value"] = value
                path.write_text(json.dumps(options))
                with self.assertRaisesRegex(sealer.SealError, "Mesa protocol directory"):
                    sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")
                self.assertEqual(commands, [])

    def test_mesa_binding_retains_exact_stage_inventory(self) -> None:
        for change in ("extra-file", "wrong-symlink"):
            with self.subTest(change=change), mesa_install_fixture() as fixture:
                source, build, stage, _, _, _, _, _ = fixture
                if change == "extra-file":
                    (stage / "unrequested").write_bytes(b"extra")
                else:
                    link = stage / "usr/lib/aarch64-linux-gnu/libgbm.so.1"
                    link.unlink()
                    link.symlink_to("unrelated.so")
                with self.assertRaisesRegex(sealer.SealError, "install .* inventory changed"):
                    sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")

    def test_mesa_binding_synchronizes_same_path_topic_updates(self) -> None:
        with mesa_install_fixture() as fixture:
            source, build, stage, _, _, runtime, commands, updates = fixture
            library = "usr/lib/aarch64-linux-gnu/libvulkan_virtio.so"
            updates[runtime[library]] = b"newly compiled topic"
            with self.assertRaisesRegex(sealer.SealError, "staged runtime differs"):
                sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")
            self.assertEqual(runtime[library].read_bytes(), b"newly compiled topic")
            self.assertEqual([command[1] for command in commands],
                             ["compile", "introspect", "install"])

    def test_mesa_binding_rejects_incomplete_runtime_mapping(self) -> None:
        with mesa_install_fixture() as fixture:
            source, build, stage, _, introspection, runtime, _, _ = fixture
            library = str(runtime["usr/lib/aarch64-linux-gnu/libvulkan_virtio.so"])
            del introspection["install_plan"]["targets"][library]
            with self.assertRaisesRegex(sealer.SealError, "runtime install mapping changed"):
                sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")

    def test_mesa_binding_rejects_foreign_runtime_output(self) -> None:
        with mesa_install_fixture() as fixture:
            source, build, stage, _, introspection, runtime, _, _ = fixture
            library = str(runtime["usr/lib/aarch64-linux-gnu/libvulkan_virtio.so"])
            foreign = build.parent / "foreign.so"
            foreign.write_bytes(b"foreign")
            introspection["installed"][str(foreign)] = introspection["installed"].pop(library)
            introspection["install_plan"]["targets"][str(foreign)] = \
                introspection["install_plan"]["targets"].pop(library)
            with self.assertRaisesRegex(sealer.SealError, "outside its authenticated source/build"):
                sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")

    def test_gl_sealer_derives_only_xml_referenced_scope_and_checks_runner(self) -> None:
        import test_run_vk_gl_cts as fixtures

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage, source = root / "stage", root / "source"
            files = fixtures.opengl_mustpass_files()
            for name, payload in files.items():
                path = stage / name.lstrip("/")
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(payload)
            runner = sealer.opengl_cts_plan.RUNNER_SOURCE
            runner_path = source / runner["path"]
            runner_path.parent.mkdir(parents=True)
            runner_path.write_bytes(b"x" * runner["size"])
            with self.assertRaisesRegex(sealer.SealError, "runner scope source"):
                sealer.opengl_manifest(stage, source)
            with patch.object(sealer, "sha256", return_value=runner["sha256"]):
                manifest = sealer.opengl_manifest(stage, source)
                self.assertEqual(manifest["leaf_count"], 8)
                self.assertEqual(len(manifest["configurations"]), 13)
                self.assertFalse(any("gtf" in leaf["path"] for leaf in manifest["leaves"]))
                missing = stage / manifest["leaves"][0]["path"].lstrip("/")
                missing.unlink()
                with self.assertRaises(FileNotFoundError):
                    sealer.opengl_manifest(stage, source)

    def test_gl_runtime_copy_excludes_collocated_build_products(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            generated = root / "generated"
            source = root / "source"
            overlay = root / "overlay"
            image_root = root / "image"
            for directory in (generated, source, overlay, image_root):
                directory.mkdir()
            (source / "shader.test").write_text("base\n", encoding="utf-8")
            (overlay / "shader.test").write_text("overlay\n", encoding="utf-8")
            (overlay / "extra.test").write_text("extra\n", encoding="utf-8")
            (generated / "shader.test").write_text("overlay\n", encoding="utf-8")
            (generated / "extra.test").write_text("extra\n", encoding="utf-8")
            (generated / "libglcts-es3.a").write_text("build\n", encoding="utf-8")

            sealer.copy_generated_runtime_directory(
                generated,
                (source, overlay),
                image_root,
                "/opt/vk-gl-cts/openglcts/gles3",
            )

            installed = image_root / "opt/vk-gl-cts/openglcts/gles3"
            self.assertEqual(
                sorted(path.name for path in installed.iterdir()),
                ["extra.test", "shader.test"],
            )
            self.assertEqual(
                (installed / "shader.test").read_text(encoding="utf-8"),
                "overlay\n",
            )

    def test_accepted_cts_images_match_the_current_guest_sources(self) -> None:
        """The one tooling check that the accepted CTS images are current.

        Every other receipt-bound tooling test uses a copy of the accepted
        receipts rebound to the current source epoch, so a guest series that
        produces a new tree fails here alone. The runners refuse the stale
        images either way, until new ones are sealed and their receipts
        checked in.
        """
        validator = receipt_fixtures.receipt_contract
        for path in (
            receipt_fixtures.VULKAN_RECEIPT,
            receipt_fixtures.ZINK_GL_RECEIPT,
        ):
            with self.subTest(receipt=path.name):
                receipt = validator.load_json(path)
                difference = None
                try:
                    validator.verify_source_epoch(receipt)
                except validator.ReceiptError as error:
                    difference = str(error)
                if difference is not None:
                    self.fail(
                        f"accepted CTS image is stale; reseal it: {path.name} "
                        "was sealed from other guest sources than "
                        f"tools/rootfs/vulkan records ({difference}). Seal a "
                        "new image and check in its receipt as "
                        "tools/rootfs/vulkan/README.md describes."
                    )

    def test_checked_receipt_and_native_arm64_cts_verify(self) -> None:
        # The accepted receipt, apart from the source epoch checked above.
        current = receipt_fixtures.current_epoch_receipt_file(self, RECEIPT)
        result = subprocess.run(
            (str(VERIFY_RECEIPT), "--receipt", str(current)),
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        receipt = json.loads(RECEIPT.read_text(encoding="utf-8"))
        self.assertEqual(receipt["schema_version"], 2)
        self.assertFalse(receipt["claim_scope"]["formal_conformance"])
        self.assertTrue(receipt["claim_scope"]["downstream_guest_mesa"])
        self.assertEqual(receipt["cts"]["reported_cpu"], "DE_CPU_ARM_64")
        self.assertEqual(receipt["mustpass"]["leaf_count"], 98)
        self.assertEqual(receipt["mustpass"]["case_count"], 3_247_552)

    def test_checked_receipt_rejects_claim_expansion(self) -> None:
        receipt = receipt_fixtures.current_epoch_receipt(RECEIPT)
        receipt["claim_scope"]["formal_conformance"] = True
        with tempfile.TemporaryDirectory() as temporary:
            changed = Path(temporary) / "receipt.json"
            changed.write_text(json.dumps(receipt), encoding="utf-8")
            result = subprocess.run(
                (str(VERIFY_RECEIPT), "--receipt", str(changed)),
                cwd=REPO_ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("claim scope changed", result.stderr)


class GuestRuntimeArchitectureTests(unittest.TestCase):
    """The multiarch directory is the only architecture-dependent part."""

    def test_default_profile_is_the_sealed_arm64_contract(self) -> None:
        profile = sealer.architecture_profile("arm64")
        self.assertEqual(sealer.DEFAULT_ARCHITECTURE, "arm64")
        self.assertEqual(profile["mesa_stage_files"], sealer.EXPECTED_MESA_STAGE_FILES)
        self.assertEqual(
            profile["mesa_stage_symlinks"], sealer.EXPECTED_MESA_STAGE_SYMLINKS
        )
        self.assertEqual(profile["mesa_guest_symlinks"], sealer.MESA_GUEST_SYMLINKS)
        self.assertEqual(profile["mesa_elf_stage_files"], sealer.MESA_ELF_STAGE_FILES)
        self.assertEqual(profile["mesa_options"], sealer.MESA_OPTIONS)
        self.assertEqual(profile["loader_options"], sealer.VULKAN_LOADER_OPTIONS)
        self.assertEqual(profile["loader_stage"], sealer.EXPECTED_LOADER_STAGE)
        self.assertEqual(profile["loader_runtime"], sealer.LOADER_RUNTIME_GUEST_PATH)
        self.assertEqual(profile["elf_machine"], "AArch64")
        self.assertIn("--libdir=lib/aarch64-linux-gnu", profile["mesa_options"])

    def test_second_architecture_renames_only_the_library_directory(self) -> None:
        arm64 = sealer.architecture_profile("arm64")
        amd64 = sealer.architecture_profile("amd64")
        self.assertNotEqual(arm64["elf_machine"], amd64["elf_machine"])

        def renamed(value):
            if isinstance(value, str):
                return value.replace("aarch64-linux-gnu", "x86_64-linux-gnu")
            if isinstance(value, dict):
                return {renamed(key): renamed(item) for key, item in value.items()}
            if isinstance(value, (list, tuple, set, frozenset)):
                return type(value)(renamed(item) for item in value)
            return value

        for name in (
            "mesa_stage_files",
            "mesa_stage_symlinks",
            "mesa_guest_symlinks",
            "mesa_elf_stage_files",
            "mesa_build_options",
            "mesa_options",
            "loader_stage",
            "loader_runtime",
            "loader_symlinks",
            "loader_options",
            "closure_consumers",
        ):
            self.assertEqual(renamed(arm64[name]), amd64[name], name)

    def test_every_staged_driver_is_in_the_audited_closure(self) -> None:
        profile = sealer.architecture_profile("amd64")
        consumers = set(profile["closure_consumers"])
        self.assertEqual(
            {f"/{relative}" for relative in profile["mesa_elf_stage_files"]}
            | {profile["loader_runtime"]},
            consumers,
        )


class GuestRuntimeImporterTests(unittest.TestCase):
    """The importer authenticates a payload before it changes an image."""

    @staticmethod
    def fixture(root: Path) -> tuple[Path, Path, Path]:
        source, target = root / "source", root / "root"
        library = source / "usr/lib/x86_64-linux-gnu/libvulkan_virtio.so"
        library.parent.mkdir(parents=True)
        library.write_bytes(b"driver")
        target.mkdir()
        receipt = root / "receipt.json"
        receipt.write_text(
            json.dumps(
                {
                    "schema_version": importer.SCHEMA_VERSION,
                    "artifact": importer.ARTIFACT,
                    "architecture": {"debian": "amd64"},
                    "mesa": {"portability_driver": False, "files": []},
                    "vulkan_loader": {
                        "runtime": {
                            "path": "/usr/lib/x86_64-linux-gnu/libvulkan_virtio.so",
                            "size": 6,
                            "sha256": "0" * 64,
                        }
                    },
                    "symlinks": {},
                    "selected_icd_manifest": "/usr/share/vulkan/icd.d/virtio_icd.json",
                }
            ),
            encoding="utf-8",
        )
        return source, receipt, target

    def test_rejects_a_payload_that_does_not_match_its_receipt(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source, receipt, target = self.fixture(Path(temporary))
            with self.assertRaisesRegex(importer.RuntimeImportError, "does not match"):
                importer.install(source, receipt, target, "amd64")
            self.assertEqual(list(target.rglob("*")), [])

    def test_rejects_a_foreign_receipt(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source, receipt, target = self.fixture(Path(temporary))
            value = json.loads(receipt.read_text(encoding="utf-8"))
            value["artifact"] = "uml-vulkan-rootfs"
            receipt.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(importer.RuntimeImportError, "not a guest Vulkan"):
                importer.install(source, receipt, target, "amd64")

    def test_rejects_another_architecture_and_a_portability_driver(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source, receipt, target = self.fixture(Path(temporary))
            with self.assertRaisesRegex(importer.RuntimeImportError, "describes amd64"):
                importer.install(source, receipt, target, "arm64")
            value = json.loads(receipt.read_text(encoding="utf-8"))
            value["mesa"]["portability_driver"] = True
            receipt.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(importer.RuntimeImportError, "portability driver"):
                importer.install(source, receipt, target, "amd64")

    def test_rejects_an_unnormalized_guest_path(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source, receipt, target = self.fixture(Path(temporary))
            value = json.loads(receipt.read_text(encoding="utf-8"))
            value["vulkan_loader"]["runtime"]["path"] = "/usr/../etc/passwd"
            receipt.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(importer.RuntimeImportError, "not a normalized"):
                importer.install(source, receipt, target, "amd64")


if __name__ == "__main__":
    unittest.main()
