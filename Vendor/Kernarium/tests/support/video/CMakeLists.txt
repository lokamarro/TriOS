# SPDX-License-Identifier: GPL-2.0-or-later
# Test-only video support; see README.md.
add_library(uml_test_synthetic_encoder INTERFACE)
add_library(UML::TestSyntheticEncoder ALIAS uml_test_synthetic_encoder)
target_include_directories(uml_test_synthetic_encoder INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uml_test_synthetic_encoder INTERFACE UML::VideoEncoder)
