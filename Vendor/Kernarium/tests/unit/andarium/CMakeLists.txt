# SPDX-License-Identifier: GPL-2.0-or-later

# AndariumCore is the Andarium application's model as a Swift package with no
# UIKit, so its tests run on the host. SwiftPM keeps its products in the build
# tree, not beside the package.
uml_add_unit_test(NAME andarium-core
    COMMAND /usr/bin/xcrun swift test
        --package-path ${PROJECT_SOURCE_DIR}/Andarium/AndariumCore
        --scratch-path ${CMAKE_BINARY_DIR}/andarium-core-swiftpm
    LABELS andarium
    TIMEOUT 1200)

add_executable(uml_andarium_motion_protocol_tests motion_protocol_tests.cpp)
target_include_directories(uml_andarium_motion_protocol_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/sensors)
uml_add_unit_test(NAME andarium-motion-protocol
    COMMAND uml_andarium_motion_protocol_tests LABELS andarium)
