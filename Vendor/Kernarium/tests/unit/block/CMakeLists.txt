# SPDX-License-Identifier: GPL-2.0-or-later

# A small Android release that android/lineage-24/tools/ubd-disk.py assembles
# from the tracked disk policy, so the host's layout parser and partition
# table builder are tested against the producer's own output.
set(_uml_android_fixture_dir ${CMAKE_CURRENT_BINARY_DIR}/android-release-fixture)
set(_uml_android_producer_root ${PROJECT_SOURCE_DIR}/android/lineage-24)
set(_uml_android_fixture_files
    ${_uml_android_fixture_dir}/release/guest/system.img
    ${_uml_android_fixture_dir}/release/guest/partition-layout.json
    ${_uml_android_fixture_dir}/release/boot/kernel-command-line.txt
    ${_uml_android_fixture_dir}/reference-data-4.img)
add_custom_command(
    OUTPUT ${_uml_android_fixture_files}
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/support/android/android_release_fixture.py
        --android-root ${_uml_android_producer_root}
        --output ${_uml_android_fixture_dir}
    DEPENDS ${PROJECT_SOURCE_DIR}/tests/support/android/android_release_fixture.py
        ${_uml_android_producer_root}/tools/ubd-disk.py
        ${_uml_android_producer_root}/support/image/disk-policy.json
    COMMENT "Assembling the Android release fixture"
    VERBATIM)
add_custom_target(uml_android_release_fixture
    DEPENDS ${_uml_android_fixture_files})

add_executable(uml_android_boot_tests android_boot_tests.cpp)
target_link_libraries(uml_android_boot_tests PRIVATE UML::AndroidBoot)
add_dependencies(uml_android_boot_tests uml_android_release_fixture)
uml_add_unit_test(NAME android-partition-boot-selection
    COMMAND uml_android_boot_tests ${_uml_android_fixture_dir}
    LABELS android;device)

add_executable(uml_boot_initramfs_tests boot_initramfs_tests.cpp)
target_link_libraries(uml_boot_initramfs_tests PRIVATE UML::BootInitramfs)
uml_add_unit_test(NAME boot-initramfs-archive
    COMMAND uml_boot_initramfs_tests
    LABELS boot;device)
