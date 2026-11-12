# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED FRAMEWORK_NAME OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "FRAMEWORK_NAME and OUTPUT are required")
endif()

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../apple-static-framework/Info.plist.in"
    "${OUTPUT}"
    @ONLY)
