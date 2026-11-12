# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME guest-harness-attached-session
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/emulator/guest_harness_session_tests.py
    LABELS emulator;guest
    TIMEOUT 180)

# The shared probe runner's installed files, guest commands and verdicts, with
# the guest replaced by a recorder.
uml_add_tooling_test(
    NAME guest-probe-runner
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/emulator/run_guest_probe_tests.py
    LABELS emulator;guest
    TIMEOUT 60)

# The AArch32 engine runner's verdict on fixed console and host output: a
# kernel line inside a probe line is no crash, and every real crash fails.
uml_add_tooling_test(
    NAME guest-aarch32-engine-runner
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/emulator/a32_engine_runner_tests.py
    LABELS emulator;guest;aarch32
    TIMEOUT 60)

# The fixture probe runner's guest command and the message of every way a probe
# fails, with the guest replaced by a recorder.
uml_add_tooling_test(
    NAME guest-fixture-probe-runner
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/emulator/fixture_transfer_tests.py
    LABELS emulator;guest
    TIMEOUT 60)

# How the guest-restart runner compares the process at two boots: the reply
# ports follow the threads, and everything else is exact. No guest starts.
uml_add_tooling_test(
    NAME guest-restart-audit-comparison
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/emulator/guest_restart_audit_tests.py
    LABELS emulator;guest
    TIMEOUT 60)

if(UML_BUILD_KERNEL_RUNNER)
    add_executable(uml_embedded_kernel_serialization_tests
        ${PROJECT_SOURCE_DIR}/tests/tooling/emulator/embedded_kernel_serialization_tests.cpp)
    target_link_libraries(uml_embedded_kernel_serialization_tests PRIVATE
        UML::BridgeAbi
        uml_emulator_test_variant
        UML::GuestPageConfiguration)
    target_include_directories(uml_embedded_kernel_serialization_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/runtime)

    uml_add_tooling_test(
        NAME embedded-kernel-handle-serialization
        COMMAND uml_embedded_kernel_serialization_tests
        LABELS emulator;loader
        TIMEOUT 30)
endif()

# The macOS executables carry the bundle identifier under which Metal keeps
# their shader cache.
get_property(_uml_identity_apps GLOBAL PROPERTY UML_MACOS_APP_TARGETS)
set(_uml_identity_executables)
foreach(_uml_app IN LISTS _uml_identity_apps)
    get_target_property(_uml_identity "${_uml_app}" UML_BUNDLE_IDENTIFIER)
    list(APPEND _uml_identity_executables
        --executable "$<TARGET_FILE:${_uml_app}>")
endforeach()
if(_uml_identity_executables)
    uml_add_tooling_test(
        NAME macos-app-bundle-identifier
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/tooling/emulator/test_macos_app_identity.py
            --identifier "${_uml_identity}"
            ${_uml_identity_executables}
        LABELS emulator;macos
        TIMEOUT 30)
endif()
unset(_uml_identity_apps)
unset(_uml_identity_executables)
unset(_uml_identity)
