# SPDX-License-Identifier: GPL-2.0-or-later

# Keep every registration in this directory scope so its default working
# directory and the manifest-visible CTest order remain stable. The final
# three modules own conditional fixtures that intentionally register last.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/ToolingTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/kernel/ToolingTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/android/ToolingTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/abi/ToolingTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/dependencies/ToolingTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/ltp/ToolingTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/rootfs/ToolingTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/gpu/ToolingTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/network/ToolingTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/performance/ToolingTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/support/ToolingTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/arm64/ToolingTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/kernarium/ToolingTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/andarium/ToolingTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/x86_64/ToolingTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/network/SlirpToolingTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/emulator/ToolingTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/gpu/VncToolingTests.cmake")
