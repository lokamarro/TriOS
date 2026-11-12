# SPDX-License-Identifier: GPL-2.0-or-later

# GuestHostCore is what Kernarium and Andarium decide the same way about a
# guest, as a Swift package with no UIKit, so its tests run on the host.
# SwiftPM keeps its products in the build tree, not beside the package.
uml_add_unit_test(NAME guest-host-core
    COMMAND /usr/bin/xcrun swift test
        --package-path ${PROJECT_SOURCE_DIR}/GuestHost/Core
        --scratch-path ${CMAKE_BINARY_DIR}/guest-host-core-swiftpm
    LABELS guesthost
    TIMEOUT 600)
