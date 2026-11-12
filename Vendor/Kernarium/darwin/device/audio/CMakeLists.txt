# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_audio_device STATIC
    apple_microphone_input.mm
    apple_playback_sink.mm
    audio_session.mm
    in_process_service.cpp
    microphone_capture_source.cpp
    microphone_switch.cpp
    null_playback_sink.cpp
    period_clock.cpp
    silent_capture_source.cpp
    virtio_sound_device.cpp)
add_library(UML::AudioDevice ALIAS uml_audio_device)
uml_declare_architecture_target(uml_audio_device device)
target_include_directories(uml_audio_device
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/device/vhost_user)
target_link_libraries(uml_audio_device
    PUBLIC
        UML::Descriptor
        UML::MicrophoneAuthorization
    PRIVATE
        UML::VhostUserTransport
    "-framework AVFAudio"
    "-framework Foundation")
if(UML_APPLE_PLATFORM STREQUAL "macos")
    # The output's IO cycle is a device property there (OutputCycle).
    target_link_libraries(uml_audio_device PRIVATE
        "-framework AudioToolbox"
        "-framework CoreAudio")
endif()
