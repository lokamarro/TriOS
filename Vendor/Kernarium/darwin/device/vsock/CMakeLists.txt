# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_vsock_device STATIC
    connection_table.cpp
    endpoint.cpp
    forwarder.cpp
    host_events.cpp
    in_process_service.cpp
    virtio_vsock_device.cpp)
add_library(UML::VsockDevice ALIAS uml_vsock_device)
uml_declare_architecture_target(uml_vsock_device device)
target_include_directories(uml_vsock_device
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/device/vhost_user
        ${PROJECT_SOURCE_DIR}/darwin/platform)
target_link_libraries(uml_vsock_device PRIVATE UML::VhostUserTransport)
