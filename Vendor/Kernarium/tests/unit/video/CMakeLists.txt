# SPDX-License-Identifier: GPL-2.0-or-later
add_executable(uml_virtio_video_tests virtio_video_tests.cpp)
target_link_libraries(uml_virtio_video_tests PRIVATE UML::VideoDevice)
uml_add_unit_test(NAME virtio-video-protocol COMMAND uml_virtio_video_tests LABELS video)

add_executable(uml_video_encoder_capabilities_tests video_encoder_capabilities_tests.cpp)
target_link_libraries(uml_video_encoder_capabilities_tests PRIVATE UML::VideoDevice UML::VideoEncoder)
uml_add_unit_test(NAME video-encoder-capabilities COMMAND uml_video_encoder_capabilities_tests LABELS video)

add_executable(uml_virtio_video_encoder_tests virtio_video_encoder_tests.cpp)
target_link_libraries(uml_virtio_video_encoder_tests PRIVATE UML::VideoDevice UML::TestSyntheticEncoder UML::VideoEncoder)
uml_add_unit_test(NAME virtio-video-encoder-protocol COMMAND uml_virtio_video_encoder_tests LABELS video TIMEOUT 300)

# These two also run the host's own hardware encoder, and the system limits
# hardware encoder sessions across every process, so they take turns with the
# platform encoder tests.
set_tests_properties(video-encoder-capabilities virtio-video-encoder-protocol PROPERTIES RESOURCE_LOCK video-encoder-hardware)
