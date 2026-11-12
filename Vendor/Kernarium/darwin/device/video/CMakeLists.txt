# SPDX-License-Identifier: GPL-2.0-or-later
add_library(uml_video_device STATIC
    encoder_capabilities.cpp
    encoder_controls.cpp
    guest_buffers.cpp
    in_process_service.cpp
    video_capabilities.cpp
    virtio_video_device.cpp
    virtio_video_encoder_device.cpp)
add_library(UML::VideoDevice ALIAS uml_video_device)
uml_declare_architecture_target(uml_video_device device)
target_include_directories(uml_video_device PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/kernel/common/video
    ${PROJECT_SOURCE_DIR}/darwin/device/vhost_user
    ${PROJECT_SOURCE_DIR}/darwin/platform)
target_link_libraries(uml_video_device PRIVATE UML::VhostUserTransport UML::VideoDecoder UML::VideoEncoder)
