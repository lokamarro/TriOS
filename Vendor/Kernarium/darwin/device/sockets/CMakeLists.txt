# SPDX-License-Identifier: GPL-2.0-or-later
#
# Filesystem socket-path ownership, shared by the devices that bind or adopt
# one and by the application that must remove them on a terminating signal.
# A static library keeps the published slot table a single object in every
# final link.

add_library(uml_device_socket_paths STATIC socket_paths.cpp)
add_library(UML::DeviceSocketPaths ALIAS uml_device_socket_paths)
uml_declare_architecture_target(uml_device_socket_paths device)
target_include_directories(uml_device_socket_paths PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
