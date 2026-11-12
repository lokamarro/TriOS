# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_battery_device STATIC
    in_process_service.cpp
    virtio_battery_device.cpp)
add_library(UML::BatteryDevice ALIAS uml_battery_device)
uml_declare_architecture_target(uml_battery_device device)
target_include_directories(uml_battery_device
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/device/vhost_user
        ${PROJECT_SOURCE_DIR}/darwin/platform)
target_link_libraries(uml_battery_device PRIVATE
    UML::VhostUserTransport
    UML::HostBattery)
