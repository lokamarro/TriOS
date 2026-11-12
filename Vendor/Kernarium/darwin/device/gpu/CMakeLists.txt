# SPDX-License-Identifier: GPL-2.0-or-later

function(uml_configure_gpu_component target)
    uml_declare_architecture_target(${target} device)
    target_include_directories(${target} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/device/vhost_user
        ${PROJECT_SOURCE_DIR}/kernel/common/gpu)
    # Whether the host GL provider is a CPU rasterizer without Vulkan: the
    # iOS Simulator's. The product's prefix decides it, so every component of
    # one configuration, test variants included, agrees.
    target_compile_definitions(${target} PRIVATE
        UML_GPU_SOFTWARE_RENDERER=$<BOOL:${UML_GPU_SOFTWARE_RENDERER}>)
endfunction()

# Keep the renderer, display, protocol device, transport, and in-process
# service as independently owned units. Their dependency direction follows
# the data path and contains no reverse service callbacks.
# Where the system keeps this process's caches; see process_cache_location.hpp.
add_library(uml_gpu_process_cache_location STATIC process_cache_location.cpp)
add_library(UML::GpuProcessCacheLocation ALIAS uml_gpu_process_cache_location)
uml_configure_gpu_component(uml_gpu_process_cache_location)
target_link_libraries(uml_gpu_process_cache_location PRIVATE
    "-framework CoreFoundation")

# Bounds the process's own Metal shader cache; see metal_shader_cache.hpp.
add_library(uml_gpu_metal_shader_cache STATIC metal_shader_cache.cpp)
add_library(UML::GpuMetalShaderCache ALIAS uml_gpu_metal_shader_cache)
uml_configure_gpu_component(uml_gpu_metal_shader_cache)
target_link_libraries(uml_gpu_metal_shader_cache PRIVATE
    UML::GpuProcessCacheLocation)

# Places and bounds the process's own Mesa disk shader cache; see
# mesa_shader_cache.hpp.
add_library(uml_gpu_mesa_shader_cache STATIC mesa_shader_cache.cpp)
add_library(UML::GpuMesaShaderCache ALIAS uml_gpu_mesa_shader_cache)
uml_configure_gpu_component(uml_gpu_mesa_shader_cache)
target_link_libraries(uml_gpu_mesa_shader_cache PRIVATE
    UML::GpuProcessCacheLocation)

add_library(uml_gpu_device_memory_pool STATIC device_memory_pool.cpp)
add_library(UML::GpuDeviceMemoryPool ALIAS uml_gpu_device_memory_pool)
uml_configure_gpu_component(uml_gpu_device_memory_pool)

# What the guest's contexts hold of the host GPU's memory, by process; see
# context_memory_ledger.hpp. The display keeps one for the device and the
# service.
add_library(uml_gpu_context_memory_ledger STATIC context_memory_ledger.cpp)
add_library(UML::GpuContextMemoryLedger ALIAS uml_gpu_context_memory_ledger)
uml_configure_gpu_component(uml_gpu_context_memory_ledger)

# When the guest's graphics memory counts as under pressure, for a guest that
# subscribed to hear of it; see memory_pressure.hpp.
add_library(uml_gpu_memory_pressure STATIC memory_pressure.cpp)
add_library(UML::GpuMemoryPressure ALIAS uml_gpu_memory_pressure)
uml_configure_gpu_component(uml_gpu_memory_pressure)

# Finds the guest contexts that have gone quiet, for the renderer to trim.
add_library(uml_gpu_context_idle_tracker STATIC context_idle_tracker.cpp)
add_library(UML::GpuContextIdleTracker ALIAS uml_gpu_context_idle_tracker)
uml_configure_gpu_component(uml_gpu_context_idle_tracker)

add_library(uml_gpu_renderer STATIC
    host_gl_provider.cpp
    virgl_renderer.cpp)
uml_configure_gpu_component(uml_gpu_renderer)
target_link_libraries(uml_gpu_renderer PUBLIC
    UML::GpuContextIdleTracker)
target_compile_definitions(uml_gpu_renderer PRIVATE
    VIRGL_RENDERER_UNSTABLE_APIS)
target_link_libraries(uml_gpu_renderer PRIVATE
    UML::GpuDeviceMemoryPool
    UML::ThirdPartyVirglrenderer
    UML::HostGpuMemory)
target_link_libraries(uml_gpu_renderer PRIVATE
    "-framework CoreFoundation"
    "-framework Foundation"
    "-framework Metal"
    "-framework QuartzCore")
add_dependencies(uml_gpu_renderer uml_dependency_prefix_verify)

set(_uml_gpu_display_sources
    display_geometry.cpp
    display_resize_notifier.cpp
    display.cpp
    frame_tap.cpp
    metal_presentation.mm
    metal_scanout_shader.mm
    native_display_common.cpp
    scanout_framebuffer.cpp)
# The frame mirror (frame_tap.hpp) copies the guest's display out of the
# process: an App Store runtime has none.
if(UML_APP_STORE_RUNTIME)
    list(REMOVE_ITEM _uml_gpu_display_sources frame_tap.cpp)
endif()
if(UML_TARGET_MACOS)
    list(APPEND _uml_gpu_display_sources
        metal_readback.mm
        native_display.mm
        offscreen_display.mm
        vnc_display.cpp)
else()
    list(APPEND _uml_gpu_display_sources native_display_ios.mm)
endif()
add_library(uml_gpu_display STATIC ${_uml_gpu_display_sources})
uml_configure_gpu_component(uml_gpu_display)
target_compile_definitions(uml_gpu_display PRIVATE
    UML_GPU_HAS_OFFSCREEN=$<BOOL:${UML_TARGET_MACOS}>
    UML_GPU_HAS_VNC=$<BOOL:${UML_TARGET_MACOS}>
    UML_GPU_VNC_STARTUP_TESTING=0)
target_include_directories(uml_gpu_display PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/device/input/vnc)
target_link_libraries(uml_gpu_display PUBLIC
    UML::Descriptor UML::VncInput UML::GpuContextMemoryLedger)
target_link_libraries(uml_gpu_display PRIVATE UML::GuestTimerMask)
if(UML_TARGET_MACOS)
    target_link_libraries(uml_gpu_display PRIVATE
        UML::ThirdPartyVncServer
        "-framework Accelerate"
        "-framework AppKit"
        "-framework CoreVideo"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore")
else()
    target_link_libraries(uml_gpu_display PRIVATE
        "-framework Accelerate"
        "-framework CoreVideo"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
        "-framework UIKit")
endif()
add_dependencies(uml_gpu_display uml_dependency_prefix_verify)

if(UML_BUILD_UNIT_TESTS AND UML_TARGET_MACOS)
    # The VNC display with its testing accessors, for the VNC lifetime tests.
    add_library(uml_gpu_display_test_variant STATIC
        display_geometry.cpp
        metal_readback.mm
        metal_scanout_shader.mm
        scanout_framebuffer.cpp
        vnc_display.cpp)
    uml_configure_gpu_component(uml_gpu_display_test_variant)
    target_compile_definitions(uml_gpu_display_test_variant PUBLIC
        UML_GPU_VNC_STARTUP_TESTING=1)
    target_include_directories(uml_gpu_display_test_variant PUBLIC
        ${PROJECT_SOURCE_DIR}/darwin/device/input/vnc)
    target_link_libraries(uml_gpu_display_test_variant PUBLIC
        UML::VncInput UML::GpuContextMemoryLedger)
    target_link_libraries(uml_gpu_display_test_variant PRIVATE
        UML::ThirdPartyVncServer
        "-framework Accelerate"
        "-framework Foundation"
        "-framework Metal")
    add_dependencies(uml_gpu_display_test_variant
        uml_dependency_prefix_verify)
endif()

add_library(uml_gpu_protocol_device STATIC virtio_gpu_device.cpp)
uml_configure_gpu_component(uml_gpu_protocol_device)
target_link_libraries(uml_gpu_protocol_device PUBLIC
    uml_gpu_renderer
    uml_gpu_display
    UML::GpuMemoryPressure)

# The Virtio-GPU device on the vhost-user transport, with its display.
add_library(uml_gpu_vhost_user_device STATIC
    deferred_fences.cpp
    gpu_vhost_user_server.cpp)
uml_configure_gpu_component(uml_gpu_vhost_user_device)
target_link_libraries(uml_gpu_vhost_user_device PUBLIC
    UML::VhostUserTransport
    uml_gpu_protocol_device
    UML::VncInput)

add_library(uml_gpu_service STATIC in_process_service.cpp)
uml_configure_gpu_component(uml_gpu_service)
target_compile_definitions(uml_gpu_service PRIVATE
    UML_GPU_STARTUP_TESTING=0)
target_link_libraries(uml_gpu_service PRIVATE
    uml_gpu_vhost_user_device
    UML::DeviceGuestEnd)

if(UML_BUILD_UNIT_TESTS)
    # Test variants for the GPU service and device tests. Each recompiles the
    # production sources under a test-only policy; the hooks those policies
    # call are implemented beside the tests.
    add_library(uml_gpu_service_test_variant STATIC
        deferred_fences.cpp
        gpu_vhost_user_server.cpp
        in_process_service.cpp)
    uml_configure_gpu_component(uml_gpu_service_test_variant)
    target_compile_definitions(uml_gpu_service_test_variant PRIVATE
        UML_GPU_STARTUP_TESTING=1)
    target_link_libraries(uml_gpu_service_test_variant PUBLIC
        uml_gpu_transport_startup_test_variant
        uml_gpu_protocol_device
        UML::DeviceGuestEnd
        UML::VncInput)

    add_library(uml_gpu_service_slow_stop_test_variant STATIC
        in_process_service.cpp)
    uml_configure_gpu_component(uml_gpu_service_slow_stop_test_variant)
    target_compile_definitions(uml_gpu_service_slow_stop_test_variant PRIVATE
        UML_GPU_STARTUP_TESTING=0
        UML_GPU_SERVICE_TRANSPORT_TESTING=1)
    target_link_libraries(uml_gpu_service_slow_stop_test_variant PRIVATE
        uml_gpu_transport_slow_stop_test_variant
        uml_gpu_protocol_device
        UML::DeviceGuestEnd)

    # The device with its command-atomicity probes, and the fence queue it
    # completes into.
    add_library(uml_gpu_protocol_device_test_variant STATIC
        deferred_fences.cpp
        virtio_gpu_device.cpp)
    uml_configure_gpu_component(uml_gpu_protocol_device_test_variant)
    target_compile_definitions(uml_gpu_protocol_device_test_variant PUBLIC
        UML_GPU_DEVICE_TESTING=1)
    target_link_libraries(uml_gpu_protocol_device_test_variant PUBLIC
        UML::GpuContextMemoryLedger
        UML::GpuMemoryPressure)
endif()

add_library(uml_gpu_device INTERFACE)
add_library(UML::GpuDevice ALIAS uml_gpu_device)
uml_declare_architecture_target(uml_gpu_device device)
target_link_libraries(uml_gpu_device INTERFACE
    uml_gpu_service)
