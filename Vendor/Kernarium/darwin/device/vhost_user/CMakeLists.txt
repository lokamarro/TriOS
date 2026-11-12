# SPDX-License-Identifier: GPL-2.0-or-later

# The device-neutral vhost-user transport and in-process service scaffold
# that every vhost-user device is built on.
function(uml_configure_vhost_user_component target)
    uml_declare_architecture_target(${target} device)
    target_include_directories(${target} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${target} PUBLIC UML::Descriptor)
endfunction()

add_library(uml_vhost_user_wire STATIC vhost_user_message.cpp)
uml_configure_vhost_user_component(uml_vhost_user_wire)

add_library(uml_vhost_user_transport STATIC
    shared_memory_regions.cpp
    vhost_user_server.cpp)
add_library(UML::VhostUserTransport ALIAS uml_vhost_user_transport)
uml_configure_vhost_user_component(uml_vhost_user_transport)
target_link_libraries(uml_vhost_user_transport PUBLIC
    UML::MemorySharedBacking
    uml_vhost_user_wire)
# AutoreleaseScope calls the Objective-C runtime directly.
target_link_libraries(uml_vhost_user_transport PRIVATE UML::GuestTimerMask objc)

if(UML_BUILD_UNIT_TESTS AND UML_BUILD_GPU)
    # The GPU service tests build the transport under test-only policies;
    # the hooks those policies call are implemented beside the tests.

    # The wire with its stop-wait observer, so a test waits until a receiver
    # has consumed every queued fragment instead of sleeping and hoping.
    add_library(uml_gpu_transport_wire_stop_wait_test_variant STATIC
        vhost_user_message.cpp)
    uml_configure_vhost_user_component(
        uml_gpu_transport_wire_stop_wait_test_variant)
    target_compile_definitions(uml_gpu_transport_wire_stop_wait_test_variant
        PRIVATE UML_GPU_STOP_WAIT_TESTING=1)

    # The shared service scaffold has a startup checkpoint of its own.
    add_library(uml_gpu_transport_startup_test_variant STATIC
        shared_memory_regions.cpp
        vhost_user_server.cpp)
    uml_configure_vhost_user_component(uml_gpu_transport_startup_test_variant)
    target_compile_definitions(uml_gpu_transport_startup_test_variant PRIVATE
        UML_GPU_STARTUP_TESTING=1)
    target_link_libraries(uml_gpu_transport_startup_test_variant PUBLIC
        UML::MemorySharedBacking
        uml_gpu_transport_wire_stop_wait_test_variant)
    target_link_libraries(uml_gpu_transport_startup_test_variant PRIVATE
        UML::GuestTimerMask
        objc)

    # A deliberately slow stop poll makes the owned stop wake observable: the
    # child watchdog fails if service shutdown falls back to polling.
    add_library(uml_gpu_transport_wire_slow_stop_test_variant STATIC
        vhost_user_message.cpp)
    uml_configure_vhost_user_component(
        uml_gpu_transport_wire_slow_stop_test_variant)
    target_compile_definitions(uml_gpu_transport_wire_slow_stop_test_variant
        PRIVATE
            UML_GPU_STOP_POLL_TIMEOUT_MILLISECONDS=5000
            UML_GPU_STOP_WAIT_TESTING=1)

    add_library(uml_gpu_transport_slow_stop_test_variant STATIC
        shared_memory_regions.cpp
        vhost_user_server.cpp)
    uml_configure_vhost_user_component(
        uml_gpu_transport_slow_stop_test_variant)
    target_compile_definitions(uml_gpu_transport_slow_stop_test_variant
        PRIVATE UML_GPU_STOP_POLL_TIMEOUT_MILLISECONDS=5000)
    target_link_libraries(uml_gpu_transport_slow_stop_test_variant PUBLIC
        UML::MemorySharedBacking
        uml_gpu_transport_wire_slow_stop_test_variant)
    target_link_libraries(uml_gpu_transport_slow_stop_test_variant PRIVATE
        objc)
endif()
