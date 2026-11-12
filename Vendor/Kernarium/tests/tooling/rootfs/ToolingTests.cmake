# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME rootfs-provisioning
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_provision.py
    LABELS rootfs;provisioning
    TIMEOUT 120)

uml_add_tooling_test(
    NAME rootfs-overlay-launcher-options
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_boot_overlay_systemd.py
        --launcher ${PROJECT_SOURCE_DIR}/tools/boot-overlay-systemd.sh
    LABELS rootfs;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME rootfs-steam-x86-64-image-audit
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_steam_x86_64_image.py
    LABELS rootfs;steam;x86_64;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME rootfs-producer-helpers
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_producer.sh
    LABELS rootfs;audit
    TIMEOUT 240)

uml_add_tooling_test(
    NAME rootfs-starter-image-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_starter_images.py
        --starter-root ${PROJECT_SOURCE_DIR}/tools/rootfs/starter
    LABELS rootfs;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME rootfs-starter-release-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_starter_release.py
    LABELS rootfs;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME rootfs-video-driver-images
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_video_driver_images.py
    LABELS rootfs;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME rootfs-elf-audit
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_elf_audit.py
    LABELS rootfs;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME rootfs-boot-base-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_boot_base.py
        --boot-ramdisk ${PROJECT_SOURCE_DIR}/tools/rootfs/boot-ramdisk
    LABELS rootfs;boot;audit
    TIMEOUT 60)

if(TARGET uml-initramfs)
    uml_add_tooling_test(
        NAME rootfs-initramfs-host-tool
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_uml_initramfs.py
            --tool $<TARGET_FILE:uml-initramfs>
            --boot-ramdisk ${PROJECT_SOURCE_DIR}/tools/rootfs/boot-ramdisk
        LABELS rootfs;boot
        TIMEOUT 60)
endif()

uml_add_tooling_test(
    NAME rootfs-desktop-image-descriptor
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/rootfs/test_desktop_images.py
    LABELS rootfs;audit
    TIMEOUT 30)
