# SPDX-License-Identifier: GPL-2.0-or-later
#
# The registry through which a device learns that its guest is ending, shared
# by the devices that hold guest requests and by the runtime that receives the
# kernel's report. A static library keeps the table a single object in every
# final link, as for the socket paths.

add_library(uml_device_guest_end STATIC guest_end.cpp)
add_library(UML::DeviceGuestEnd ALIAS uml_device_guest_end)
uml_declare_architecture_target(uml_device_guest_end device)
target_include_directories(uml_device_guest_end PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
