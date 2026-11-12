# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_vnc_input STATIC vnc_input.cpp)
add_library(UML::VncInput ALIAS uml_vnc_input)
uml_declare_architecture_target(uml_vnc_input device)
target_include_directories(uml_vnc_input
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
