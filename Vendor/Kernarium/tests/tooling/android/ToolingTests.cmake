# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME lineage24-builder-lane
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_builder_lane.py
        --source-root ${PROJECT_SOURCE_DIR} --part verifiers
    LABELS android;audit
    TIMEOUT 180)

uml_add_tooling_test(
    NAME lineage24-target-files-fixtures
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_builder_lane.py
        --source-root ${PROJECT_SOURCE_DIR} --part target-files
    LABELS android;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME lineage24-aosp-patch-regenerator
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_aosp_patch_regenerator.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME lineage24-routed-workspaces
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_routed_workspaces.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 900)

uml_add_tooling_test(
    NAME lineage24-mesa-venus-source-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_mesa_venus_source_contract.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 300)

uml_add_tooling_test(
    NAME lineage24-mesa-venus-port-generator
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_generate_mesa_venus_port.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME lineage24-first-boot-precompile
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_precompile_first_boot_apps.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME lineage24-sign-release
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_sign_release.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME lineage24-release-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_release_contract.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 30)

# The list of paths that decide a release names every producer, and the object
# IDs it yields do not depend on history.
uml_add_tooling_test(
    NAME lineage24-release-inputs
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_release_inputs.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME android17-macos-launcher-options
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_boot_android.py
    LABELS android
    TIMEOUT 30)

uml_add_tooling_test(
    NAME android17-instance-rules
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_android_instance.py
    LABELS android
    TIMEOUT 30)

# Each Android runner's default adb forward port is its guest slot's first
# port, and its own fixed port outside a slot.
uml_add_tooling_test(
    NAME android-runner-slot-ports
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_slot_port_defaults.py
    LABELS android
    TIMEOUT 30)

uml_add_tooling_test(
    NAME android17-adb-debugging-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_adb_debugging_runner.py
    LABELS android
    TIMEOUT 30)

uml_add_tooling_test(
    NAME android17-provision-helper
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_android_provision.py
    LABELS android
    TIMEOUT 30)

uml_add_tooling_test(
    NAME android17-oobe-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_oobe_runner.py
    LABELS android
    TIMEOUT 30)
