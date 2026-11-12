# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED PREFIX)
    message(FATAL_ERROR "PREFIX is required for dependency verification")
endif()
if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required for dependency verification")
endif()
if(NOT DEFINED PLATFORM)
    set(PLATFORM macos)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/VerifyManifestProduct.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/VerifyPrefix.cmake")

if(PLATFORM STREQUAL "macos")
    set(_platform_contract macos-gpu-vulkan-arm64.tsv)
    set(_mach_platform 1)
    set(_minimum_os_key LSMinimumSystemVersion)
elseif(PLATFORM STREQUAL "ios")
    set(_platform_contract ios-device-gpu-vulkan-arm64.tsv)
    set(_mach_platform 2)
    set(_minimum_os_key MinimumOSVersion)
elseif(PLATFORM STREQUAL "ios-simulator")
    # KosmicKrisp is a Metal 4 driver and the iOS Simulator exposes no Metal 4
    # device, so the Simulator prefix is the software variant: Mesa's llvmpipe
    # and softpipe rasterizers on the CPU, with no Vulkan driver and no Venus.
    set(_platform_contract ios-simulator-gpu-software-arm64.tsv)
    set(_mach_platform 7)
    set(_minimum_os_key MinimumOSVersion)
else()
    message(FATAL_ERROR "Unsupported Apple GPU platform: ${PLATFORM}")
endif()
set(_deployment_target 27.0)
if(PLATFORM STREQUAL "ios-simulator")
    set(_software TRUE)
else()
    set(_software FALSE)
endif()

if(NOT IS_DIRECTORY "${PREFIX}" OR IS_SYMLINK "${PREFIX}")
    message(FATAL_ERROR "Invalid Apple GPU dependency prefix: ${PREFIX}")
endif()

foreach(_tool IN ITEMS ar lipo nm otool)
    execute_process(
        COMMAND xcrun --find "${_tool}"
        RESULT_VARIABLE _tool_status
        OUTPUT_VARIABLE _tool_path
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _tool_status EQUAL 0 OR NOT EXISTS "${_tool_path}")
        message(FATAL_ERROR "Xcode tool is unavailable: ${_tool}")
    endif()
    set(_tool_${_tool} "${_tool_path}")
endforeach()

uml_verify_provenance_copy(
    LABEL "Apple GPU"
    SOURCE "${SOURCE_ROOT}/dependencies/platforms/${_platform_contract}"
    INSTALLED "${PREFIX}/share/uml-dependencies/${_platform_contract}")
if(_software)
    # llvmpipe is built against the LLVM release this archive manifest pins.
    uml_verify_provenance_copy(
        LABEL "Apple GPU LLVM"
        SOURCE "${SOURCE_ROOT}/dependencies/llvm/archives.tsv"
        INSTALLED "${PREFIX}/share/uml-dependencies/llvm-archives.tsv")
endif()

uml_verify_dependency_manifest_product(
    SOURCE "${SOURCE_ROOT}/dependencies/manifest.tsv"
    INSTALLED "${PREFIX}/share/uml-dependencies/manifest.tsv"
    PRODUCT apple-gpu-vulkan-experiment
    LABEL "Apple GPU")
uml_verify_source_trees(
    INSTALLED "${PREFIX}/share/uml-dependencies/source-trees.tsv"
    LABEL "Apple GPU"
    SOURCE_ROOT "${SOURCE_ROOT}"
    PRODUCT apple-gpu-vulkan-experiment)

file(GLOB_RECURSE _prefix_entries LIST_DIRECTORIES true "${PREFIX}/*")
foreach(_entry IN LISTS _prefix_entries)
    if(IS_SYMLINK "${_entry}")
        message(FATAL_ERROR "Symlink in Apple GPU dependency prefix: ${_entry}")
    endif()
endforeach()

file(GLOB _top_level RELATIVE "${PREFIX}" "${PREFIX}/*")
list(SORT _top_level)
set(_expected_top_level Frameworks include lib share)
# Older prefixes remain usable by the host. Application packaging requires
# the dSYMs that new builds publish, and any supplied symbols must match.
if(IS_DIRECTORY "${PREFIX}/dSYMs")
    list(APPEND _expected_top_level dSYMs)
    list(SORT _expected_top_level)
endif()
if(NOT _top_level STREQUAL _expected_top_level)
    message(FATAL_ERROR "Unexpected Apple GPU top-level payload: ${_top_level}")
endif()

file(GLOB _frameworks RELATIVE "${PREFIX}/Frameworks"
    "${PREFIX}/Frameworks/*")
list(SORT _frameworks)
set(_expected_frameworks MesaEGL.framework MesaGallium.framework)
if(NOT _software)
    list(APPEND _expected_frameworks KosmicKrisp.framework)
endif()
list(SORT _expected_frameworks)
if(NOT _frameworks STREQUAL _expected_frameworks)
    message(FATAL_ERROR "Unexpected Apple GPU frameworks: ${_frameworks}")
endif()

if(IS_DIRECTORY "${PREFIX}/dSYMs")
    file(GLOB _symbols RELATIVE "${PREFIX}/dSYMs" "${PREFIX}/dSYMs/*")
    set(_expected_symbols)
    foreach(_framework IN LISTS _expected_frameworks)
        list(APPEND _expected_symbols "${_framework}.dSYM")
        get_filename_component(_name "${_framework}" NAME_WE)
        foreach(_kind IN ITEMS binary symbols)
            if(_kind STREQUAL "binary")
                set(_path "${PREFIX}/Frameworks/${_framework}/${_name}")
            else()
                set(_path "${PREFIX}/dSYMs/${_framework}.dSYM")
            endif()
            execute_process(COMMAND xcrun dwarfdump --uuid "${_path}"
                RESULT_VARIABLE _uuid_status OUTPUT_VARIABLE _uuid_output
                ERROR_QUIET TIMEOUT 30)
            if(NOT _uuid_status EQUAL 0 OR NOT _uuid_output MATCHES
               "UUID: ([0-9A-Fa-f-]+) \\(arm64\\)")
                message(FATAL_ERROR "Missing debug symbol UUID: ${_path}")
            endif()
            set(_uuid_${_kind} "${CMAKE_MATCH_1}")
        endforeach()
        if(NOT _uuid_binary STREQUAL _uuid_symbols)
            message(FATAL_ERROR "${_framework} debug symbol UUID does not match")
        endif()
        # dsymutil exits 0 for a binary built without debug information and
        # writes a dSYM with the binary's UUID and no compile unit, so the UUID
        # alone does not show that the symbols are any use.
        execute_process(COMMAND xcrun otool -l
            "${PREFIX}/dSYMs/${_framework}.dSYM/Contents/Resources/DWARF/${_name}"
            RESULT_VARIABLE _sections_status OUTPUT_VARIABLE _sections_output
            ERROR_QUIET TIMEOUT 60)
        if(NOT _sections_status EQUAL 0)
            message(FATAL_ERROR "Cannot read the debug symbols of ${_framework}")
        endif()
        if(NOT _sections_output MATCHES
           "sectname __debug_info\n[ ]*segname __DWARF\n[ ]*addr 0x[0-9A-Fa-f]+\n[ ]*size 0x([0-9A-Fa-f]+)\n"
           OR NOT "${CMAKE_MATCH_1}" MATCHES "[1-9A-Fa-f]")
            message(FATAL_ERROR
                "${_framework} debug symbols carry no debug information")
        endif()
    endforeach()
    list(SORT _symbols)
    list(SORT _expected_symbols)
    if(NOT _symbols STREQUAL _expected_symbols)
        message(FATAL_ERROR "Unexpected Apple GPU debug symbols: ${_symbols}")
    endif()
endif()

file(GLOB_RECURSE _library_files LIST_DIRECTORIES false
    RELATIVE "${PREFIX}/lib" "${PREFIX}/lib/*")
list(SORT _library_files)
set(_expected_library_files
    libepoxy.a libvirglrenderer.a)
list(SORT _expected_library_files)
if(NOT _library_files STREQUAL _expected_library_files)
    message(FATAL_ERROR "Unexpected Apple GPU library payload: ${_library_files}")
endif()

file(GLOB _include_roots RELATIVE "${PREFIX}/include" "${PREFIX}/include/*")
list(SORT _include_roots)
set(_expected_include_roots EGL GL KHR epoxy virgl)
if(NOT _software)
    list(APPEND _expected_include_roots vk_video vulkan)
endif()
list(SORT _expected_include_roots)
if(NOT _include_roots STREQUAL _expected_include_roots)
    message(FATAL_ERROR "Unexpected Apple GPU include payload: ${_include_roots}")
endif()

file(GLOB_RECURSE _installed_provenance LIST_DIRECTORIES false
    RELATIVE "${PREFIX}/share/uml-dependencies"
    "${PREFIX}/share/uml-dependencies/*")
list(SORT _installed_provenance)
set(_expected_provenance
    manifest.tsv "${_platform_contract}" source-trees.tsv build-receipt.tsv)
if(_software)
    list(APPEND _expected_provenance llvm-archives.tsv)
endif()
list(SORT _expected_provenance)
if(NOT _installed_provenance STREQUAL _expected_provenance)
    message(FATAL_ERROR
        "Unexpected Apple GPU provenance payload: ${_installed_provenance}")
endif()

set(_receipt
    "${PREFIX}/share/uml-dependencies/build-receipt.tsv")
file(STRINGS "${_receipt}" _receipt_rows)
set(_receipt_fields
    format product platform sdk_name sdk_version sdk_path compiler
    built_at_utc)
if(NOT _software)
    list(APPEND _receipt_fields venus_protocol_generated_sha256)
endif()
# A row is a field and its value, with no list separator in either. Each field
# is known and appears once; _receipt_value_<field> holds its value.
foreach(_row IN LISTS _receipt_rows)
    if(_row STREQUAL "" OR _row STREQUAL "field\tvalue")
        continue()
    endif()
    if(NOT _row MATCHES "^([^\t;]*)\t([^\t;]*)$")
        message(FATAL_ERROR
            "Malformed Apple GPU build receipt row: ${_row}")
    endif()
    set(_field "${CMAKE_MATCH_1}")
    set(_value "${CMAKE_MATCH_2}")
    list(FIND _receipt_fields "${_field}" _field_index)
    if(_field_index EQUAL -1 OR DEFINED _receipt_value_${_field})
        message(FATAL_ERROR
            "Unexpected or duplicate Apple GPU receipt field: ${_field}")
    endif()
    set(_receipt_value_${_field} "${_value}")
endforeach()
foreach(_field IN LISTS _receipt_fields)
    if(NOT DEFINED _receipt_value_${_field})
        message(FATAL_ERROR "Apple GPU build receipt is incomplete")
    endif()
endforeach()
function(_receipt_expect _field _expected)
    if(NOT _receipt_value_${_field} STREQUAL "${_expected}")
        message(FATAL_ERROR
            "Apple GPU build receipt ${_field} is ${_receipt_value_${_field}}, expected ${_expected}")
    endif()
endfunction()
_receipt_expect(format 2)
_receipt_expect(product apple-gpu-vulkan-experiment)
_receipt_expect(platform "${PLATFORM}")
if(NOT _software)
    string(LENGTH "${_receipt_value_venus_protocol_generated_sha256}" _length)
    if(NOT _receipt_value_venus_protocol_generated_sha256 MATCHES "^[0-9a-f]+$" OR
       NOT _length EQUAL 64)
        message(FATAL_ERROR
            "Invalid protocol build identity: venus_protocol_generated_sha256")
    endif()
endif()
foreach(_field IN ITEMS sdk_name sdk_version sdk_path compiler built_at_utc)
    if(_receipt_value_${_field} STREQUAL "")
        message(FATAL_ERROR "Apple GPU build receipt has invalid ${_field}")
    endif()
endforeach()

set(_static_archives
    "${PREFIX}/lib/libepoxy.a"
    "${PREFIX}/lib/libvirglrenderer.a")
set(_gl_frameworks MesaEGL MesaGallium)
set(_framework_binaries)
foreach(_framework IN LISTS _gl_frameworks)
    list(APPEND _framework_binaries
        "${PREFIX}/Frameworks/${_framework}.framework/${_framework}")
endforeach()
if(NOT _software)
    list(APPEND _framework_binaries
        "${PREFIX}/Frameworks/KosmicKrisp.framework/KosmicKrisp")
endif()
set(_required
    ${_static_archives}
    ${_framework_binaries}
    "${PREFIX}/include/epoxy/egl.h"
    "${PREFIX}/include/virgl/virglrenderer.h"
    "${PREFIX}/include/virgl/virgl_hw.h"
    )
foreach(_framework IN LISTS _gl_frameworks)
    list(APPEND _required
        "${PREFIX}/Frameworks/${_framework}.framework/Info.plist")
endforeach()
if(NOT _software)
    list(APPEND _required
        "${PREFIX}/include/vulkan/vulkan.h"
        "${PREFIX}/Frameworks/KosmicKrisp.framework/Info.plist"
        "${PREFIX}/Frameworks/KosmicKrisp.framework/Headers/KosmicKrisp.h"
        "${PREFIX}/Frameworks/KosmicKrisp.framework/Modules/module.modulemap")
endif()
uml_require_prefix_files(LABEL "Apple GPU" FILES ${_required})

foreach(_archive IN LISTS _static_archives)
    execute_process(COMMAND "${_tool_ar}" -t "${_archive}"
        RESULT_VARIABLE _archive_status OUTPUT_QUIET ERROR_QUIET)
    if(NOT _archive_status EQUAL 0)
        message(FATAL_ERROR "Invalid Apple GPU static archive: ${_archive}")
    endif()
    execute_process(COMMAND "${_tool_nm}" -u "${_archive}"
        RESULT_VARIABLE _archive_nm_status
        OUTPUT_VARIABLE _archive_undefined_symbols)
    get_filename_component(_archive_name "${_archive}" NAME)
    # Venus calls KosmicKrisp's vkGetInstanceProcAddr directly and loads no
    # code at run time (venus_dynamic_loading=disabled in the platform
    # contract). libepoxy may dlopen: its Apple topic opens only the packaged
    # @rpath/MesaEGL.framework/MesaEGL and resolves GL through EGL.
    if(_archive_name STREQUAL "libvirglrenderer.a" AND NOT _software)
        if(NOT _archive_undefined_symbols MATCHES "_vkGetInstanceProcAddr")
            message(FATAL_ERROR
                "${_archive} does not directly reference vkGetInstanceProcAddr")
        endif()
        if(_archive_undefined_symbols MATCHES "(^|\n)_(dlopen|dlsym)(\n|$)")
            message(FATAL_ERROR
                "${_archive} contains a dynamic-loader fallback")
        endif()
        string(REGEX REPLACE
            "[^\n]*_vkGetInstanceProcAddr[^\n]*(\n|$)" ""
            _unexpected_provider_symbols "${_archive_undefined_symbols}")
    else()
        set(_unexpected_provider_symbols "${_archive_undefined_symbols}")
    endif()
    if(NOT _archive_nm_status EQUAL 0 OR
       _unexpected_provider_symbols MATCHES "_(CGL|EAGL|vk[A-Z])")
        message(FATAL_ERROR
            "${_archive} contains a forbidden provider symbol")
    endif()
endforeach()

foreach(_binary IN LISTS _static_archives _framework_binaries)
    uml_verify_apple_binary(PATH "${_binary}"
        PLATFORM ${_mach_platform} MINOS ${_deployment_target}
        LIPO "${_tool_lipo}" OTOOL "${_tool_otool}")
endforeach()

# _verify_framework(<name> SYSTEM <regex> [DEPENDENCIES <path>...])
# Checks a framework's install name, load commands, and Info.plist deployment
# target, and that it links only itself, the base system libraries, the system
# frameworks SYSTEM matches, and the exact DEPENDENCIES. Each framework keeps
# its own allowlist. A system framework admits any path below its directory,
# so no framework may name a GL or Vulkan framework or loader anywhere.
function(_verify_framework _name)
    cmake_parse_arguments(PARSE_ARGV 1 _arg "" "SYSTEM" "DEPENDENCIES")
    set(_bundle "${PREFIX}/Frameworks/${_name}.framework")
    set(_binary "${_bundle}/${_name}")
    execute_process(COMMAND "${_tool_otool}" -D "${_binary}"
        RESULT_VARIABLE _id_status OUTPUT_VARIABLE _install_name_output
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    # otool prints the binary's path line, then its install name.
    if(NOT _id_status EQUAL 0 OR NOT _install_name_output MATCHES
       "\n@rpath/${_name}\\.framework/${_name}$")
        message(FATAL_ERROR "${_name}.framework has an invalid install name")
    endif()
    execute_process(COMMAND "${_tool_otool}" -l "${_binary}"
        RESULT_VARIABLE _load_status OUTPUT_VARIABLE _load_commands)
    if(NOT _load_status EQUAL 0 OR _load_commands MATCHES "LC_SUB_CLIENT" OR
       _load_commands MATCHES "LC_RPATH")
        message(FATAL_ERROR
            "${_name}.framework has an invalid client or rpath command")
    endif()
    execute_process(COMMAND plutil -extract "${_minimum_os_key}" raw -o -
        "${_bundle}/Info.plist"
        RESULT_VARIABLE _plist_status OUTPUT_VARIABLE _minimum_os
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _plist_status EQUAL 0 OR
       NOT _minimum_os STREQUAL "${_deployment_target}")
        message(FATAL_ERROR
            "${_name}.framework has an invalid deployment target")
    endif()

    execute_process(COMMAND "${_tool_otool}" -L "${_binary}"
        RESULT_VARIABLE _closure_status OUTPUT_VARIABLE _closure)
    if(NOT _closure_status EQUAL 0)
        message(FATAL_ERROR "Could not inspect ${_name}.framework closure")
    endif()
    # Each indented line is a dependency's whole path, which may contain
    # spaces, then its versions in parentheses.
    string(REPLACE "\n" ";" _closure_lines "${_closure}")
    foreach(_line IN LISTS _closure_lines)
        if(NOT _line MATCHES "^[ \t]")
            continue()
        endif()
        if(NOT _line MATCHES "^[ \t]+(.+) \\(compatibility version [^()]*\\)$")
            message(FATAL_ERROR
                "Could not parse ${_name}.framework dependency: ${_line}")
        endif()
        set(_dependency "${CMAKE_MATCH_1}")
        list(FIND _arg_DEPENDENCIES "${_dependency}" _allowed)
        if(_dependency STREQUAL "@rpath/${_name}.framework/${_name}" OR
           NOT _allowed EQUAL -1 OR
           _dependency MATCHES "^/usr/lib/(libc\\+\\+\\.1|libSystem\\.B|libobjc\\.A|libz\\.1)\\.dylib$" OR
           _dependency MATCHES "^/System/Library/Frameworks/(${_arg_SYSTEM})\\.framework/")
            continue()
        endif()
        message(FATAL_ERROR
            "Unexpected ${_name}.framework dependency: ${_dependency}")
    endforeach()
    if(_closure MATCHES
       "OpenGL\\.framework|OpenGLES\\.framework|Vulkan\\.framework|libvulkan")
        message(FATAL_ERROR
            "${_name}.framework contains a forbidden GL/Vulkan dependency")
    endif()
endfunction()

foreach(_framework IN LISTS _gl_frameworks)
    set(_binary "${PREFIX}/Frameworks/${_framework}.framework/${_framework}")
    file(GLOB_RECURSE _framework_payload LIST_DIRECTORIES false
        RELATIVE "${PREFIX}/Frameworks/${_framework}.framework"
        "${PREFIX}/Frameworks/${_framework}.framework/*")
    list(SORT _framework_payload)
    set(_expected_framework_payload Info.plist "${_framework}")
    list(SORT _expected_framework_payload)
    if(NOT _framework_payload STREQUAL _expected_framework_payload)
        message(FATAL_ERROR
            "Unexpected ${_framework}.framework payload: ${_framework_payload}")
    endif()

    # The OpenGL provider is a two-library Mesa stack rendering on the
    # packaged Vulkan driver, so each links exactly the next one down. The
    # software variant's Gallium library renders on the CPU and links no
    # driver library.
    if(_framework STREQUAL "MesaEGL")
        set(_next MesaGallium)
    elseif(_software)
        set(_next)
    else()
        set(_next KosmicKrisp)
    endif()
    set(_next_dependency)
    if(_next)
        set(_next_dependency "@rpath/${_next}.framework/${_next}")
    endif()
    _verify_framework(${_framework}
        SYSTEM "CoreFoundation|CoreGraphics|Foundation|IOKit|IOSurface|Metal|QuartzCore"
        DEPENDENCIES
            /usr/lib/libexpat.1.dylib ${_next_dependency})
    execute_process(COMMAND "${_tool_nm}" -u "${_binary}"
        RESULT_VARIABLE _nm_status OUTPUT_VARIABLE _undefined_symbols)
    if(_framework STREQUAL "MesaGallium" AND NOT _software)
        if(NOT _undefined_symbols MATCHES "_vkGetInstanceProcAddr")
            message(FATAL_ERROR
                "MesaGallium.framework does not reference vkGetInstanceProcAddr")
        endif()
        string(REGEX REPLACE
            "[^\n]*_vkGetInstanceProcAddr[^\n]*(\n|$)" ""
            _provider_symbols "${_undefined_symbols}")
    else()
        set(_provider_symbols "${_undefined_symbols}")
    endif()
    if(NOT _nm_status EQUAL 0 OR
       _provider_symbols MATCHES "_(CGL|EAGL|vk[A-Z])")
        message(FATAL_ERROR
            "${_framework}.framework contains a forbidden provider symbol")
    endif()
    if(_framework STREQUAL "MesaGallium")
        # The software rasterizers, and the LLVM that llvmpipe JITs through,
        # are the Simulator's alone: a Zink build has no software Gallium
        # screen to fall back to and carries no LLVM, and the Simulator has
        # nothing else to render with.
        execute_process(COMMAND "${_tool_nm}" -a "${_binary}"
            RESULT_VARIABLE _all_status OUTPUT_VARIABLE _all_symbols)
        if(NOT _all_status EQUAL 0)
            message(FATAL_ERROR "MesaGallium.framework could not be listed")
        endif()
        if(_software)
            # The contract's gl_provider lists the drivers the prefix carries.
            file(STRINGS
                "${SOURCE_ROOT}/dependencies/platforms/${_platform_contract}"
                _provider_rows REGEX "^gl_provider\t")
            string(REGEX REPLACE "^gl_provider\t" "" _providers
                "${_provider_rows}")
            string(REPLACE "," ";" _providers "${_providers}")
            if(NOT _providers)
                message(FATAL_ERROR
                    "the platform contract names no gl_provider")
            endif()
            foreach(_provider IN LISTS _providers)
                if(NOT _all_symbols MATCHES
                   "(^|\n)[0-9a-f]+ [tT] _${_provider}_create_screen\n")
                    message(FATAL_ERROR
                        "MesaGallium.framework carries no ${_provider} "
                        "Gallium screen")
                endif()
            endforeach()
        elseif(_all_symbols MATCHES "_(softpipe|llvmpipe)_create_screen")
            message(FATAL_ERROR
                "MesaGallium.framework carries a software Gallium screen")
        elseif(_all_symbols MATCHES "(^|\n)[0-9a-f]+ [a-zA-Z] _(LLVM[A-Za-z]|_ZN4llvm)")
            message(FATAL_ERROR "MesaGallium.framework carries LLVM")
        endif()
    endif()
    if(_framework STREQUAL "MesaEGL")
        execute_process(COMMAND "${_tool_nm}" -gU "${_binary}"
            RESULT_VARIABLE _export_status OUTPUT_VARIABLE _exports)
        if(NOT _export_status EQUAL 0 OR
           NOT _exports MATCHES "[ 	]_eglGetPlatformDisplay([ 	
]|$)")
            message(FATAL_ERROR
                "MesaEGL.framework does not export the EGL platform bootstrap")
        endif()
    endif()
endforeach()

if(NOT _software)
    set(_binary "${PREFIX}/Frameworks/KosmicKrisp.framework/KosmicKrisp")
    _verify_framework(KosmicKrisp SYSTEM "Metal|QuartzCore")
    execute_process(COMMAND "${_tool_nm}" -gU "${_binary}"
        RESULT_VARIABLE _export_status OUTPUT_VARIABLE _exports)
    if(NOT _export_status EQUAL 0 OR
       NOT _exports MATCHES "[ \t]_vkGetInstanceProcAddr([ \t\n]|$)")
        message(FATAL_ERROR
            "KosmicKrisp.framework does not export vkGetInstanceProcAddr")
    endif()
    execute_process(COMMAND "${_tool_nm}" -u "${_binary}"
        RESULT_VARIABLE _undefined_status OUTPUT_VARIABLE _undefined_symbols)
    if(NOT _undefined_status EQUAL 0 OR
       _undefined_symbols MATCHES "(^|\n)_(dlopen|dlsym)(\n|$)")
        message(FATAL_ERROR
            "KosmicKrisp.framework contains a dynamic-loader fallback")
    endif()
    file(READ
        "${PREFIX}/Frameworks/KosmicKrisp.framework/Modules/module.modulemap"
        _module_map)
    if(NOT _module_map MATCHES "framework module KosmicKrisp" OR
       NOT _module_map MATCHES "umbrella header \"KosmicKrisp.h\"")
        message(FATAL_ERROR
            "KosmicKrisp.framework has an invalid module map")
    endif()
endif()

message(STATUS
    "Verified apple-gpu-vulkan-experiment/${PLATFORM} Apple GPU dependency "
    "prefix: ${PREFIX}")
