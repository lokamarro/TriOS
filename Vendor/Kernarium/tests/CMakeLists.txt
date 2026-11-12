# SPDX-License-Identifier: GPL-2.0-or-later

# Instruction assets are build outputs shared by unit and tooling consumers.
if(UML_BUILD_UNIT_TESTS OR UML_BUILD_TOOLING_TESTS)
    add_subdirectory(unit/arm64/corpus)
    add_subdirectory(unit/arm64/aarch32_corpus)
endif()
if(UML_BUILD_UNIT_TESTS AND TARGET uml_x86_64_gadgets)
    add_subdirectory(unit/x86_64/corpus)
    add_subdirectory(unit/x86_64/masked_flags)
endif()

# Host test fixtures for GPU builds. The standalone GPU server serves only the
# VNC tooling smoke; the VNC client serves that smoke and the external
# integration workflows, so either suite can use it without the other.
if(TARGET uml_gpu_service AND UML_BUILD_TOOLING_TESTS)
    add_executable(uml-virtio-gpu
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/virtio_gpu_fixture.cpp)
    target_link_libraries(uml-virtio-gpu PRIVATE UML::GpuDevice)
    target_include_directories(uml-virtio-gpu PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
    set_target_properties(uml-virtio-gpu PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()
if(TARGET uml_gpu_service AND (UML_BUILD_TOOLING_TESTS OR
   (UML_BUILD_INTEGRATION_TESTS AND UML_ENABLE_EXTERNAL_NETWORK_TESTS)))
    add_executable(uml-vnc-smoke-client
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/vnc_smoke_client.cpp)
    target_link_libraries(uml-vnc-smoke-client PRIVATE
        UML::ThirdPartyVncClient
        "-framework CoreFoundation")
    set_target_properties(uml-vnc-smoke-client PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()

# Manual benchmark payloads have explicit source ownership but remain outside
# all correctness-suite aggregates and registrations.
add_subdirectory(perf EXCLUDE_FROM_ALL)

if(UML_BUILD_UNIT_TESTS OR UML_BUILD_INTEGRATION_TESTS)
    # Test-only device wrappers that unit and integration tests share.
    add_subdirectory(support/vhost_user)
    add_subdirectory(support/video)
endif()

if(UML_BUILD_UNIT_TESTS)
    add_library(uml_host_capabilities_test_support OBJECT
        unit/host_capabilities_test_support.cpp)
    target_link_libraries(uml_host_capabilities_test_support PRIVATE
        UML::CpuContract
        UML::PlatformContract)
    target_include_directories(uml_host_capabilities_test_support PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    # Unit tests keep scratch files in the per-test build-tree directory that
    # uml_add_unit_test() names, instead of a host-global path.
    add_library(uml_test_scratch_directory OBJECT
        unit/scratch/scratch_directory.cpp)
    target_include_directories(uml_test_scratch_directory PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/scratch)
    # The require, fail and requireThrows assertions every unit test uses.
    add_library(uml_test_require INTERFACE)
    target_include_directories(uml_test_require INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/require)
    # One Mach VM region query for every test that inspects this task's map.
    add_library(uml_test_mach_vm_region INTERFACE)
    target_include_directories(uml_test_mach_vm_region INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/mach_vm)
    # Change-sequence reads the tests make without an execution context. An
    # archive, so a binary that names neither helper links no MMU symbols.
    add_library(uml_static_mmu_test_support STATIC
        unit/static_mmu/static_mmu_test_support.cpp)
    target_link_libraries(uml_static_mmu_test_support PRIVATE UML::BridgeAbi)
    target_include_directories(uml_static_mmu_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/static_mmu)
    # The route a run would give, for white-box tests that call the runtime's
    # guest-kernel callbacks without one.
    add_library(uml_runtime_test_route INTERFACE)
    target_include_directories(uml_runtime_test_route INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime)
    # Unit binaries are independent consumers of the same scoped build
    # configuration as production targets; no directory-global definitions
    # are needed.
    link_libraries(
        uml_host_capabilities_test_support
        uml_test_scratch_directory
        uml_test_require
        uml_test_mach_vm_region
        uml_static_mmu_test_support
        UML::GadgetHistogramConfiguration
        UML::GuestPageConfiguration)
    add_subdirectory(unit)
endif()

if(UML_BUILD_TOOLING_TESTS)
    add_subdirectory(tooling)
endif()

if(UML_BUILD_INTEGRATION_TESTS)
    # Guest payloads and helpers that a command reaches only through a path
    # add themselves to this aggregate; the finalization below adds every
    # target a registered command names.
    add_custom_target(integration-test-binaries)
    add_subdirectory(integration)
endif()

# Suite aggregates are derived from the actual target-backed CTest commands,
# including commands whose fixtures are owned by another directory. A build
# preset therefore cannot finish successfully while leaving a registered test
# executable absent from a fresh graph.
if(UML_BUILD_UNIT_TESTS)
    uml_finalize_suite_binary_target(unit unit-test-binaries)
endif()
if(UML_BUILD_UNIT_TESTS AND UML_BUILD_GPU)
    uml_finalize_test_preset_binary_target(unit-gpu gpu-unit-test-binaries)
endif()
if(UML_BUILD_TOOLING_TESTS)
    uml_finalize_suite_binary_target(tooling tooling-test-binaries)
    add_dependencies(tooling-test-binaries uml_arm64_instruction_corpus
        uml_arm64_aarch32_corpus)
endif()
if(UML_BUILD_INTEGRATION_TESTS)
    uml_finalize_suite_binary_target(integration integration-test-binaries)
endif()
