# SPDX-License-Identifier: GPL-2.0-or-later

# The host's side of an Android guest's disks (android_disk_tool.cpp): the
# runner creates and checks the data disk with the library both launchers use.
add_executable(uml_android_disk_tool android_disk_tool.cpp)
target_link_libraries(uml_android_disk_tool PRIVATE UML::AndroidBoot)
target_compile_options(uml_android_disk_tool PRIVATE -Wall -Wextra -Werror)
target_compile_features(uml_android_disk_tool PRIVATE cxx_std_20)
add_dependencies(integration-test-binaries uml_android_disk_tool)

# A guest boots on the release's read-only system disk and a blank data disk
# the host created, then on the same data disk again.
uml_add_guest_integration_test(
    NAME guest-android-disks
    SCRIPT run_guest_android_disks.py
    ARGS --disk-tool $<TARGET_FILE:uml_android_disk_tool>
        --android-root ${PROJECT_SOURCE_DIR}/android/lineage-24
    LABELS guest;boot;android
    TIMEOUT 600
    CPUS 1)

# The release's adb debugging setting and adbd's key rule (the p0007 and p0011
# adb topics), on a fresh data disk and across reboots. It boots Android with
# the macOS launcher, so it is opt-in: name the release under test.
set(UML_ANDROID_ADB_DEBUGGING_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-adb-debugging test boots")
if(UML_ANDROID_ADB_DEBUGGING_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-adb-debugging
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_adb_debugging.py
            --release ${UML_ANDROID_ADB_DEBUGGING_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# A fresh guest starts in Setup Wizard; the runner completes the wizard through
# its pages and checks that the harness helper (boot_android.py --provision)
# reaches the same end state. It boots Android twice with the macOS launcher, so
# it is opt-in: name the release under test.
set(UML_ANDROID_OOBE_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-oobe test boots")
if(UML_ANDROID_OOBE_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-oobe
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_oobe.py
            --release ${UML_ANDROID_OOBE_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()
