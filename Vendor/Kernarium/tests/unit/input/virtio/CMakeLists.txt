# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_virtio_input_tests virtio_input_tests.cpp)
target_link_libraries(uml_virtio_input_tests PRIVATE UML::VirtioInput)
uml_add_unit_test(NAME virtio-input-protocol COMMAND uml_virtio_input_tests
    LABELS input)
