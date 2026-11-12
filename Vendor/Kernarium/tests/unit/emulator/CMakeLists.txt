# SPDX-License-Identifier: GPL-2.0-or-later

# Emulator product tests: options, the C API boundary, the run API, and
# native-window admission and ownership. Runs enter a test kernel image
# through the emulator test variant (darwin/emulator/src/testing.hpp). The
# native-window helpers live in tests/unit/memory; every test here links the
# shared-backing archive MappedBacking references.

add_executable(uml_emulator_c_api_boundary_tests
    emulator_c_api_boundary_tests.cpp)
target_link_libraries(uml_emulator_c_api_boundary_tests PRIVATE
    UML::BridgeAbi
    uml_emulator_test_variant)
uml_add_unit_test(
    NAME emulator-c-api-boundary
    COMMAND uml_emulator_c_api_boundary_tests)

# The lifecycle calls' console requests, seen by a socket standing where the
# guest binds its console.
add_executable(uml_emulator_lifecycle_tests emulator_lifecycle_tests.cpp)
target_link_libraries(uml_emulator_lifecycle_tests PRIVATE
    UML::BridgeAbi
    UML::DeviceGuestConsole
    UML::MemorySharedBacking
    uml_emulator_test_variant)
target_include_directories(uml_emulator_lifecycle_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/runtime
    ${PROJECT_SOURCE_DIR}/tests/unit/mconsole
    ${PROJECT_SOURCE_DIR}/tests/unit/runtime)
uml_add_unit_test(
    NAME emulator-lifecycle
    COMMAND uml_emulator_lifecycle_tests
    LABELS console)
# A suspend that outlasts its deadline is aborted, never hibernated; each
# waits out the ten-second deadline.
uml_add_unit_test(
    NAME emulator-lifecycle-suspend-timeout
    COMMAND uml_emulator_lifecycle_tests suspend-timeout
    LABELS console)
uml_add_unit_test(
    NAME emulator-lifecycle-suspend-silent
    COMMAND uml_emulator_lifecycle_tests suspend-silent
    LABELS console)
# A guest whose threads the fence cannot hold keeps its disk locks; the fence
# spends its whole two-second budget finding that out.
uml_add_unit_test(
    NAME emulator-lifecycle-unheld-guest
    COMMAND uml_emulator_lifecycle_tests unheld-guest
    LABELS console)
# A guest that is inside the disk-lock registry nearly all the time is held at
# a moment when none of its threads owns the registry's mutex, or the release
# of the locks waits on a stopped owner for good.
uml_add_unit_test(
    NAME emulator-lifecycle-registry-owner
    COMMAND uml_emulator_lifecycle_tests registry-owner
    LABELS console)

# How a run says its guest ended: the kernel's report, the host's stop, and
# the runs that ran no guest.
add_executable(uml_emulator_run_end_tests run_end_tests.cpp)
target_link_libraries(uml_emulator_run_end_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking
    uml_emulator_test_variant)
target_include_directories(uml_emulator_run_end_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/runtime)
uml_add_unit_test(NAME emulator-run-end COMMAND uml_emulator_run_end_tests)

# What a handle says of its running guest: the vCPU carriers' CPU time, the
# RAM held, and nothing for a handle that does not run.
add_executable(uml_emulator_statistics_tests statistics_tests.cpp)
target_link_libraries(uml_emulator_statistics_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking
    uml_emulator_test_variant)
uml_add_unit_test(
    NAME emulator-statistics
    COMMAND uml_emulator_statistics_tests)

# The line a macOS launcher prints for each --stats interval.
add_executable(uml_emulator_statistics_report_tests
    statistics_report_tests.cpp)
target_include_directories(uml_emulator_statistics_report_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/emulator/src)
uml_add_unit_test(
    NAME emulator-statistics-report
    COMMAND uml_emulator_statistics_report_tests)

# Creation copies the kernel image, except the __TEXT,__uml_elf section the
# process links into itself, which is run in place.
add_executable(uml_emulator_kernel_image_ownership_tests
    kernel_image_ownership_tests.cpp)
target_link_libraries(uml_emulator_kernel_image_ownership_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking
    uml_emulator_test_variant)
uml_add_unit_test(
    NAME emulator-kernel-image-ownership
    COMMAND uml_emulator_kernel_image_ownership_tests)

# The limit a launch states for the process, and the budgets that derive from
# it.
add_executable(uml_emulator_memory_limit_tests memory_limit_tests.cpp)
target_link_libraries(uml_emulator_memory_limit_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking
    uml_emulator_test_variant)
target_include_directories(uml_emulator_memory_limit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(
    NAME emulator-memory-limit
    COMMAND uml_emulator_memory_limit_tests)

# The host GPU's budget as a launch plans it, from the arguments, the gadget
# cache the run will set and the limit the launch states.
add_executable(uml_emulator_gpu_budget_plan_tests gpu_budget_plan_tests.cpp)
target_link_libraries(uml_emulator_gpu_budget_plan_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking
    uml_emulator_test_variant)
target_include_directories(uml_emulator_gpu_budget_plan_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/darwin/emulator/src)
uml_add_unit_test(
    NAME emulator-gpu-budget-plan
    COMMAND uml_emulator_gpu_budget_plan_tests)

# The guard that ends a loop of restarts the guest never boots past.
add_executable(uml_restart_loop_guard_tests restart_loop_guard_tests.cpp)
target_include_directories(uml_restart_loop_guard_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/emulator/src)
uml_add_unit_test(
    NAME emulator-restart-loop-guard
    COMMAND uml_restart_loop_guard_tests)

add_executable(uml_emulator_options_tests emulator_options_tests.cpp)
target_link_libraries(uml_emulator_options_tests PRIVATE
    UML::BridgeAbi
    UML::Emulator
    UML::MemorySharedBacking)
target_include_directories(uml_emulator_options_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/darwin/emulator/src
    ${PROJECT_SOURCE_DIR}/tests/unit/memory)
uml_add_unit_test(NAME emulator-options COMMAND uml_emulator_options_tests)

add_executable(uml_emulator_api_tests emulator_api_tests.cpp)
target_link_libraries(uml_emulator_api_tests PRIVATE
    UML::BridgeAbi
    uml_emulator_test_variant
    UML::MemorySharedBacking
    uml_test_allocation_failure)
target_include_directories(uml_emulator_api_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/tests/unit/memory)
target_compile_definitions(uml_emulator_api_tests PRIVATE
    UML_EMULATOR_TEST_HAS_KERNEL_RUNNER=$<BOOL:${UML_BUILD_KERNEL_RUNNER}>
    UML_EMULATOR_TEST_HAS_SLIRP=$<BOOL:${UML_BUILD_NETWORK_PROVIDER}>)
if(TARGET uml_network_provider)
    target_link_libraries(uml_emulator_api_tests PRIVATE
        UML::NetworkProvider)
endif()
uml_add_unit_test(NAME emulator-api COMMAND uml_emulator_api_tests)

add_executable(uml_native_4k_admission_tests native_4k_admission_tests.cpp)
target_link_libraries(uml_native_4k_admission_tests PRIVATE
    UML::BridgeAbi
    UML::MemorySharedBacking
    uml_emulator_test_variant)
target_include_directories(uml_native_4k_admission_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/darwin/memory/mmu
    ${PROJECT_SOURCE_DIR}/tests/unit/memory)
uml_add_unit_test(
    NAME emulator-native-4k-admission
    COMMAND uml_native_4k_admission_tests
    LABELS tsan)
