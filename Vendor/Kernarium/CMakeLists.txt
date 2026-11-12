# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.27)

project(uml_apple LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The one owner of the warning policy: every first-party C++ and
# Objective-C++ source builds warning-clean. Hand-written assembly opts in
# with a source COMPILE_OPTIONS property. Objective-C++ always uses ARC.
add_compile_options("$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wall;-Wextra;-Werror>"
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")

list(APPEND CMAKE_MODULE_PATH
    "${PROJECT_SOURCE_DIR}/cmake"
    "${PROJECT_SOURCE_DIR}/dependencies/cmake")
include(UmlTesting)
include(VerifyDependencies)
include(VerifyAbi)
include(UmlArchitecture)
include(UmlAppleSdk)
uml_track_apple_sdk()

# The one interpreter for every Python tool and test. Python 3.11 or newer:
# tomllib is in the standard library from 3.11. Later find_package(Python3)
# calls in subdirectories reuse the interpreter cached here.
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

uml_verify_generated_abi()

# Request the codemodel and the cache for this configure invocation. Unlike
# creating a query file while configuration is already in progress,
# cmake_file_api() guarantees that a fresh build tree has its reply at
# generation time. The architecture audit reads the cache for the platform.
cmake_file_api(QUERY API_VERSION 1 CODEMODEL 2 CACHE 2)

set(UML_APPLE_PLATFORM "macos" CACHE STRING
    "Apple target platform: macos, ios, or ios-simulator")
set_property(CACHE UML_APPLE_PLATFORM PROPERTY STRINGS macos ios ios-simulator)

if(NOT UML_APPLE_PLATFORM MATCHES "^(macos|ios|ios-simulator)$")
    message(FATAL_ERROR "Unsupported UML_APPLE_PLATFORM=${UML_APPLE_PLATFORM}")
endif()

if(NOT APPLE)
    message(FATAL_ERROR "The UML Apple emulator requires an Apple SDK")
endif()

if(UML_APPLE_PLATFORM STREQUAL "macos")
    set(UML_TARGET_MACOS ON)
else()
    set(UML_TARGET_MACOS OFF)
endif()
option(UML_BUILD_CLI "Build the arm64 macOS command-line app"
    ${UML_TARGET_MACOS})
option(UML_BUILD_ANDROID17_CLI
    "Build the separate Android 17 arm64 macOS executable" OFF)
option(UML_BUILD_KERNEL_RUNNER
    "Build the embedded ELF kernel runner for the macOS CLI or Kernarium"
    ${UML_TARGET_MACOS})

option(UML_BUILD_UNIT_TESTS
    "Build host-only C++/assembly unit tests"
    ${UML_TARGET_MACOS})
option(UML_BUILD_INTEGRATION_TESTS
    "Register guest, rootfs, Python, VNC, and workload integration tests" OFF)
set(_uml_build_tooling_tests_default OFF)
if(UML_BUILD_UNIT_TESTS OR UML_BUILD_INTEGRATION_TESTS)
    set(_uml_build_tooling_tests_default ON)
endif()
option(UML_BUILD_TOOLING_TESTS
    "Build and register host-only repository/tooling validation tests"
    ${_uml_build_tooling_tests_default})
set(UML_INTEGRATION_ROOTFS "" CACHE FILEPATH
    "Unified EROFS rootfs image used by integration tests")
option(UML_ENABLE_EXTERNAL_NETWORK_TESTS
    "Register guest tests that intentionally access the public network" OFF)
set(UML_EXTERNAL_NETWORK_TEST_HOST "example.com" CACHE STRING
    "Public host used only when UML_ENABLE_EXTERNAL_NETWORK_TESTS is enabled")
set(UML_DEPENDENCY_PREFIX
    "${PROJECT_SOURCE_DIR}/.build/dependencies/macos-arm64-27"
    CACHE PATH "Private prefix containing source-built host dependencies")
option(UML_BUILD_GPU "Build the host GPU/display provider" ${UML_TARGET_MACOS})
option(UML_BUILD_AUDIO "Build the host Virtio Sound playback provider"
    ${UML_BUILD_GPU})
option(UML_BUILD_KERNARIUM_RUNTIME
    "Build the Kernarium application framework for iOS" OFF)
if(UML_BUILD_KERNARIUM_RUNTIME)
    # The framework's dSYM needs debug information from its whole static
    # closure, including optimized Release objects.
    add_compile_options(-g)
endif()
option(UML_KERNARIUM_AUTOMATION_HARNESS
    "Build the automation-only Kernarium runtime: Linux boots host payloads, Android lets a launch turn ADB authorization off"
    OFF)
option(UML_APP_STORE_RUNTIME
    "Build the App Store Kernarium runtime: it reads no environment variable that tunes, observes or drives a run, and carries no debugging or automation support"
    OFF)
option(UML_BUILD_NETWORK_PROVIDER
    "Build the optional SLiRP network provider"
    ${UML_BUILD_KERNEL_RUNNER})
if(UML_APP_STORE_RUNTIME)
    if(UML_KERNARIUM_AUTOMATION_HARNESS OR NOT UML_BUILD_KERNARIUM_RUNTIME)
        message(FATAL_ERROR
            "UML_APP_STORE_RUNTIME builds the Kernarium runtime and conflicts "
            "with UML_KERNARIUM_AUTOMATION_HARNESS")
    endif()
    # Every component of the runtime reads its diagnostics and A/B switches
    # through this definition, so one tree agrees.
    add_compile_definitions(UML_APP_STORE_RUNTIME)
endif()

# darwin/platform compiles Objective-C++ in every configuration. Without the
# language its .mm sources fall to the C++ rule, which drops their
# OBJCXX-only options, among them the -fobjc-arc set above.
enable_language(OBJCXX)

set(UML_MAINLINE_KERNEL_SLICE "arm64" CACHE STRING
    "Mainline kernel userspace ISA slice")
set_property(CACHE UML_MAINLINE_KERNEL_SLICE PROPERTY STRINGS arm64 x86_64)
if(NOT UML_MAINLINE_KERNEL_SLICE STREQUAL "arm64" AND
   NOT UML_MAINLINE_KERNEL_SLICE STREQUAL "x86_64")
    message(FATAL_ERROR
        "UML_MAINLINE_KERNEL_SLICE must be arm64 or x86_64")
endif()
if(UML_MAINLINE_KERNEL_SLICE STREQUAL "x86_64" AND NOT UML_TARGET_MACOS)
    message(FATAL_ERROR
        "The x86_64 mainline kernel slice is a macOS product")
endif()
if(UML_MAINLINE_KERNEL_SLICE STREQUAL "x86_64" AND
   (UML_BUILD_UNIT_TESTS OR UML_BUILD_INTEGRATION_TESTS))
    message(FATAL_ERROR
        "The fixed x86_64 userspace product does not use the arm64 unit or "
        "integration suites")
endif()

# Every guest uses 4 KiB pages. The host page stays 16 KiB on Apple silicon,
# and the code that maps a 4 KiB guest page inside it is host-page handling.
set(UML_GUEST_PAGE_SIZE 4096)
set(UML_GUEST_PAGE_SHIFT 12)
if(UML_TARGET_MACOS AND
   (UML_MAINLINE_KERNEL_SLICE STREQUAL "x86_64" OR UML_BUILD_UNIT_TESTS))
    set(UML_BUILD_X86_64_GUEST ON)
else()
    set(UML_BUILD_X86_64_GUEST OFF)
endif()
if(UML_MAINLINE_KERNEL_SLICE STREQUAL "x86_64")
    set(UML_X86_64_USERSPACE_ONLY ON)
else()
    set(UML_X86_64_USERSPACE_ONLY OFF)
endif()
if(UML_X86_64_USERSPACE_ONLY AND NOT UML_BUILD_X86_64_GUEST)
    message(FATAL_ERROR
        "The fixed x86_64 userspace slice requires the x86_64 gadget backend")
endif()
# The x86-64 JIT embeds FEXCore, which only the macOS dependency prefix
# carries. It is a macOS product by design: no iOS product, XCFramework slice
# or Kernarium runtime may build it, and the architecture audit and the iOS
# package checks prove that it does not.
option(UML_BUILD_X86_64_JIT
    "Build the macOS-only x86-64 JIT engine beside the x86-64 gadget engine"
    ${UML_BUILD_X86_64_GUEST})
if(UML_BUILD_X86_64_JIT AND NOT UML_TARGET_MACOS)
    message(FATAL_ERROR
        "The x86-64 JIT is macOS-only; UML_APPLE_PLATFORM="
        "${UML_APPLE_PLATFORM} cannot build it")
endif()
if(UML_BUILD_X86_64_JIT AND NOT UML_BUILD_X86_64_GUEST)
    message(FATAL_ERROR
        "The x86-64 JIT runs the 4 KiB x86-64 guest, which this "
        "configuration does not build")
endif()
# The x86-64 product dispatches x86 code to the JIT when it is selected. The
# arm64 product's x86 test guest builds the engine for its unit tests only and
# runs x86 code on the gadget engine.
if(UML_BUILD_X86_64_JIT AND UML_X86_64_USERSPACE_ONLY)
    set(UML_X86_64_JIT_RUNTIME ON)
else()
    set(UML_X86_64_JIT_RUNTIME OFF)
endif()
# Every Apple product uses one relocatable ET_DYN kernel linked at 48 GiB and
# one sparse software guest-address contract. Device and Simulator top-pack the
# scalable native window below the physical-iOS 62 GiB ceiling. macOS exercises
# the same relocation, exclusion-hole, and RAM-scaled auxiliary mechanisms at
# its established 96 TiB native base, outside Darwin's crowded low host range.
# The window is guest RAM plus a page-rounded auxiliary allowance for kernel
# mappings; that allowance is stated with its constants below. The guarded
# vhost-user shared-memory reservation and the kernel stub pages are additional
# address space and must not consume that ordinary allowance.
# The guest page tables expose a 47-bit range (128 TiB); a true 48-bit range
# would add a page-table level to every kernel.
set(UML_GUEST_ADDRESS_LIMIT 0x0000800000000000ULL)
set(UML_NATIVE_ADDRESS_START 0x0000000c00000000ULL)
set(UML_NATIVE_ADDRESS_END 0x0000000f80000000ULL)
set(UML_NATIVE_LINK_ADDRESS 0x0000000c00000000ULL)
set(UML_NATIVE_ADDRESS_CEILING 0x0000000f80000000ULL)
set(UML_MACOS_NATIVE_ADDRESS 0x0000600000000000ULL)
# The auxiliary window is address space, not memory: vmalloc's pages come from
# the guest page allocator and are already bounded by mem=. Sizing it at one
# eighth of RAM made it the binding constraint instead - 504 MiB on a 4 GiB
# guest, which vmapped kernel stacks exhausted. Track RAM one-for-one between a
# 1 GiB floor and a 4 GiB ceiling so vmalloc is never what runs out. The
# ceiling, together with the reserved space below, keeps RAM plus auxiliary
# inside the 14 GiB iOS envelope for every guest up to 9 GiB; no Apple product
# configures more than 4 GiB.
set(UML_NATIVE_AUXILIARY_MINIMUM 0x0000000040000000ULL)
set(UML_NATIVE_AUXILIARY_MAXIMUM 0x0000000100000000ULL)
# The kernel reserves a guarded 1 GiB guest-physical aperture for negotiated
# vhost-user shared-memory regions plus its stub pages above the auxiliary
# window, so the host reservation must cover the same span or the kernel's own
# stub and vmalloc tail fall outside the admitted native window. These values
# mirror UML_VHOST_USER_SHMEM_RESERVATION_SIZE and STUB_SIZE in the kernel
# series; they are address space, not memory.
set(UML_NATIVE_VHOST_USER_SHMEM_RESERVATION 0x0000000040008000ULL)
set(UML_NATIVE_STUB_PAGES 3)
set(UML_SIGRETURN_TRAMPOLINE 0x00007fffffffff00ULL)
if(UML_APPLE_PLATFORM MATCHES "^ios")
    set(UML_SIGNED_KERNEL_CARRIER 1)
else()
    set(UML_SIGNED_KERNEL_CARRIER 0)
endif()
add_library(uml_guest_page_configuration INTERFACE)
add_library(UML::GuestPageConfiguration ALIAS
    uml_guest_page_configuration)
target_compile_definitions(uml_guest_page_configuration INTERFACE
    DARWIN_UML_X86_64_GUEST=$<BOOL:${UML_BUILD_X86_64_GUEST}>
    DARWIN_UML_X86_64_USERSPACE_ONLY=$<BOOL:${UML_X86_64_USERSPACE_ONLY}>
    DARWIN_UML_X86_64_JIT=$<BOOL:${UML_X86_64_JIT_RUNTIME}>
    DARWIN_UML_GUEST_PAGE_SIZE=${UML_GUEST_PAGE_SIZE}
    DARWIN_UML_GUEST_PAGE_SHIFT=${UML_GUEST_PAGE_SHIFT}
    DARWIN_UML_GUEST_ADDRESS_LIMIT=${UML_GUEST_ADDRESS_LIMIT}
    DARWIN_UML_NATIVE_ADDRESS_START=${UML_NATIVE_ADDRESS_START}
    DARWIN_UML_NATIVE_ADDRESS_END=${UML_NATIVE_ADDRESS_END}
    DARWIN_UML_NATIVE_LINK_ADDRESS=${UML_NATIVE_LINK_ADDRESS}
    DARWIN_UML_NATIVE_ADDRESS_CEILING=${UML_NATIVE_ADDRESS_CEILING}
    DARWIN_UML_MACOS_NATIVE_ADDRESS=${UML_MACOS_NATIVE_ADDRESS}
    DARWIN_UML_NATIVE_HIGH_PLACEMENT=$<BOOL:${UML_TARGET_MACOS}>
    DARWIN_UML_NATIVE_AUXILIARY_MINIMUM=${UML_NATIVE_AUXILIARY_MINIMUM}
    DARWIN_UML_NATIVE_AUXILIARY_MAXIMUM=${UML_NATIVE_AUXILIARY_MAXIMUM}
    DARWIN_UML_NATIVE_VHOST_USER_SHMEM_RESERVATION=${UML_NATIVE_VHOST_USER_SHMEM_RESERVATION}
    DARWIN_UML_NATIVE_STUB_PAGES=${UML_NATIVE_STUB_PAGES}
    DARWIN_UML_SIGRETURN_TRAMPOLINE=${UML_SIGRETURN_TRAMPOLINE}
    DARWIN_UML_SIGNED_KERNEL_CARRIER=${UML_SIGNED_KERNEL_CARRIER})

if(UML_BUILD_UNIT_TESTS OR UML_BUILD_INTEGRATION_TESTS OR
   UML_BUILD_TOOLING_TESTS)
    enable_testing()
endif()

if(UML_TARGET_MACOS AND (UML_BUILD_GPU OR UML_BUILD_NETWORK_PROVIDER OR
   UML_BUILD_X86_64_GUEST))
    add_subdirectory(dependencies)
endif()

if(UML_BUILD_GPU)
    include(dependencies/cmake/AppleGpuDependencies.cmake)
endif()

if(UML_BUILD_NETWORK_PROVIDER AND NOT UML_TARGET_MACOS)
    include(dependencies/cmake/IosNetworkDependencies.cmake)
endif()

if(UML_BUILD_CLI OR UML_BUILD_ANDROID17_CLI OR UML_BUILD_KERNARIUM_RUNTIME)
    add_subdirectory(kernel)
endif()
if(UML_BUILD_ANDROID17_CLI AND NOT UML_TARGET_MACOS)
    message(FATAL_ERROR "The Android 17 CLI is supported only on macOS")
endif()

add_subdirectory(darwin)

if(UML_BUILD_KERNARIUM_RUNTIME)
    add_subdirectory(Kernarium/Runtime)
endif()

if(UML_BUILD_UNIT_TESTS OR UML_BUILD_INTEGRATION_TESTS OR
   UML_BUILD_TOOLING_TESTS)
    add_subdirectory(tests)
endif()

uml_write_architecture_manifest()

# This structural gate is part of every Apple product configuration, including
# iOS builds with behavioral tests disabled. The File API reply is produced by
# the completed configure and the target never reconfigures its own tree.
add_custom_target(architecture-audit
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -P ${PROJECT_SOURCE_DIR}/scripts/audit-source-tree.cmake
    COMMAND ${CMAKE_COMMAND}
        -DBUILD_ROOT=${CMAKE_BINARY_DIR}
        -P ${PROJECT_SOURCE_DIR}/scripts/audit-cmake-file-api.cmake
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Auditing host source-tree architecture"
    VERBATIM)
