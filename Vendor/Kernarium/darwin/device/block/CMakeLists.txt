# SPDX-License-Identifier: GPL-2.0-or-later

# The rule for naming a host file in a UBD kernel argument, shared by every
# product that composes one.
add_library(uml_device_ubd_path INTERFACE)
add_library(UML::DeviceUbdPath ALIAS uml_device_ubd_path)
uml_declare_architecture_target(uml_device_ubd_path device)
target_include_directories(uml_device_ubd_path INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR})

add_library(uml_android_boot STATIC
    android_boot.cpp
    android_data_disk.cpp
    android_gpt.cpp
    android_layout.cpp)
add_library(UML::AndroidBoot ALIAS uml_android_boot)
uml_declare_architecture_target(uml_android_boot device)
target_include_directories(uml_android_boot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_android_boot PUBLIC UML::DeviceUbdPath)

add_library(uml_boot_initramfs STATIC boot_initramfs.cpp)
add_library(UML::BootInitramfs ALIAS uml_boot_initramfs)
uml_declare_architecture_target(uml_boot_initramfs device)
target_include_directories(uml_boot_initramfs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
