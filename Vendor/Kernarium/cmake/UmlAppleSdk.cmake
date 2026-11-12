# SPDX-License-Identifier: GPL-2.0-or-later
#
# The Apple SDK a tree is configured against. cmake/toolchains/Apple.cmake
# asks `xcrun` for it on every configure, so a configure always sees the SDK
# that is installed now; this module makes the build do the same.

include_guard(GLOBAL)

# uml_track_apple_sdk()
# Makes the selected SDK's SDKSettings.json a configure dependency. An Xcode
# update either rewrites that file or moves the SDK to another path, which
# removes it; both make the build rerun configure before it compiles or
# links, instead of failing at a link input of the old SDK.
function(uml_track_apple_sdk)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_OSX_SYSROOT}/SDKSettings.json")
endfunction()

# uml_find_sdk_library(<result> <name>) and uml_find_sdk_framework(<result>
# <name>) set <result> to a library or framework of the selected SDK.
# find_library would cache the path of the SDK it found first, and a tree
# configured again against a newer SDK would keep linking the old one's
# files, so the cached entry is dropped and the SDK is searched every time.
function(uml_find_sdk_library result name)
    unset(${result} CACHE)
    find_library(${result}
        NAMES "${name}"
        PATHS "${CMAKE_OSX_SYSROOT}/usr/lib"
        NO_DEFAULT_PATH REQUIRED)
    set(${result} "${${result}}" PARENT_SCOPE)
endfunction()

function(uml_find_sdk_framework result name)
    unset(${result} CACHE)
    find_library(${result}
        NAMES "${name}"
        PATHS "${CMAKE_OSX_SYSROOT}/System/Library/Frameworks"
        NO_DEFAULT_PATH REQUIRED)
    set(${result} "${${result}}" PARENT_SCOPE)
endfunction()
