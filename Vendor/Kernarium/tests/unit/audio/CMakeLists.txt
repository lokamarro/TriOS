# SPDX-License-Identifier: GPL-2.0-or-later

# The mute clock is a header, so its test needs no audio device.
add_executable(uml_audio_mute_clock_tests mute_clock_tests.cpp)
target_include_directories(uml_audio_mute_clock_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/device/audio)
uml_add_unit_test(NAME audio-mute-clock COMMAND uml_audio_mute_clock_tests
    LABELS audio)

# So is the output's IO cycle for a guest stream.
add_executable(uml_audio_output_cycle_tests output_cycle_tests.cpp)
target_include_directories(uml_audio_output_cycle_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/device/audio)
uml_add_unit_test(NAME audio-output-cycle COMMAND uml_audio_output_cycle_tests
    LABELS audio)

if(TARGET uml_audio_device)
    add_executable(uml_virtio_sound_tests virtio_sound_tests.cpp)
    target_include_directories(uml_virtio_sound_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(uml_virtio_sound_tests PRIVATE UML::AudioDevice)
    uml_add_unit_test(NAME virtio-sound-protocol COMMAND uml_virtio_sound_tests
        LABELS audio)

    # The Microphone setting, the capture source behind it and the ring
    # between the audio thread and the device, all over fakes: nothing here
    # opens the microphone or can show a permission prompt.
    add_executable(uml_virtio_sound_microphone_tests microphone_tests.cpp)
    target_include_directories(uml_virtio_sound_microphone_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(uml_virtio_sound_microphone_tests PRIVATE
        UML::AudioDevice)
    uml_add_unit_test(NAME virtio-sound-microphone
        COMMAND uml_virtio_sound_microphone_tests LABELS audio)
endif()
