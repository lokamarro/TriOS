# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_runtime_context_tests context_tests.cpp)
target_link_libraries(uml_runtime_context_tests PRIVATE
    UML::BridgeAbi
    UML::Emulator
    uml_test_allocation_failure)
target_include_directories(uml_runtime_context_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(
    NAME runtime-context-lifecycle
    COMMAND uml_runtime_context_tests
    LABELS tsan)

# The import through which the guest kernel says how its run ends.
add_executable(uml_runtime_guest_shutdown_tests guest_shutdown_tests.cpp)
target_link_libraries(uml_runtime_guest_shutdown_tests PRIVATE
    UML::BridgeAbi
    UML::DeviceGuestEnd
    UML::Emulator)
target_include_directories(uml_runtime_guest_shutdown_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(
    NAME runtime-guest-shutdown
    COMMAND uml_runtime_guest_shutdown_tests
    LABELS tsan)

add_executable(uml_runtime_kernel_signal_scope_tests
    kernel_signal_scope_tests.cpp)
target_link_libraries(uml_runtime_kernel_signal_scope_tests PRIVATE
    UML::RuntimeKernelSignals)
uml_add_unit_test(
    NAME runtime-kernel-signal-scope
    COMMAND uml_runtime_kernel_signal_scope_tests)

# The panic path's rewrite of the kernel's frames to the carrier bytes.
add_executable(uml_runtime_kernel_crash_frames_tests
    kernel_crash_frames_tests.cpp)
target_link_libraries(uml_runtime_kernel_crash_frames_tests PRIVATE
    UML::RuntimeKernelSignals)
uml_add_unit_test(
    NAME runtime-kernel-crash-frames
    COMMAND uml_runtime_kernel_crash_frames_tests)

# The record of how a guest session ended and the kernel output it quotes.
add_executable(uml_runtime_session_record_tests
    session_record_tests.cpp)
target_link_libraries(uml_runtime_session_record_tests PRIVATE
    UML::RuntimeSessionRecord
    UML::RuntimeGuestStatistics)
uml_add_unit_test(
    NAME runtime-session-record
    COMMAND uml_runtime_session_record_tests)

# The kernel a shared library links into itself is found through the library
# that holds the lookup, not through the executable that loads it.
add_library(uml_runtime_embedded_kernel_probe SHARED
    embedded_kernel_probe.cpp
    $<TARGET_OBJECTS:uml_runtime_kernel_signals>)
# The directory's link_libraries() would give the library the process-wide
# test host-capability installer, which a second copy in the process aborts.
set_property(TARGET uml_runtime_embedded_kernel_probe PROPERTY
    LINK_LIBRARIES UML::MemorySharedBacking)
target_include_directories(uml_runtime_embedded_kernel_probe PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/darwin/runtime)
target_compile_options(uml_runtime_embedded_kernel_probe PRIVATE
    -Wall -Wextra -Werror)
target_link_options(uml_runtime_embedded_kernel_probe PRIVATE
    "LINKER:-sectcreate,__TEXT,__uml_elf,${CMAKE_CURRENT_SOURCE_DIR}/embedded_kernel_probe.cpp"
    "LINKER:-sectalign,__TEXT,__uml_elf,0x4000")
set_target_properties(uml_runtime_embedded_kernel_probe PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set_property(TARGET uml_runtime_embedded_kernel_probe APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/embedded_kernel_probe.cpp")
add_executable(uml_runtime_embedded_kernel_image_tests
    embedded_kernel_image_tests.cpp)
target_link_libraries(uml_runtime_embedded_kernel_image_tests PRIVATE
    UML::RuntimeKernelSignals)
add_dependencies(uml_runtime_embedded_kernel_image_tests
    uml_runtime_embedded_kernel_probe)
uml_add_unit_test(
    NAME runtime-embedded-kernel-image
    COMMAND uml_runtime_embedded_kernel_image_tests
        $<TARGET_FILE:uml_runtime_embedded_kernel_probe>)

# The ledger of what an embedded kernel creates through its imports.
add_executable(uml_runtime_kernel_resources_tests kernel_resources_tests.cpp)
target_link_libraries(uml_runtime_kernel_resources_tests PRIVATE
    UML::BridgeAbi
    uml_emulator_test_variant)
target_include_directories(uml_runtime_kernel_resources_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/runtime)
uml_add_unit_test(
    NAME runtime-kernel-resources
    COMMAND uml_runtime_kernel_resources_tests)

# The loader's release of the kernel image against a host-service view that
# still borrows its guest-RAM tail.
add_executable(uml_runtime_kernel_image_release_tests
    kernel_image_release_tests.cpp)
target_link_libraries(uml_runtime_kernel_image_release_tests PRIVATE
    UML::RuntimeKernelSignals)
uml_add_unit_test(
    NAME runtime-kernel-image-release
    COMMAND uml_runtime_kernel_image_release_tests
    LABELS tsan)

add_executable(uml_runtime_sequential_run_tests sequential_run_tests.cpp)
target_link_libraries(uml_runtime_sequential_run_tests PRIVATE
    UML::BridgeAbi
    UML::DeviceSocketPaths
    UML::MemorySharedBacking
    uml_emulator_test_variant
    uml_test_mach_vm_region)
# The JIT's exclusive-monitor probe the restart modes read between runs.
target_sources(uml_runtime_sequential_run_tests PRIVATE
    $<TARGET_OBJECTS:uml_arm64_jit_test_support>)
target_include_directories(uml_runtime_sequential_run_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/darwin/platform
    ${PROJECT_SOURCE_DIR}/darwin/runtime
    ${PROJECT_SOURCE_DIR}/tests/unit/emulator)
uml_add_unit_test(
    NAME runtime-sequential-run-same-handle
    COMMAND uml_runtime_sequential_run_tests same-handle)
uml_add_unit_test(
    NAME runtime-sequential-run-fresh-handle
    COMMAND uml_runtime_sequential_run_tests fresh-handle)
uml_add_unit_test(
    NAME runtime-sequential-run-unclaimed-first-run
    COMMAND uml_runtime_sequential_run_tests unclaimed-first-run)
# A guest that restarts itself boots again in the same process; every other end
# leaves the guest spent, the hand-back refuses what still holds the guest, and
# it waits for a stop or a resume still reaching the hibernation it drops.
# One process each, because the claim on guest RAM outlives every handle.
foreach(_uml_restart_mode IN ITEMS restart power-off halt stopped failed
        another-handle borrowed-view running-thread abort-stop abort-resume
        release-same-handle release-fresh-handle release-halt
        release-borrowed-view release-running-thread)
    uml_add_unit_test(
        NAME runtime-restart-${_uml_restart_mode}
        COMMAND uml_runtime_sequential_run_tests ${_uml_restart_mode})
endforeach()
unset(_uml_restart_mode)

add_executable(uml_runtime_x86_tier_tests x86_tier_tests.cpp)
target_include_directories(uml_runtime_x86_tier_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(
    NAME runtime-x86-tier
    COMMAND uml_runtime_x86_tier_tests
    LABELS tsan)

# The registry of an embedded kernel's threads across a storm of restarts.
add_executable(uml_runtime_kernel_threads_tests kernel_threads_tests.cpp)
target_link_libraries(uml_runtime_kernel_threads_tests PRIVATE
    UML::RuntimeThreadFence)
uml_add_unit_test(
    NAME runtime-kernel-threads
    COMMAND uml_runtime_kernel_threads_tests)

# The fence against real threads. Each test stops and starts threads of the
# process, so the binaries run alone.
add_executable(uml_runtime_thread_fence_tests thread_fence_tests.cpp)
target_link_libraries(uml_runtime_thread_fence_tests PRIVATE
    UML::RuntimeThreadFence)
uml_add_unit_test(
    NAME runtime-thread-fence
    COMMAND uml_runtime_thread_fence_tests
    TIMEOUT 240)
uml_add_unit_test(
    NAME runtime-thread-fence-registry-overflow
    COMMAND uml_runtime_thread_fence_tests overflow
    TIMEOUT 120)
set_tests_properties(runtime-thread-fence
    runtime-thread-fence-registry-overflow PROPERTIES RUN_SERIAL TRUE)

# The order of a hibernation against recording steps, and a real one against
# real kernel threads and guest RAM.
add_executable(uml_runtime_guest_hibernation_tests guest_hibernation_tests.cpp)
target_link_libraries(uml_runtime_guest_hibernation_tests PRIVATE
    UML::RuntimeHibernation
    UML::RuntimeSessionRecord
    UML::RuntimeThreadFence
    UML::MemorySharedBacking)
uml_add_unit_test(
    NAME runtime-guest-hibernation
    COMMAND uml_runtime_guest_hibernation_tests
    TIMEOUT 240)
set_tests_properties(runtime-guest-hibernation PROPERTIES RUN_SERIAL TRUE)

# The virtual Jetsam: the decision, the line, and the sampler of a run whose
# launch stated the process's memory limit.
add_executable(uml_runtime_virtual_jetsam_tests virtual_jetsam_tests.cpp)
target_link_libraries(uml_runtime_virtual_jetsam_tests PRIVATE
    UML::RuntimeVirtualJetsam
    UML::PlatformContract
    "-framework Foundation")
target_sources(uml_runtime_virtual_jetsam_tests PRIVATE
    $<TARGET_OBJECTS:uml_host_environment>)
target_include_directories(uml_runtime_virtual_jetsam_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform)
uml_add_unit_test(
    NAME runtime-virtual-jetsam
    COMMAND uml_runtime_virtual_jetsam_tests
    LABELS tsan)

# The memory the guest side of the process holds beside the guest's RAM as the
# vCPU count grows, and the most RAM a process limit leaves a guest.
add_executable(uml_runtime_guest_memory_budget_tests
    guest_memory_budget_tests.cpp)
target_include_directories(uml_runtime_guest_memory_budget_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(
    NAME runtime-guest-memory-budget
    COMMAND uml_runtime_guest_memory_budget_tests)

# What the host reads of a guest without asking it: the CPU time of the vCPU
# carriers, found by name among the process's threads, and the RAM the host
# holds for the guest.
add_executable(uml_runtime_guest_statistics_tests
    guest_statistics_tests.cpp)
target_link_libraries(uml_runtime_guest_statistics_tests PRIVATE
    UML::RuntimeGuestStatistics)
target_include_directories(uml_runtime_guest_statistics_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/runtime)
uml_add_unit_test(
    NAME runtime-guest-statistics
    COMMAND uml_runtime_guest_statistics_tests
    LABELS tsan)
