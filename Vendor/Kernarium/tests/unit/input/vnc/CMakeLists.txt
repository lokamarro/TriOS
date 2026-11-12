# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_vnc_input_tests vnc_input_tests.cpp)
target_link_libraries(uml_vnc_input_tests PRIVATE UML::VncInput)
target_include_directories(uml_vnc_input_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/device/input/vnc)
uml_add_unit_test(NAME vnc-input-boundary COMMAND uml_vnc_input_tests
    LABELS input)
