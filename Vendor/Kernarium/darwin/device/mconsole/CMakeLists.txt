# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_device_guest_console OBJECT guest_console.cpp)
add_library(UML::DeviceGuestConsole ALIAS uml_device_guest_console)
uml_declare_architecture_target(uml_device_guest_console device)
target_link_libraries(uml_device_guest_console PUBLIC UML::DeviceSocketPaths)
target_include_directories(uml_device_guest_console
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${PROJECT_SOURCE_DIR}/darwin/device)
