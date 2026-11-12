# SPDX-License-Identifier: GPL-2.0-or-later

include("${CMAKE_CURRENT_LIST_DIR}/VerifyIosStaticXCFramework.cmake")

uml_verify_ios_static_xcframework(
    LABEL "iOS storage"
    PRODUCT ios-storage
    FRAMEWORK GPTFDisk
    HEADERS GPTFDisk.h
    SYMBOLS
        kernarium_gptfdisk_read_partition
        kernarium_gptfdisk_grow_partition_to_fill
    FORBIDDEN
        "(^|[\n ])_main([\n ]|$)"
        "GPTData(TextUI|CL|Curses)"
        "[Pp]opt"
    FORBIDDEN_MESSAGE "a GPT fdisk CLI front end")
