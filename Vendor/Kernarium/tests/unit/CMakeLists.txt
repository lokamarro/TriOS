# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_test_allocation_failure STATIC allocation_failure.cpp)
target_include_directories(uml_test_allocation_failure PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

# CPU conformance and engine/runtime tests have their own directory scope.
# This keeps the suite entry point small while preserving one explicit source
# manifest per test domain.
add_subdirectory(arm64)
if(TARGET uml_x86_64_gadgets)
    add_subdirectory(x86_64)
endif()
add_subdirectory(memory)
add_subdirectory(runtime)
add_subdirectory(emulator)
add_subdirectory(platform)
add_subdirectory(block)
add_subdirectory(sockets)
add_subdirectory(network)
add_subdirectory(require)
add_subdirectory(scratch)
add_subdirectory(mconsole)
add_subdirectory(kernarium)
# Swift package tests need the macOS host toolchain, not an iOS configure.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_subdirectory(guesthost)
    add_subdirectory(andarium)
endif()
add_subdirectory(input/vnc)
add_subdirectory(input/virtio)
add_subdirectory(audio)
add_subdirectory(battery)
add_subdirectory(vsock)
add_subdirectory(console)
add_subdirectory(gpu)
add_subdirectory(vhost_user)
add_subdirectory(video)
