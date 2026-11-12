# SPDX-License-Identifier: GPL-2.0-or-later
# Test-only vhost-user device wrappers; see README.md.
add_library(uml_delayed_answer_device STATIC delayed_answer_device.cpp)
add_library(UML::DelayedAnswerDevice ALIAS uml_delayed_answer_device)
target_include_directories(uml_delayed_answer_device PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_delayed_answer_device PUBLIC
    UML::VhostUserTransport)
