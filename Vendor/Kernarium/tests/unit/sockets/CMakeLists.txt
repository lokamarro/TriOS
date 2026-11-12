# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_socket_path_tests socket_path_tests.cpp)
target_link_libraries(uml_socket_path_tests PRIVATE UML::DeviceSocketPaths)
uml_add_unit_test(NAME device-socket-path-lifetime
    COMMAND uml_socket_path_tests
    LABELS console)
