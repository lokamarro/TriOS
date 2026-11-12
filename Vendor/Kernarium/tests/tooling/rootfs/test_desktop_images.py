#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The desktop image descriptor binds each accepted image to its inputs."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import importlib.util
import inspect
import json
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

DESKTOP = pathlib.Path(__file__).resolve().parents[3] / "tools/rootfs/desktop"


def load_derive():
    specification = importlib.util.spec_from_file_location(
        "desktop_derive", DESKTOP / "derive.py"
    )
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


class DesktopImageTests(unittest.TestCase):
    def test_accepted_images_name_the_setup_that_produced_them(self) -> None:
        descriptor = json.loads(
            (DESKTOP / "images.json").read_text(encoding="utf-8")
        )
        self.assertEqual(descriptor["schema_version"], 1)
        self.assertEqual(
            set(descriptor["images"]), {"kde", "phrog", "postmarketos"}
        )
        for name, entry in descriptor["images"].items():
            with self.subTest(image=name):
                setup = DESKTOP / entry["setup"]
                self.assertEqual(
                    hashlib.sha256(setup.read_bytes()).hexdigest(),
                    entry["accepted"]["setup_sha256"],
                    "a setup change needs a new derived release",
                )
                self.assertTrue(setup.stat().st_mode & 0o111)
                subprocess.run(["sh", "-n", str(setup)], check=True)
                self.assertRegex(entry["source"]["sha256"], r"^[0-9a-f]{64}$")
                self.assertRegex(
                    entry["accepted"]["sha256"], r"^[0-9a-f]{64}$"
                )
                # A kernel command line names the image, so the name is plain.
                self.assertRegex(
                    entry["accepted"]["file"], r"^[A-Za-z0-9._-]+\.erofs$"
                )
                self.assertRegex(
                    entry["accepted"]["release"], r"^desktop-\d{8}-r\d+$"
                )
                self.assertIsNotNone(
                    re.fullmatch(r"(zstd,level=\d+|lz4hc)", entry["compression"])
                )

    def test_kernarium_defaults_to_the_accepted_kde_image(self) -> None:
        descriptor = json.loads(
            (DESKTOP / "images.json").read_text(encoding="utf-8")
        )
        accepted = descriptor["images"]["kde"]["accepted"]["file"]
        kernarium = DESKTOP.parents[2] / "Kernarium"
        source = (kernarium / "Kernarium/EmulatorConfiguration.swift").read_text(
            encoding="utf-8"
        )
        self.assertEqual(
            re.findall(r'static let defaultFilename = "([^"]*)"', source),
            [accepted],
        )
        tests = (kernarium / "KernariumTests/KernariumTests.swift").read_text(
            encoding="utf-8"
        )
        self.assertEqual(
            set(re.findall(r'rootfsFilename == "(kde-[^"]*)"', tests)),
            {accepted},
        )
        self.assertIn(
            f"`{accepted}`",
            (kernarium / "README.md").read_text(encoding="utf-8"),
        )

    def test_derivation_refuses_a_source_that_does_not_match(self) -> None:
        derive = load_derive()
        with tempfile.TemporaryDirectory() as name:
            directory = pathlib.Path(name)
            source = directory / "rootfs.erofs"
            source.write_bytes(b"not the pinned image")
            with self.assertRaisesRegex(derive.DerivationError, "size"):
                derive.derive(
                    "kde", source, directory / "out.erofs", directory
                )
            with self.assertRaisesRegex(derive.DerivationError, "no image"):
                derive.derive(
                    "gnome", source, directory / "out.erofs", directory
                )
            self.assertEqual(sorted(directory.iterdir()), [source])

    @staticmethod
    def make_root(directory: pathlib.Path, *, systemd: bool) -> pathlib.Path:
        """A source tree carrying the machine ID of the build that made it."""
        root = directory / ("systemd" if systemd else "openrc")
        (root / "etc").mkdir(parents=True)
        if systemd:
            (root / "usr/lib/systemd").mkdir(parents=True)
            (root / "usr/lib/systemd/systemd").write_bytes(b"")
            (root / "lib").symlink_to("usr/lib")
        else:
            (root / "lib").mkdir()
        machine_id = root / "etc/machine-id"
        machine_id.write_text("0982c3c086124c19948eea126de9f9f6\n")
        machine_id.chmod(0o444)
        return root

    def test_derived_images_carry_no_machine_id(self) -> None:
        derive = load_derive()
        with tempfile.TemporaryDirectory() as name:
            directory = pathlib.Path(name)

            # systemd fills an empty file; D-Bus's copy follows it.
            root = self.make_root(directory, systemd=True)
            (root / "var/lib/dbus").mkdir(parents=True)
            (root / "var/lib/dbus/machine-id").write_text(
                "0982c3c086124c19948eea126de9f9f6\n"
            )
            derive.reset_machine_id(root)
            machine_id = root / "etc/machine-id"
            self.assertFalse(machine_id.is_symlink())
            self.assertTrue(machine_id.is_file())
            self.assertEqual(machine_id.read_bytes(), b"")
            self.assertEqual(machine_id.stat().st_mode & 0o7777, 0o444)
            copy = root / "var/lib/dbus/machine-id"
            self.assertTrue(copy.is_symlink())
            self.assertEqual(os.readlink(copy), "/etc/machine-id")

            # dbus-uuidgen --ensure creates a missing file but rejects an
            # empty one, so an image without systemd has none.
            root = self.make_root(directory, systemd=False)
            (root / "var/lib/dbus").mkdir(parents=True)
            derive.reset_machine_id(root)
            self.assertFalse(os.path.lexists(root / "etc/machine-id"))
            self.assertEqual(
                os.readlink(root / "var/lib/dbus/machine-id"), "/etc/machine-id"
            )

            # An image without D-Bus state gains none.
            (root / "etc/machine-id").write_text("96662f44ec16583a845a46f66a803704\n")
            shutil.rmtree(root / "var")
            derive.reset_machine_id(root)
            self.assertFalse(os.path.lexists(root / "etc/machine-id"))
            self.assertFalse(os.path.lexists(root / "var"))

            # A path that could lead outside the tree is refused.
            (root / "var").mkdir()
            (root / "var/lib").symlink_to("/var/lib")
            with self.assertRaisesRegex(derive.DerivationError, "/var/lib is"):
                derive.reset_machine_id(root)
            (root / "var/lib").unlink()
            (root / "etc/machine-id").symlink_to("/etc/machine-id")
            with self.assertRaisesRegex(derive.DerivationError, "not a file"):
                derive.reset_machine_id(root)

    def test_derivation_resets_the_machine_id_after_the_setup(self) -> None:
        # The setup may install packages that write a machine ID, so the
        # reset follows it.
        source = inspect.getsource(load_derive().derive)
        setup = source.index('["chroot", str(root), "/bin/sh", "-s"]')
        reset = source.index("reset_machine_id(root)")
        image = source.index('"mkfs.erofs"')
        self.assertLess(setup, reset)
        self.assertLess(reset, image)

    @staticmethod
    def shared_object(machine: int, kind: int = 3) -> bytes:
        header = bytearray(64)
        header[:4] = b"\x7fELF"
        header[4] = 2
        header[5] = 1
        header[16:18] = kind.to_bytes(2, "little")
        header[18:20] = machine.to_bytes(2, "little")
        return bytes(header)

    def test_the_video_driver_is_a_shared_object_for_the_image(self) -> None:
        derive = load_derive()
        with tempfile.TemporaryDirectory() as name:
            directory = pathlib.Path(name)
            good = directory / "good.so"
            good.write_bytes(self.shared_object(183))
            derive.check_driver(good, "aarch64")
            for label, content in (
                ("another machine", self.shared_object(62)),
                ("an executable", self.shared_object(183, kind=2)),
                ("not an ELF file", b"#!/bin/sh\n" + bytes(64)),
                ("truncated", self.shared_object(183)[:10]),
            ):
                with self.subTest(label):
                    bad = directory / "bad.so"
                    bad.write_bytes(content)
                    with self.assertRaisesRegex(
                        derive.DerivationError, "shared object"
                    ):
                        derive.check_driver(bad, "aarch64")
            with self.assertRaisesRegex(derive.DerivationError, "shared object"):
                derive.check_driver(directory / "missing.so", "aarch64")

    def test_the_kde_image_carries_the_video_driver_and_its_chromium_file(
        self,
    ) -> None:
        derive = load_derive()
        descriptor = json.loads(
            (DESKTOP / "images.json").read_text(encoding="utf-8")
        )
        entry = descriptor["images"]["kde"]
        self.assertEqual(
            {name for name, image in descriptor["images"].items() if "driver" in image},
            {"kde"},
        )
        configuration = (DESKTOP / entry["driver"]["configuration"]["from"]).resolve()
        text = configuration.read_text(encoding="utf-8")
        # What Chromium needs to use the driver: the VA-API driver name and a
        # render node it cannot find itself, and the encode features are on.
        self.assertIn("export LIBVA_DRIVER_NAME=uml\n", text)
        self.assertIn("--render-node-override=/dev/dri/renderD128", text)
        self.assertIn("--enable-features=AcceleratedVideoEncoder,ChromeOSHWVBREncoding", text)
        self.assertNotIn("--disable-gpu-memory-buffer-video-frames", text)
        self.assertIn("export UML_VA_V4L2_EXPORT=argb\n", text)
        self.assertNotIn("KERNARIUM_CHROMIUM_VIDEO_ENCODE", text)
        self.assertNotIn('CHROMIUM_FLAGS="$CHROMIUM_FLAGS --render-node-override=/dev/dri/renderD128 --enable-features', text)
        subprocess.run(["sh", "-n", str(configuration)], check=True)
        with tempfile.TemporaryDirectory() as name:
            directory = pathlib.Path(name)
            root = directory / "root"
            (root / "usr/lib/aarch64-linux-gnu/dri").mkdir(parents=True)
            (root / "etc").mkdir()
            (root / "usr/lib/aarch64-linux-gnu/gbm").mkdir(parents=True)
            driver = directory / "uml_drv_video.so"
            driver.write_bytes(self.shared_object(183) + b"driver")
            module = directory / "virtio_gpu_gbm.so"
            module.write_bytes(self.shared_object(183) + b"module")
            installed = derive.install_driver(root, entry, driver, module)
            target = root / entry["driver"]["destination"]
            self.assertEqual(target.read_bytes(), driver.read_bytes())
            self.assertEqual(target.stat().st_mode & 0o7777, 0o644)
            # The gbm module joins Mesa's backends, where libgbm looks for one
            # named after the DRM driver.
            self.assertEqual(
                entry["driver"]["gbm"]["destination"],
                "usr/lib/aarch64-linux-gnu/gbm/virtio_gpu_gbm.so",
            )
            installed_module = root / entry["driver"]["gbm"]["destination"]
            self.assertEqual(installed_module.read_bytes(), module.read_bytes())
            self.assertEqual(installed_module.stat().st_mode & 0o7777, 0o644)
            self.assertEqual(
                installed["gbm"]["sha256"], hashlib.sha256(module.read_bytes()).hexdigest()
            )
            chromium = root / entry["driver"]["configuration"]["destination"]
            self.assertEqual(chromium.read_text(encoding="utf-8"), text)
            self.assertEqual(
                installed["sha256"], hashlib.sha256(driver.read_bytes()).hexdigest()
            )
            # Neither file replaces one the image already has.
            with self.assertRaisesRegex(derive.DerivationError, "already has"):
                derive.install_driver(root, entry, driver, module)

    def test_a_driver_only_goes_to_an_image_that_names_one(self) -> None:
        derive = load_derive()
        source = inspect.getsource(derive.derive)
        self.assertIn("pass --driver", source)
        self.assertIn("--gbm-module", source)
        self.assertIn("carries no VA-API driver", source)
        # The driver is in place before the setup, and the setup's result is
        # checked for libraries the image lacks before the image is written.
        install = source.index("install_driver(root, entry, driver, gbm_module)")
        setup = source.index('["chroot", str(root), "/bin/sh", "-s"]')
        linked = source.index('"/usr/bin/ldd"')
        image = source.index('"mkfs.erofs"')
        self.assertLess(install, setup)
        self.assertLess(setup, linked)
        self.assertLess(linked, image)


if __name__ == "__main__":
    unittest.main()
