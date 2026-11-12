# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/VerifyManifestProduct.cmake")

# Checks the CMake dependency prefix verifiers share. LABEL names the prefix in
# diagnostics.

# uml_verify_source_trees(INSTALLED <file> LABEL <text> SOURCE_ROOT <dir>
#     PRODUCT <name>)
# A patched source is identified by the tree its generated series records,
# and a product's patched sources are its manifest rows with a patch series.
# Builders install source-trees.tsv from the prepared sources; the prefix is
# current exactly while it lists those sources and every tree matches its
# series. A product without a patched source records no source trees.
function(uml_verify_source_trees)
    cmake_parse_arguments(PARSE_ARGV 0 _arg ""
        "INSTALLED;LABEL;SOURCE_ROOT;PRODUCT" "")
    uml_manifest_patch_series(
        MANIFEST "${_arg_SOURCE_ROOT}/dependencies/manifest.tsv"
        SOURCE_ROOT "${_arg_SOURCE_ROOT}"
        PRODUCT "${_arg_PRODUCT}"
        OWNED _pairs)
    if(NOT _pairs)
        return()
    endif()
    if(NOT EXISTS "${_arg_INSTALLED}" OR IS_SYMLINK "${_arg_INSTALLED}")
        message(FATAL_ERROR
            "${_arg_LABEL} source trees are missing: ${_arg_INSTALLED}")
    endif()
    file(STRINGS "${_arg_INSTALLED}" _rows)
    set(_installed_names)
    foreach(_row IN LISTS _rows)
        if(_row STREQUAL "source\ttree")
            continue()
        endif()
        if(NOT _row MATCHES "^([a-z0-9-]+)\t([0-9a-f]+)$")
            message(FATAL_ERROR
                "Malformed ${_arg_LABEL} source-tree row: ${_row}")
        endif()
        list(FIND _installed_names "${CMAKE_MATCH_1}" _seen)
        if(NOT _seen EQUAL -1)
            message(FATAL_ERROR
                "${_arg_LABEL} source tree is listed twice: ${CMAKE_MATCH_1}")
        endif()
        list(APPEND _installed_names "${CMAKE_MATCH_1}")
        set(_installed_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
    endforeach()

    set(_expected_names)
    while(_pairs)
        list(POP_FRONT _pairs _name _series)
        list(APPEND _expected_names "${_name}")
        if(NOT EXISTS "${_series}" OR IS_SYMLINK "${_series}")
            message(FATAL_ERROR "Patch series is missing: ${_series}")
        endif()
        file(STRINGS "${_series}" _series_trees REGEX "^tree\t")
        list(LENGTH _series_trees _tree_count)
        if(NOT _tree_count EQUAL 1 OR
                NOT _series_trees MATCHES "^tree\t([0-9a-f]+)$")
            message(FATAL_ERROR "Patch series records no tree: ${_series}")
        endif()
        if(NOT "${_installed_${_name}}" STREQUAL "${CMAKE_MATCH_1}")
            message(FATAL_ERROR
                "${_arg_LABEL} ${_name} was built from tree "
                "'${_installed_${_name}}'; its series records ${CMAKE_MATCH_1}. "
                "Rebuild the prefix from sources prepared by the current series.")
        endif()
    endwhile()
    list(SORT _installed_names)
    list(SORT _expected_names)
    if(NOT _installed_names STREQUAL _expected_names)
        message(FATAL_ERROR
            "${_arg_LABEL} source trees list ${_installed_names}, "
            "expected ${_expected_names}")
    endif()
endfunction()

# uml_require_prefix_files(LABEL <text> FILES <file>...)
# Every file exists and is not a symlink.
function(uml_require_prefix_files)
    cmake_parse_arguments(PARSE_ARGV 0 _arg "" "LABEL" "FILES")
    foreach(_file IN LISTS _arg_FILES)
        if(NOT EXISTS "${_file}" OR IS_SYMLINK "${_file}")
            message(FATAL_ERROR
                "${_arg_LABEL} prefix file is missing or a symlink: ${_file}")
        endif()
    endforeach()
endfunction()

# uml_verify_provenance_copy(LABEL <text> SOURCE <file> INSTALLED <file>)
# The prefix installed the checked-in SOURCE byte for byte as INSTALLED, so
# its fields need no separate checks.
function(uml_verify_provenance_copy)
    cmake_parse_arguments(PARSE_ARGV 0 _arg "" "LABEL;SOURCE;INSTALLED" "")
    foreach(_role IN ITEMS SOURCE INSTALLED)
        if(NOT EXISTS "${_arg_${_role}}" OR IS_SYMLINK "${_arg_${_role}}")
            message(FATAL_ERROR
                "${_arg_LABEL} provenance is missing or a symlink: "
                "${_arg_${_role}}")
        endif()
    endforeach()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${_arg_SOURCE}" "${_arg_INSTALLED}"
        RESULT_VARIABLE _status)
    if(NOT _status EQUAL 0)
        get_filename_component(_name "${_arg_INSTALLED}" NAME)
        message(FATAL_ERROR
            "${_arg_LABEL} provenance does not match: ${_name}")
    endif()
endfunction()

# uml_verify_apple_binary(PATH <file> [PLATFORM <number> MINOS <version>]
#     [LIPO <tool>] [OTOOL <tool>])
# PATH holds only arm64 code. With PLATFORM, every object in it, each archive
# member or the binary itself, is Mach-O with exactly one build-version
# command, which names that Mach-O platform and minimum OS version. A legacy
# minimum-version command names no platform row for this check to see, so no
# object may carry one.
function(uml_verify_apple_binary)
    cmake_parse_arguments(PARSE_ARGV 0 _arg ""
        "PATH;PLATFORM;MINOS;LIPO;OTOOL" "")
    if(NOT _arg_LIPO)
        set(_arg_LIPO lipo)
    endif()
    if(NOT _arg_OTOOL)
        set(_arg_OTOOL otool)
    endif()
    execute_process(
        COMMAND "${_arg_LIPO}" -archs "${_arg_PATH}"
        RESULT_VARIABLE _status
        OUTPUT_VARIABLE _architectures
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _status EQUAL 0 OR NOT _architectures STREQUAL "arm64")
        message(FATAL_ERROR
            "${_arg_PATH} is not an arm64-only Apple binary: ${_architectures}")
    endif()
    if(NOT DEFINED _arg_PLATFORM)
        return()
    endif()
    execute_process(
        COMMAND "${_arg_OTOOL}" -l "${_arg_PATH}"
        RESULT_VARIABLE _status
        OUTPUT_VARIABLE _load_commands)
    if(NOT _status EQUAL 0)
        message(FATAL_ERROR "Could not inspect Apple binary: ${_arg_PATH}")
    endif()
    if(_load_commands MATCHES "LC_VERSION_MIN_[A-Z]+")
        message(FATAL_ERROR
            "${_arg_PATH} has a legacy ${CMAKE_MATCH_0} command")
    endif()
    # otool starts a line with PATH for the binary, for each archive member,
    # and for each member it cannot read as Mach-O, such as LLVM bitcode.
    # List separators and brackets in a member name would split the rows.
    string(REPLACE "\n${_arg_PATH}" "\n@OBJECT@" _objects "\n${_load_commands}")
    string(REGEX REPLACE "[][;]" "_" _objects "${_objects}")
    string(REGEX MATCHALL "@OBJECT@[^\n]*|\n[ \t]+cmd LC_BUILD_VERSION\n"
        _rows "${_objects}")
    if(NOT _rows MATCHES "^@OBJECT@")
        message(FATAL_ERROR "otool listed no object in ${_arg_PATH}")
    endif()
    # A final marker closes the last object; the first marker has no object
    # before it to check.
    set(_count 1)
    foreach(_row IN LISTS _rows ITEMS @OBJECT@)
        if(NOT _row MATCHES "^@OBJECT@(.*)$")
            math(EXPR _count "${_count} + 1")
        elseif(NOT _count EQUAL 1)
            message(FATAL_ERROR
                "Expected one build-version command, found ${_count}, in "
                "${_arg_PATH}${_object}")
        else()
            set(_object "${CMAKE_MATCH_1}")
            set(_count 0)
        endif()
    endforeach()
    string(REGEX MATCHALL "platform [0-9]+" _platform_rows "${_load_commands}")
    # A row runs to the end of its line, so the whole version is compared.
    string(REGEX MATCHALL "minos [^\n]*" _minimum_rows "${_load_commands}")
    if(NOT _platform_rows OR NOT _minimum_rows)
        message(FATAL_ERROR
            "Apple binary has no build-version command: ${_arg_PATH}")
    endif()
    foreach(_row IN LISTS _platform_rows)
        if(NOT _row STREQUAL "platform ${_arg_PLATFORM}")
            message(FATAL_ERROR "Wrong Mach-O platform in ${_arg_PATH}: ${_row}")
        endif()
    endforeach()
    foreach(_row IN LISTS _minimum_rows)
        if(NOT _row STREQUAL "minos ${_arg_MINOS}")
            message(FATAL_ERROR
                "Wrong deployment target in ${_arg_PATH}: ${_row}")
        endif()
    endforeach()
endfunction()
