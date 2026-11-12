# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_platform_contract INTERFACE)
add_library(UML::PlatformContract ALIAS uml_platform_contract)
uml_declare_architecture_target(uml_platform_contract platform)

# Header-only: blocks the guest timer around host thread creation.
add_library(uml_guest_timer_mask INTERFACE)
add_library(UML::GuestTimerMask ALIAS uml_guest_timer_mask)
uml_declare_architecture_target(uml_guest_timer_mask platform)
target_include_directories(uml_guest_timer_mask INTERFACE
    ${PROJECT_SOURCE_DIR}/darwin)

# Header-only: the host GPU's fixed budget, evaluated once at launch.
add_library(uml_gpu_budget INTERFACE)
add_library(UML::GpuBudget ALIAS uml_gpu_budget)
uml_declare_architecture_target(uml_gpu_budget platform)
target_include_directories(uml_gpu_budget INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR})

# Header-only: host file-descriptor ownership.
add_library(uml_descriptor INTERFACE)
add_library(UML::Descriptor ALIAS uml_descriptor)
uml_declare_architecture_target(uml_descriptor platform)
target_include_directories(uml_descriptor INTERFACE
    ${PROJECT_SOURCE_DIR}/darwin)

add_library(uml_video_decoder STATIC apple/video_decoder.mm apple/hevc_stream.cpp apple/h264_stream.cpp)
add_library(UML::VideoDecoder ALIAS uml_video_decoder)
uml_declare_architecture_target(uml_video_decoder platform)
target_include_directories(uml_video_decoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_video_decoder PRIVATE UML::PlatformContract
    "-framework Foundation" "-framework CoreMedia" "-framework CoreVideo"
    "-framework VideoToolbox")

# The hardware video encoder: VideoToolbox discovery, sessions and Annex B
# output behind darwin/platform/video_encoder.hpp.
set(UML_VIDEO_ENCODER_SOURCES
    apple/video_encoder.mm
    apple/video_encoder_discovery.mm)
add_library(uml_video_encoder STATIC ${UML_VIDEO_ENCODER_SOURCES})
add_library(UML::VideoEncoder ALIAS uml_video_encoder)
uml_declare_architecture_target(uml_video_encoder platform)
target_include_directories(uml_video_encoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_video_encoder PRIVATE UML::PlatformContract
    "-framework Accelerate" "-framework Foundation" "-framework CoreMedia"
    "-framework CoreVideo" "-framework VideoToolbox")
if(UML_BUILD_UNIT_TESTS)
    # The same sources with the session-loss injection the recovery test
    # drives; products compile them without it.
    add_library(uml_video_encoder_test_variant STATIC
        ${UML_VIDEO_ENCODER_SOURCES})
    uml_declare_architecture_target(uml_video_encoder_test_variant platform)
    target_include_directories(uml_video_encoder_test_variant PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(uml_video_encoder_test_variant PUBLIC
        UML_VIDEO_ENCODER_TESTING=1)
    target_link_libraries(uml_video_encoder_test_variant PRIVATE
        UML::PlatformContract
        "-framework Accelerate" "-framework Foundation" "-framework CoreMedia"
        "-framework CoreVideo" "-framework VideoToolbox")
endif()

add_library(uml_host_battery STATIC apple/host_battery.mm)
add_library(UML::HostBattery ALIAS uml_host_battery)
uml_declare_architecture_target(uml_host_battery platform)
target_include_directories(uml_host_battery PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_host_battery PRIVATE
    UML::PlatformContract
    "-framework Foundation")
if(UML_TARGET_MACOS)
    target_link_libraries(uml_host_battery PRIVATE "-framework IOKit")
else()
    target_link_libraries(uml_host_battery PRIVATE "-framework UIKit")
endif()

# The system's microphone permission. Reading it never prompts.
add_library(uml_microphone_authorization STATIC
    apple/microphone_authorization.mm)
add_library(UML::MicrophoneAuthorization ALIAS uml_microphone_authorization)
uml_declare_architecture_target(uml_microphone_authorization platform)
target_include_directories(uml_microphone_authorization PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_microphone_authorization PRIVATE
    UML::PlatformContract
    "-framework AVFAudio"
    "-framework Foundation")

add_library(uml_host_gpu_memory STATIC apple/host_gpu_memory.mm)
add_library(UML::HostGpuMemory ALIAS uml_host_gpu_memory)
uml_declare_architecture_target(uml_host_gpu_memory platform)
target_include_directories(uml_host_gpu_memory PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_host_gpu_memory PRIVATE
    UML::PlatformContract
    objc
    "-framework Metal")

add_library(uml_run_routing OBJECT run_routing.cpp)
add_library(UML::RunRouting ALIAS uml_run_routing)
uml_declare_architecture_target(uml_run_routing platform)
target_include_directories(uml_run_routing PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(uml_run_routing PRIVATE
    UML_RUN_ROUTING_TESTING=0)
if(UML_BUILD_UNIT_TESTS)
    # The same source with its failure-injection points, for the routing
    # transaction tests; products compile it without them.
    add_library(uml_run_routing_test_variant OBJECT run_routing.cpp)
    uml_declare_architecture_target(uml_run_routing_test_variant platform)
    target_include_directories(uml_run_routing_test_variant PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(uml_run_routing_test_variant PRIVATE
        UML_RUN_ROUTING_TESTING=1)
endif()

add_library(uml_host_topology OBJECT host_topology.cpp)
add_library(UML::HostTopology ALIAS uml_host_topology)
uml_declare_architecture_target(uml_host_topology platform)
target_link_libraries(uml_host_topology PRIVATE UML::PlatformContract)
target_include_directories(uml_host_topology PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

add_library(uml_host_environment OBJECT apple/host_environment.mm)
add_library(UML::HostEnvironment ALIAS uml_host_environment)
uml_declare_architecture_target(uml_host_environment platform)
target_link_libraries(uml_host_environment PRIVATE
    UML::PlatformContract)
target_include_directories(uml_host_environment PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

add_library(uml_guest_files OBJECT guest_files.cpp)
add_library(UML::GuestFiles ALIAS uml_guest_files)
uml_declare_architecture_target(uml_guest_files platform)
target_link_libraries(uml_guest_files PRIVATE UML::PlatformContract)
target_include_directories(uml_guest_files PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
if(UML_BUILD_UNIT_TESTS)
    # The same source under the iOS family's disk-lock policy, which takes no
    # host file lock, for its tests on a macOS host.
    add_library(uml_guest_files_process_locks OBJECT guest_files.cpp)
    uml_declare_architecture_target(uml_guest_files_process_locks platform)
    target_link_libraries(uml_guest_files_process_locks PRIVATE
        UML::PlatformContract)
    target_include_directories(uml_guest_files_process_locks PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(uml_guest_files_process_locks PRIVATE
        UML_GUEST_FILES_PROCESS_LOCKS=1)
endif()

add_library(uml_platform_component INTERFACE)
add_library(UML::PlatformComponent ALIAS uml_platform_component)
uml_declare_architecture_target(uml_platform_component platform)
target_link_libraries(uml_platform_component INTERFACE
    UML::PlatformContract
    "-framework Foundation")
target_sources(uml_platform_component INTERFACE
    $<TARGET_OBJECTS:uml_run_routing>
    $<TARGET_OBJECTS:uml_host_topology>
    $<TARGET_OBJECTS:uml_host_environment>
    $<TARGET_OBJECTS:uml_guest_files>)
