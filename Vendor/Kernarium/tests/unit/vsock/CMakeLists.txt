# SPDX-License-Identifier: GPL-2.0-or-later

# The device is driven through its queues with synthetic packets and real
# socketpairs; no guest is involved.
add_executable(uml_vsock_connection_table_tests connection_table_tests.cpp)
target_link_libraries(uml_vsock_connection_table_tests PRIVATE UML::VsockDevice)
uml_add_unit_test(NAME virtio-vsock-connection-table
    COMMAND uml_vsock_connection_table_tests
    LABELS "device;vsock")

add_executable(uml_vsock_device_tests virtio_vsock_device_tests.cpp)
target_link_libraries(uml_vsock_device_tests PRIVATE UML::VsockDevice)
uml_add_unit_test(NAME virtio-vsock-device
    COMMAND uml_vsock_device_tests
    LABELS "device;vsock")

add_executable(uml_vsock_forwarder_tests forwarder_tests.cpp)
target_link_libraries(uml_vsock_forwarder_tests PRIVATE UML::VsockDevice)
uml_add_unit_test(NAME vsock-forwarder
    COMMAND uml_vsock_forwarder_tests
    LABELS "device;vsock")
