# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

# The Apple GPU product renders OpenGL with Mesa's Zink Gallium driver on the
# KosmicKrisp Vulkan driver. The iOS Simulator has no Metal 4 device for
# KosmicKrisp, so its prefix is the software variant: Mesa's llvmpipe
# rasterizer (and softpipe) on the CPU, no Vulkan driver and no Venus. The
# platform alone selects it; no cache option does.
set(_uml_apple_gpu_software FALSE)
if(UML_APPLE_PLATFORM STREQUAL "macos")
    set(_uml_apple_gpu_default_prefix
        "${PROJECT_SOURCE_DIR}/.build/dependencies/apple-gpu-vulkan-experiment/macos-arm64")
elseif(UML_APPLE_PLATFORM STREQUAL "ios")
    set(_uml_apple_gpu_default_prefix
        "${PROJECT_SOURCE_DIR}/.build/dependencies/apple-gpu-vulkan-experiment/ios-device-arm64")
elseif(UML_APPLE_PLATFORM STREQUAL "ios-simulator")
    set(_uml_apple_gpu_default_prefix
        "${PROJECT_SOURCE_DIR}/.build/dependencies/apple-gpu-vulkan-experiment/ios-simulator-arm64")
    set(_uml_apple_gpu_software TRUE)
else()
    message(FATAL_ERROR
        "The Apple GPU product does not support ${UML_APPLE_PLATFORM}")
endif()
set(UML_APPLE_GPU_DEPENDENCY_PREFIX
    "${_uml_apple_gpu_default_prefix}"
    CACHE PATH
    "Verified Apple GPU prefix: host GL provider, libepoxy, virglrenderer")

set(_uml_apple_gpu_include "${UML_APPLE_GPU_DEPENDENCY_PREFIX}/include")
set(_uml_apple_gpu_lib "${UML_APPLE_GPU_DEPENDENCY_PREFIX}/lib")
set(_uml_apple_gpu_frameworks
    "${UML_APPLE_GPU_DEPENDENCY_PREFIX}/Frameworks")
set(_uml_apple_gpu_inputs
    "${_uml_apple_gpu_lib}/libepoxy.a"
    "${_uml_apple_gpu_lib}/libvirglrenderer.a"
    "${_uml_apple_gpu_include}/epoxy/egl.h"
    "${_uml_apple_gpu_include}/virgl/virglrenderer.h"
    "${_uml_apple_gpu_include}/virgl/virgl_hw.h")
set(_uml_apple_gl_frameworks MesaEGL MesaGallium)
foreach(_uml_apple_gl_framework IN LISTS _uml_apple_gl_frameworks)
    list(APPEND _uml_apple_gpu_inputs
        "${_uml_apple_gpu_frameworks}/${_uml_apple_gl_framework}.framework/${_uml_apple_gl_framework}")
endforeach()
if(NOT _uml_apple_gpu_software)
    list(APPEND _uml_apple_gpu_inputs
        "${_uml_apple_gpu_include}/vulkan/vulkan.h"
        "${_uml_apple_gpu_include}/vulkan/vulkan_core.h"
        "${PROJECT_SOURCE_DIR}/tools/rootfs/vulkan/patches/venus-protocol/series.tsv"
        "${_uml_apple_gpu_frameworks}/KosmicKrisp.framework/KosmicKrisp"
        "${_uml_apple_gpu_frameworks}/KosmicKrisp.framework/Modules/module.modulemap")
endif()
foreach(_uml_apple_gpu_input IN LISTS _uml_apple_gpu_inputs)
    if(NOT EXISTS "${_uml_apple_gpu_input}")
        message(FATAL_ERROR
            "Missing ${UML_APPLE_PLATFORM} Apple GPU dependency: "
            "${_uml_apple_gpu_input}. Run dependencies/build-apple-gpu.sh "
            "--platform ${UML_APPLE_PLATFORM} first.")
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DPREFIX=${UML_APPLE_GPU_DEPENDENCY_PREFIX}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -DPLATFORM=${UML_APPLE_PLATFORM}
        -P ${CMAKE_CURRENT_LIST_DIR}/VerifyAppleGpuPrefix.cmake
    COMMAND_ERROR_IS_FATAL ANY)

find_package(Threads REQUIRED)
uml_find_sdk_library(_uml_apple_gpu_math m)

# The packaged host GL provider is Mesa's EGL entry point over its Gallium
# library. libepoxy and the renderer dispatch through that pair.
set(_uml_apple_gl_provider_targets)
foreach(_uml_apple_gl_framework IN LISTS _uml_apple_gl_frameworks)
    string(TOLOWER "${_uml_apple_gl_framework}" _uml_apple_gl_lower)
    set(_uml_apple_gl_target "uml_third_party_host_gl_${_uml_apple_gl_lower}")
    add_library(${_uml_apple_gl_target} SHARED IMPORTED GLOBAL)
    set_target_properties(${_uml_apple_gl_target} PROPERTIES
        IMPORTED_LOCATION
            "${_uml_apple_gpu_frameworks}/${_uml_apple_gl_framework}.framework/${_uml_apple_gl_framework}")
    list(APPEND _uml_apple_gl_provider_targets "${_uml_apple_gl_target}")
endforeach()

add_library(uml_third_party_epoxy STATIC IMPORTED GLOBAL)
set_target_properties(uml_third_party_epoxy PROPERTIES
    IMPORTED_LOCATION "${_uml_apple_gpu_lib}/libepoxy.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_uml_apple_gpu_include}"
    INTERFACE_LINK_LIBRARIES
        "${_uml_apple_gl_provider_targets}")
add_library(UML::ThirdPartyEpoxy ALIAS uml_third_party_epoxy)

set(_uml_apple_gpu_virgl_links
    UML::ThirdPartyEpoxy Threads::Threads "${_uml_apple_gpu_math}")
if(NOT _uml_apple_gpu_software)
    add_library(uml_third_party_kosmickrisp SHARED IMPORTED GLOBAL)
    set_target_properties(uml_third_party_kosmickrisp PROPERTIES
        IMPORTED_LOCATION
            "${_uml_apple_gpu_frameworks}/KosmicKrisp.framework/KosmicKrisp"
        INTERFACE_INCLUDE_DIRECTORIES "${_uml_apple_gpu_include}")
    add_library(UML::ThirdPartyKosmicKrisp ALIAS uml_third_party_kosmickrisp)
    list(APPEND _uml_apple_gpu_virgl_links UML::ThirdPartyKosmicKrisp)
endif()
add_library(uml_third_party_virglrenderer STATIC IMPORTED GLOBAL)
set_target_properties(uml_third_party_virglrenderer PROPERTIES
    IMPORTED_LOCATION "${_uml_apple_gpu_lib}/libvirglrenderer.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_uml_apple_gpu_include}/virgl"
    INTERFACE_LINK_LIBRARIES "${_uml_apple_gpu_virgl_links}")
add_library(UML::ThirdPartyVirglrenderer ALIAS
    uml_third_party_virglrenderer)

add_custom_target(uml_apple_gpu_prefix_verify
    COMMAND "${CMAKE_COMMAND}"
        -DPREFIX=${UML_APPLE_GPU_DEPENDENCY_PREFIX}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -DPLATFORM=${UML_APPLE_PLATFORM}
        -P ${CMAKE_CURRENT_LIST_DIR}/VerifyAppleGpuPrefix.cmake
    DEPENDS ${_uml_apple_gpu_inputs}
    COMMENT "Checking the common ${UML_APPLE_PLATFORM} Apple GPU prefix"
    VERBATIM)
# dependencies/ is not added for an iOS build, which then has no aggregate
# verification target yet.
if(NOT TARGET uml_dependency_prefix_verify)
    add_custom_target(uml_dependency_prefix_verify)
endif()
add_dependencies(uml_dependency_prefix_verify uml_apple_gpu_prefix_verify)

set(UML_APPLE_GPU_FRAMEWORK_DIRECTORY
    "${_uml_apple_gpu_frameworks}"
    CACHE INTERNAL "Verified Apple GPU runtime framework directory")
set(UML_APPLE_GPU_GL_FRAMEWORK_NAMES
    "${_uml_apple_gl_frameworks}"
    CACHE INTERNAL "Packaged Apple host GL provider frameworks")
if(_uml_apple_gpu_software)
    set(UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK ""
        CACHE INTERNAL "Verified KosmicKrisp runtime framework")
else()
    set(UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK
        "${_uml_apple_gpu_frameworks}/KosmicKrisp.framework"
        CACHE INTERNAL "Verified KosmicKrisp runtime framework")
endif()
set(UML_GPU_SOFTWARE_RENDERER ${_uml_apple_gpu_software}
    CACHE INTERNAL "The host GL provider is a CPU rasterizer without Vulkan")
