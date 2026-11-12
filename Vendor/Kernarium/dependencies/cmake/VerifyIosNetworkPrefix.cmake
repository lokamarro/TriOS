# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED PREFIX)
    message(FATAL_ERROR "PREFIX is required for dependency verification")
endif()
if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required for dependency verification")
endif()
if(NOT DEFINED PLATFORM)
    set(PLATFORM ios-simulator)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/VerifyManifestProduct.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/VerifyPrefix.cmake")
if(PLATFORM STREQUAL "ios-simulator")
    set(_platform_receipt ios-simulator-network-arm64.tsv)
    set(_mach_platform 7)
elseif(PLATFORM STREQUAL "ios")
    set(_platform_receipt ios-device-network-arm64.tsv)
    set(_mach_platform 2)
else()
    message(FATAL_ERROR "Unsupported iOS network platform: ${PLATFORM}")
endif()

set(_required
    "${PREFIX}/lib/libpcre2-8.a"
    "${PREFIX}/lib/libffi.a"
    "${PREFIX}/lib/libintl.a"
    "${PREFIX}/lib/libglib-2.0.a"
    "${PREFIX}/lib/libslirp.a"
    "${PREFIX}/include/pcre2.h"
    "${PREFIX}/include/ffi.h"
    "${PREFIX}/include/glib-2.0/glib.h"
    "${PREFIX}/lib/glib-2.0/include/glibconfig.h"
    "${PREFIX}/include/slirp/libslirp.h")
uml_require_prefix_files(LABEL "iOS network" FILES ${_required})

uml_verify_dependency_manifest_product(
    SOURCE "${SOURCE_ROOT}/dependencies/manifest.tsv"
    INSTALLED "${PREFIX}/share/uml-dependencies/manifest.tsv"
    PRODUCT ios-network
    LABEL "iOS network")
uml_verify_source_trees(
    INSTALLED "${PREFIX}/share/uml-dependencies/source-trees.tsv"
    LABEL "iOS network"
    SOURCE_ROOT "${SOURCE_ROOT}"
    PRODUCT ios-network)
uml_verify_provenance_copy(
    LABEL "iOS network"
    SOURCE "${SOURCE_ROOT}/dependencies/platforms/${_platform_receipt}"
    INSTALLED "${PREFIX}/share/uml-dependencies/${_platform_receipt}")

foreach(_archive IN ITEMS
        libpcre2-8.a libffi.a libintl.a libglib-2.0.a libslirp.a)
    uml_verify_apple_binary(PATH "${PREFIX}/lib/${_archive}"
        PLATFORM ${_mach_platform} MINOS 27.0)
endforeach()
