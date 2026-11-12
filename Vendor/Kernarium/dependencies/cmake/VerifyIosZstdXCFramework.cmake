# SPDX-License-Identifier: GPL-2.0-or-later

include("${CMAKE_CURRENT_LIST_DIR}/VerifyIosStaticXCFramework.cmake")

uml_verify_ios_static_xcframework(
    LABEL "iOS zstd"
    PRODUCT ios-compression
    FRAMEWORK LibZstd
    HEADERS zstd.h zstd_errors.h
    SYMBOLS
        ZSTD_createDStream
        ZSTD_DStreamInSize
        ZSTD_DStreamOutSize
        ZSTD_decompressStream
        ZSTD_freeDStream
        ZSTD_getErrorName
        ZSTD_initDStream
        ZSTD_isError
        ZSTD_versionNumber
    FORBIDDEN
        "(^|[\n ])_main([\n ]|$)"
        "_ZSTD_compress(Stream|CCtx|2)?([^A-Za-z0-9_]|$)"
        "_ZDICT_trainFromBuffer"
        "_ZSTDv0[1-7]_"
    FORBIDDEN_MESSAGE "a zstd CLI or unused compression API")
