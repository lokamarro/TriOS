#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Debian desktop producers build and select the Virtio video VA driver."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
ROOTFS = ROOT / "tools/rootfs"
VIDEO = ROOT / "tools/video/va-v4l2"


def text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


class VideoDriverImageTests(unittest.TestCase):
    def test_the_shared_helper_builds_tests_and_installs_in_the_image_root(
        self,
    ) -> None:
        producer = text(ROOTFS / "producer.sh")
        start = producer.index("producer_install_uml_va_driver()")
        helper = producer[start:producer.index("producer_stage()", start)]
        build = helper.index("cmake --build")
        unit = helper.index("uml_va_v4l2_tests")
        selftest = helper.index("tests/probe_selftest.sh")
        install = helper.index("cmake --install")
        purge = helper.index("apt-get purge")
        self.assertLess(build, unit)
        self.assertLess(unit, selftest)
        self.assertLess(selftest, install)
        self.assertLess(install, purge)
        # /usr, so that the library lands beside the other VA-API drivers and
        # the Chromium file under /etc.
        self.assertIn("-DCMAKE_INSTALL_PREFIX=/usr", helper)
        self.assertIn("not found", helper)
        # The gbm backend module ships with the driver and is checked the same
        # way, since every libgbm client of the image loads it.
        self.assertIn("virtio_gpu_gbm.so", helper)
        # The libraries the driver runs on outlive the build tools.
        self.assertRegex(helper, r"apt-mark manual[^\n]*libva2[^\n]*libdrm2")
        subprocess.run(["sh", "-n", str(ROOTFS / "producer.sh")], check=True)

    def test_the_driver_installs_the_chromium_file_with_itself(self) -> None:
        cmake = text(VIDEO / "CMakeLists.txt")
        self.assertIn(
            "install(FILES chromium.d/kernarium-video "
            "DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/chromium.d)",
            cmake,
        )
        self.assertIn(
            "DESTINATION ${CMAKE_INSTALL_LIBDIR}/dri)", cmake
        )

    def test_the_gbm_module_installs_where_libgbm_looks_for_backends(self) -> None:
        cmake = text(VIDEO / "CMakeLists.txt")
        # libgbm loads <driver name>_gbm.so from its backend directory, and the
        # module names the DRM driver it serves.
        self.assertIn("install(TARGETS virtio_gpu_gbm", cmake)
        self.assertIn("DESTINATION ${CMAKE_INSTALL_LIBDIR}/gbm", cmake)
        self.assertIn("virtio_gpu_gbm", cmake)

    def test_the_starter_plasma_image_builds_the_driver_after_chromium(
        self,
    ) -> None:
        script = text(ROOTFS / "starter/build-all.sh")
        start = script.index("configure_plasma_user()")
        body = script[start:script.index("build_debian_plasma()", start)]
        chromium = body.index("chromium")
        helper = body.index(
            'producer_install_uml_va_driver "$root" '
            '"$SCRIPT_DIR/../../video/va-v4l2"'
        )
        self.assertLess(chromium, helper)
        # The helper runs apt and the driver's tests, which need /proc.
        plasma = script[script.index("build_debian_plasma()"):]
        self.assertLess(
            plasma.index('mount_chroot_proc "$root"'),
            plasma.index('configure_plasma_user "$root"'),
        )
        subprocess.run(["sh", "-n", str(ROOTFS / "starter/build-all.sh")], check=True)

    def test_the_x86_64_desktop_image_carries_chromium_and_the_driver(self) -> None:
        script = text(ROOTFS / "x86_64-desktop/build.sh")
        packages = script.index("apt-get install -y --no-install-recommends")
        chromium = script.index(" chromium", packages)
        helper = script.index(
            'producer_install_uml_va_driver "$root" '
            '"$SCRIPT_DIR/../../video/va-v4l2"'
        )
        audit = script.index("verify-elf.py")
        self.assertLess(chromium, helper)
        self.assertLess(helper, audit)
        # /proc is mounted for the helper and gone again before the image is
        # written, whichever way the producer exits.
        mount = script.index('sudo mount -t proc proc "$root/proc"')
        unmount = script.index('sudo umount "$root/proc"', mount)
        self.assertLess(mount, script.index("producer_install_uml_va_driver", mount))
        self.assertLess(helper, unmount)
        self.assertLess(unmount, script.index("==> Building EROFS"))
        self.assertIn("producer_exit_hook=unmount_root_proc", script)
        subprocess.run(["bash", "-n", str(ROOTFS / "x86_64-desktop/build.sh")], check=True)


if __name__ == "__main__":
    unittest.main()
