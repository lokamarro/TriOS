# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

if(UML_APPLE_PLATFORM STREQUAL "ios")
    set(_uml_ios_network_default_prefix
        "${PROJECT_SOURCE_DIR}/.build/dependencies/ios-device-arm64/network")
else()
    set(_uml_ios_network_default_prefix
        "${PROJECT_SOURCE_DIR}/.build/dependencies/ios-simulator-arm64/network")
endif()
set(UML_IOS_NETWORK_DEPENDENCY_PREFIX
    "${_uml_ios_network_default_prefix}"
    CACHE PATH
    "Verified GLib/libslirp prefix for Kernarium")

set(_uml_ios_network_include "${UML_IOS_NETWORK_DEPENDENCY_PREFIX}/include")
set(_uml_ios_network_lib "${UML_IOS_NETWORK_DEPENDENCY_PREFIX}/lib")
set(_uml_ios_network_inputs
    "${_uml_ios_network_lib}/libpcre2-8.a"
    "${_uml_ios_network_lib}/libffi.a"
    "${_uml_ios_network_lib}/libintl.a"
    "${_uml_ios_network_lib}/libglib-2.0.a"
    "${_uml_ios_network_lib}/libslirp.a"
    "${_uml_ios_network_include}/glib-2.0/glib.h"
    "${_uml_ios_network_include}/slirp/libslirp.h")
foreach(_uml_ios_network_input IN LISTS _uml_ios_network_inputs)
    if(NOT EXISTS "${_uml_ios_network_input}")
        message(FATAL_ERROR
            "Missing Kernarium iOS network dependency: "
            "${_uml_ios_network_input}. Run "
            "dependencies/build-ios-network.sh first.")
    endif()
endforeach()

find_package(Threads REQUIRED)
include("${CMAKE_CURRENT_LIST_DIR}/UmlThirdPartyTargets.cmake")
foreach(_uml_sdk_library IN ITEMS m resolv iconv)
    uml_find_sdk_library(_uml_sdk_${_uml_sdk_library} ${_uml_sdk_library})
endforeach()
foreach(_uml_framework IN ITEMS Foundation CoreFoundation)
    uml_find_sdk_framework(_uml_sdk_${_uml_framework} ${_uml_framework})
endforeach()

_uml_import_static_dependency(pcre2 "${_uml_ios_network_lib}/libpcre2-8.a"
    ALIAS UML::ThirdPartyPcre2
    INCLUDES "${_uml_ios_network_include}"
    DEFINITIONS PCRE2_STATIC
    LINKS Threads::Threads)

_uml_import_static_dependency(intl "${_uml_ios_network_lib}/libintl.a"
    ALIAS UML::ThirdPartyIntl
    INCLUDES "${_uml_ios_network_include}"
    DEFINITIONS G_INTL_STATIC_COMPILATION)

# iOS GLib uses the SDK's libiconv and needs no AppKit or Carbon.
_uml_import_static_dependency(glib "${_uml_ios_network_lib}/libglib-2.0.a"
    ALIAS UML::ThirdPartyGlib
    INCLUDES
        "${_uml_ios_network_include}/glib-2.0"
        "${_uml_ios_network_lib}/glib-2.0/include"
    LINKS
        UML::ThirdPartyPcre2
        UML::ThirdPartyIntl
        Threads::Threads
        "${_uml_sdk_iconv}"
        "${_uml_sdk_m}"
        "${_uml_sdk_Foundation}"
        "${_uml_sdk_CoreFoundation}")

_uml_import_static_dependency(slirp "${_uml_ios_network_lib}/libslirp.a"
    ALIAS UML::ThirdPartySlirp
    INCLUDES "${_uml_ios_network_include}/slirp"
    LINKS UML::ThirdPartyGlib "${_uml_sdk_resolv}")

add_custom_target(uml_ios_network_prefix_verify
    COMMAND "${CMAKE_COMMAND}"
        -DPREFIX=${UML_IOS_NETWORK_DEPENDENCY_PREFIX}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -DPLATFORM=${UML_APPLE_PLATFORM}
        -P ${CMAKE_CURRENT_LIST_DIR}/VerifyIosNetworkPrefix.cmake
    DEPENDS ${_uml_ios_network_inputs}
    COMMENT "Checking the pinned Kernarium iOS network dependency prefix"
    VERBATIM)
