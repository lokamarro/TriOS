# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_virtio_battery_tests virtio_battery_tests.cpp)
target_link_libraries(uml_virtio_battery_tests PRIVATE UML::BatteryDevice)
uml_add_unit_test(NAME virtio-battery-protocol COMMAND uml_virtio_battery_tests
    LABELS "device;battery")
