# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_memory_contract INTERFACE)
add_library(UML::MemoryContract ALIAS uml_memory_contract)
uml_declare_architecture_target(uml_memory_contract memory)
target_link_libraries(uml_memory_contract INTERFACE
    UML::GuestPageConfiguration)

# The process's heap region and the heaps taken from it. It is an archive of its
# own, because the region is one per process and every composition of the MMU
# objects, and the CPU layer's caches, must load the same one.
add_library(uml_memory_heap STATIC domain_heap.cpp)
add_library(UML::MemoryHeap ALIAS uml_memory_heap)
uml_declare_architecture_target(uml_memory_heap memory)
target_include_directories(uml_memory_heap PUBLIC
    ${PROJECT_SOURCE_DIR}/darwin)
# An iOS app's address space is small and its guest RAM window takes most of
# it, so the region is 1 GiB there, the Simulator included, and 8 GiB on macOS.
if(UML_APPLE_PLATFORM MATCHES "^ios")
    set(_uml_heap_region_bytes 1073741824)
else()
    set(_uml_heap_region_bytes 8589934592)
endif()
target_compile_definitions(uml_memory_heap PRIVATE
    UML_HEAP_REGION_BYTES=${_uml_heap_region_bytes})

add_library(uml_memory_shared_backing STATIC
    shared_backing.cpp
    guest_ram_commitment.cpp
    guest_ram_image.cpp)
add_library(UML::MemorySharedBacking ALIAS uml_memory_shared_backing)
uml_declare_architecture_target(uml_memory_shared_backing memory)
# Every composition of the MMU objects links this archive, so it carries the
# heap region those objects allocate from.
target_link_libraries(uml_memory_shared_backing
    PUBLIC UML::MemoryContract UML::MemoryHeap compression
    PRIVATE UML::GuestTimerMask)
target_include_directories(uml_memory_shared_backing PUBLIC
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_definitions(uml_memory_shared_backing PRIVATE
    UML_WIRE_ANONYMOUS_GUEST_RAM=$<STREQUAL:${UML_APPLE_PLATFORM},ios>)

set(_uml_static_mmu_sources
    mmu/address_layout.cpp
    mmu/address_space.cpp
    mmu/execution_gate.cpp
    mmu/mapped_backing.cpp)

add_library(uml_static_mmu OBJECT ${_uml_static_mmu_sources})
uml_declare_architecture_target(uml_static_mmu memory)
target_link_libraries(uml_static_mmu PRIVATE UML::BridgeAbi UML::MemoryContract)
target_include_directories(uml_static_mmu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/mmu)
target_compile_definitions(uml_static_mmu PRIVATE
    UML_MEMORY_ALLOCATION_TESTING=0)

add_library(uml_static_guest_memory OBJECT static_guest_memory.cpp)
uml_declare_architecture_target(uml_static_guest_memory memory)
target_link_libraries(uml_static_guest_memory PRIVATE
    UML::BridgeAbi
    UML::MemoryContract)
target_include_directories(uml_static_guest_memory PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)

add_library(uml_memory_vm_bridge OBJECT mmu/static_api.cpp)
uml_declare_architecture_target(uml_memory_vm_bridge memory)
target_link_libraries(uml_memory_vm_bridge PRIVATE
    UML::BridgeAbi
    UML::MemoryContract)
target_include_directories(uml_memory_vm_bridge PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${CMAKE_CURRENT_SOURCE_DIR}/mmu)
# Only an embedded ELF kernel maps native memory through this bridge: the
# macOS CLI and the opt-in Kernarium runtime. A build without the runner,
# such as every XCFramework slice, keeps the software-MMU-only boundary;
# the macOS unit presets keep the native bridge because their tests
# exercise native mappings through it.
set(_uml_memory_has_native_vm OFF)
if(UML_BUILD_KERNEL_RUNNER OR (UML_TARGET_MACOS AND UML_BUILD_UNIT_TESTS))
    set(_uml_memory_has_native_vm ON)
endif()

target_compile_definitions(uml_memory_vm_bridge PRIVATE
    UML_MEMORY_HAS_NATIVE_VM=$<BOOL:${_uml_memory_has_native_vm}>)

if(_uml_memory_has_native_vm)
    set(_uml_native_mmu_sources
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu/native_address_space.cpp)
    add_library(uml_native_mmu OBJECT ${_uml_native_mmu_sources})
    uml_declare_architecture_target(uml_native_mmu memory)
    target_link_libraries(uml_native_mmu PRIVATE UML::MemoryContract)
    target_include_directories(uml_native_mmu PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
    target_compile_definitions(uml_native_mmu PRIVATE
        UML_MEMORY_ALLOCATION_TESTING=0)
endif()

# Keep complete memory-object composition in the owning module. Consumers
# receive target-owned objects rather than copying a source manifest across
# directory scopes. The shared backing also stays an archive so separate
# in-tree final links load one anonymous-backing registry; carrying its object
# here keeps an archive composed from this component complete.
add_library(uml_memory_component INTERFACE)
add_library(UML::MemoryComponent ALIAS uml_memory_component)
uml_declare_architecture_target(uml_memory_component memory)
target_link_libraries(uml_memory_component INTERFACE
    UML::MemoryContract
    UML::MemorySharedBacking)
target_sources(uml_memory_component INTERFACE
    $<TARGET_OBJECTS:uml_memory_shared_backing>
    $<TARGET_OBJECTS:uml_static_mmu>
    $<TARGET_OBJECTS:uml_static_guest_memory>
    $<TARGET_OBJECTS:uml_memory_vm_bridge>)
if(TARGET uml_native_mmu)
    target_sources(uml_memory_component INTERFACE
        $<TARGET_OBJECTS:uml_native_mmu>)
endif()

if(UML_BUILD_UNIT_TESTS)
    # Fault injection and white-box ownership queries recompile the exact
    # production implementation manifests under a test-only policy. Ordinary
    # products contain neither the probe atomics nor their conditional calls.
    add_library(uml_static_mmu_test_variant OBJECT
        ${_uml_static_mmu_sources}
        mmu/allocation_fault.cpp)
    uml_declare_architecture_target(uml_static_mmu_test_variant memory)
    target_link_libraries(uml_static_mmu_test_variant PRIVATE
        UML::BridgeAbi
        UML::MemoryContract)
    target_include_directories(uml_static_mmu_test_variant PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/mmu)
    target_compile_definitions(uml_static_mmu_test_variant PUBLIC
        UML_MEMORY_ALLOCATION_TESTING=1)

    if(TARGET uml_native_mmu)
        add_library(uml_native_mmu_test_variant OBJECT
            ${_uml_native_mmu_sources})
        uml_declare_architecture_target(uml_native_mmu_test_variant memory)
        target_link_libraries(uml_native_mmu_test_variant PRIVATE
            UML::MemoryContract)
        target_include_directories(uml_native_mmu_test_variant PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
        target_compile_definitions(uml_native_mmu_test_variant PUBLIC
            UML_MEMORY_ALLOCATION_TESTING=1)
    endif()

    # The published address window alone, for its reader test; products
    # compile it into uml_static_mmu.
    add_library(uml_address_layout_test_variant OBJECT mmu/address_layout.cpp)
    uml_declare_architecture_target(uml_address_layout_test_variant memory)
    target_link_libraries(uml_address_layout_test_variant PUBLIC
        UML::MemoryContract)
    target_include_directories(uml_address_layout_test_variant PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/mmu)

    add_library(uml_memory_test_component INTERFACE)
    add_library(UML::MemoryTestComponent ALIAS uml_memory_test_component)
    uml_declare_architecture_target(uml_memory_test_component memory)
    target_link_libraries(uml_memory_test_component INTERFACE
        UML::MemoryContract
        UML::MemorySharedBacking)
    target_compile_definitions(uml_memory_test_component INTERFACE
        UML_MEMORY_ALLOCATION_TESTING=1)
    target_sources(uml_memory_test_component INTERFACE
        $<TARGET_OBJECTS:uml_memory_shared_backing>
        $<TARGET_OBJECTS:uml_static_mmu_test_variant>
        $<TARGET_OBJECTS:uml_static_guest_memory>
        $<TARGET_OBJECTS:uml_memory_vm_bridge>)
    if(TARGET uml_native_mmu_test_variant)
        target_sources(uml_memory_test_component INTERFACE
            $<TARGET_OBJECTS:uml_native_mmu_test_variant>)
    endif()
endif()
