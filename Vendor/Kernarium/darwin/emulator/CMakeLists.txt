# SPDX-License-Identifier: GPL-2.0-or-later

set(_uml_emulator_boundary_sources
    src/emulator.cpp
    src/gpu_budget_plan.cpp
    src/options.cpp)
if(UML_BUILD_KERNEL_RUNNER)
    list(APPEND _uml_emulator_boundary_sources loader/elf_loader.cpp)
endif()

function(_uml_add_emulator_boundary TARGET TESTING)
    add_library(${TARGET} OBJECT ${_uml_emulator_boundary_sources})
    uml_declare_architecture_target(${TARGET} emulator)
    target_link_libraries(${TARGET} PRIVATE
        UML::ApiContract
        UML::DeviceGuestConsole
        UML::DeviceUbdPath
        UML::GuestPageConfiguration
        UML::RuntimeContract)
    target_include_directories(${TARGET} PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/api/include
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/device/network
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu
        ${PROJECT_SOURCE_DIR}/darwin/runtime
        ${CMAKE_CURRENT_SOURCE_DIR})
    if(UML_BUILD_KERNEL_RUNNER)
        target_include_directories(${TARGET} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/loader)
    endif()
    target_compile_definitions(${TARGET} PRIVATE
        UML_APPLE_PLATFORM_NAME="${UML_APPLE_PLATFORM}"
        UML_EMULATOR_HAS_KERNEL_RUNNER=$<BOOL:${UML_BUILD_KERNEL_RUNNER}>
        UML_EMULATOR_TESTING=${TESTING})
endfunction()

_uml_add_emulator_boundary(uml_emulator_boundary 0)

# An emulator archive is a final product, and CMake merges only object
# libraries and interface-carried objects into it, never a linked static
# archive. Socket-path ownership and the guest-end registry remain separate
# archives for the in-tree device libraries, so each emulator archive also
# carries their objects and the process-wide heap region; a link that names
# both archives loads only one copy of each table and heap region.
set(_uml_emulator_carried_objects
    $<TARGET_OBJECTS:uml_memory_heap>
    $<TARGET_OBJECTS:uml_device_socket_paths>
    $<TARGET_OBJECTS:uml_device_guest_end>)

add_library(uml_emulator STATIC
    $<TARGET_OBJECTS:uml_emulator_boundary>
    ${_uml_emulator_carried_objects})

add_library(UML::Emulator ALIAS uml_emulator)
uml_declare_architecture_target(uml_emulator emulator)

target_link_libraries(uml_emulator PRIVATE
    UML::ApiContract
    UML::DeviceGuestConsole
    UML::GuestPageConfiguration
    UML::NetworkDevice
    UML::RuntimeCore
    UML::RuntimeContract)
target_include_directories(uml_emulator PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/darwin/api/include>
    $<INSTALL_INTERFACE:include>)

set_target_properties(uml_emulator PROPERTIES
    OUTPUT_NAME uml_emulator
    PUBLIC_HEADER
        "${PROJECT_SOURCE_DIR}/darwin/api/include/uml/emulator.h;${PROJECT_SOURCE_DIR}/darwin/api/include/uml/abi_identity.h")

set(_uml_need_emulator_test_variant OFF)
if(UML_BUILD_UNIT_TESTS OR
   (UML_BUILD_TOOLING_TESTS AND UML_BUILD_KERNEL_RUNNER))
    set(_uml_need_emulator_test_variant ON)
endif()
if(_uml_need_emulator_test_variant)
    # The test kernel image, deterministic admission interleavings and memory
    # failure probes belong to a test-only product variant (src/testing.hpp).
    # The emulator and memory modules reuse the exact production source
    # manifests under compile-time test policies.
    _uml_add_emulator_boundary(uml_emulator_boundary_test_variant 1)
    add_library(uml_emulator_test_variant STATIC
        $<TARGET_OBJECTS:uml_emulator_boundary_test_variant>
        ${_uml_emulator_carried_objects})
    uml_declare_architecture_target(uml_emulator_test_variant emulator)
    set(_uml_test_runtime_core UML::RuntimeCore)
    if(TARGET uml_runtime_core_test_variant)
        set(_uml_test_runtime_core UML::RuntimeCoreTestVariant)
        target_compile_definitions(uml_emulator_test_variant
            PUBLIC UML_MEMORY_ALLOCATION_TESTING=1)
    endif()
    target_compile_definitions(uml_emulator_test_variant
        PUBLIC UML_EMULATOR_TESTING=1)
    target_link_libraries(uml_emulator_test_variant PRIVATE
        UML::ApiContract
        UML::DeviceGuestConsole
        UML::GuestPageConfiguration
        UML::NetworkDevice
        ${_uml_test_runtime_core}
        UML::RuntimeContract)
    target_include_directories(uml_emulator_test_variant PUBLIC
        ${PROJECT_SOURCE_DIR}/darwin/api/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

install(TARGETS uml_emulator
    ARCHIVE DESTINATION lib
    PUBLIC_HEADER DESTINATION include/uml)

# A static C API implemented in C++ and Objective-C++ must carry its runtime
# link requirements to installed C consumers. Keep this small config
# relocatable and describe the exact platform/profile represented by the
# archive beside it.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/UMLEmulatorConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/UMLEmulatorConfig.cmake
    @ONLY NEWLINE_STYLE UNIX)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/UMLEmulatorConfig.cmake
    DESTINATION lib/cmake/UMLEmulator)
