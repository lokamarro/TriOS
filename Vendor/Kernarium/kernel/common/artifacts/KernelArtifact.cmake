# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

function(uml_read_artifact_env METADATA KEY OUT_VALUE)
    if(NOT IS_ABSOLUTE "${METADATA}" OR NOT EXISTS "${METADATA}")
        message(FATAL_ERROR "Artifact metadata is missing: ${METADATA}")
    endif()
    file(STRINGS "${METADATA}" _uml_artifact_rows
        REGEX "^${KEY}=(('[^']*')|([0-9]+))$")
    list(LENGTH _uml_artifact_rows _uml_artifact_row_count)
    if(NOT _uml_artifact_row_count EQUAL 1)
        message(FATAL_ERROR
            "Artifact metadata does not contain exactly one ${KEY}: ${METADATA}")
    endif()
    list(GET _uml_artifact_rows 0 _uml_artifact_row)
    string(REGEX REPLACE "^[^=]+='?([^']*)'?$" "\\1"
        _uml_artifact_value "${_uml_artifact_row}")
    set(${OUT_VALUE} "${_uml_artifact_value}" PARENT_SCOPE)
endfunction()

function(uml_watch_artifact_inputs)
    foreach(_uml_artifact_input IN LISTS ARGN)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${_uml_artifact_input}")
    endforeach()
endfunction()

# The cached path follows the accepted default until a caller sets it to
# another path, which then stays pinned across later acceptance changes.
# Clearing the value opts back into the accepted default.
function(uml_cache_accepted_artifact_path VARIABLE DEFAULT_VALUE DESCRIPTION)
    set(_uml_default_tracker "${VARIABLE}_ACCEPTED_DEFAULT")
    set(_uml_follow_tracker "${VARIABLE}_FOLLOW_ACCEPTED")
    if(NOT DEFINED ${VARIABLE} OR "${${VARIABLE}}" STREQUAL "")
        set(_uml_follow_accepted ON)
    elseif(DEFINED ${_uml_follow_tracker})
        # A value that no longer equals the last generated default was set by
        # a caller and is pinned from then on.
        if(${_uml_follow_tracker} AND
           "${${VARIABLE}}" STREQUAL "${${_uml_default_tracker}}")
            set(_uml_follow_accepted ON)
        else()
            set(_uml_follow_accepted OFF)
        endif()
    elseif("${${VARIABLE}}" STREQUAL "${DEFAULT_VALUE}")
        set(_uml_follow_accepted ON)
    else()
        set(_uml_follow_accepted OFF)
    endif()

    if(_uml_follow_accepted)
        set(_uml_selected_artifact "${DEFAULT_VALUE}")
    else()
        set(_uml_selected_artifact "${${VARIABLE}}")
    endif()

    set(${VARIABLE} "${_uml_selected_artifact}"
        CACHE PATH "${DESCRIPTION}" FORCE)
    set(${_uml_default_tracker} "${DEFAULT_VALUE}" CACHE INTERNAL
        "Last generated accepted-artifact default for ${VARIABLE}" FORCE)
    set(${_uml_follow_tracker} "${_uml_follow_accepted}" CACHE INTERNAL
        "Whether ${VARIABLE} follows the accepted-artifact default" FORCE)
    set(${VARIABLE} "${_uml_selected_artifact}" PARENT_SCOPE)
endfunction()

# DESCRIPTOR is optional: an accepted-*.env descriptor whose recorded identity
# must also match the bundle's manifest.
function(uml_import_kernel_artifact)
    set(options)
    set(oneValueArgs
        TARGET ARTIFACT_ROOT COMMON_ROOT EXPECTED_FAMILY EXPECTED_SLICE
        EXPECTED_RELEASE_SERIES DESCRIPTOR)
    cmake_parse_arguments(ARTIFACT "${options}" "${oneValueArgs}" "" ${ARGN})
    foreach(_uml_required IN ITEMS
            TARGET ARTIFACT_ROOT COMMON_ROOT EXPECTED_FAMILY EXPECTED_SLICE
            EXPECTED_RELEASE_SERIES)
        if(NOT ARTIFACT_${_uml_required})
            message(FATAL_ERROR
                "uml_import_kernel_artifact requires ${_uml_required}")
        endif()
    endforeach()
    if(TARGET "${ARTIFACT_TARGET}")
        message(FATAL_ERROR "Kernel artifact target already exists: ${ARTIFACT_TARGET}")
    endif()
    if(NOT IS_ABSOLUTE "${ARTIFACT_ARTIFACT_ROOT}")
        message(FATAL_ERROR "Kernel artifact root must be absolute")
    endif()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_uml_abi_metadata "${ARTIFACT_COMMON_ROOT}/abi/identity.env")
    set(_uml_verifier
        "${ARTIFACT_COMMON_ROOT}/artifacts/verify-kernel-artifact.py")
    set(_uml_descriptor_arguments)
    if(ARTIFACT_DESCRIPTOR)
        set(_uml_descriptor_arguments --descriptor "${ARTIFACT_DESCRIPTOR}")
    endif()
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -B "${_uml_verifier}"
            --artifact "${ARTIFACT_ARTIFACT_ROOT}"
            --common-root "${ARTIFACT_COMMON_ROOT}"
            --abi-metadata "${_uml_abi_metadata}"
            --compatible
            --expected-family "${ARTIFACT_EXPECTED_FAMILY}"
            --expected-slice "${ARTIFACT_EXPECTED_SLICE}"
            --expected-release-series "${ARTIFACT_EXPECTED_RELEASE_SERIES}"
            ${_uml_descriptor_arguments}
        RESULT_VARIABLE _uml_artifact_result
        OUTPUT_VARIABLE _uml_artifact_output
        ERROR_VARIABLE _uml_artifact_error)
    if(NOT _uml_artifact_result EQUAL 0)
        message(FATAL_ERROR
            "Cannot import the compatible kernel artifact:\n"
            "${_uml_artifact_output}${_uml_artifact_error}")
    endif()

    # The verifier admitted the bundle only with the expected identity.
    add_library("${ARTIFACT_TARGET}" INTERFACE)
    set_property(TARGET "${ARTIFACT_TARGET}" PROPERTY
        UML_ARTIFACT_FAMILY "${ARTIFACT_EXPECTED_FAMILY}")
    set_property(TARGET "${ARTIFACT_TARGET}" PROPERTY
        UML_KERNEL_IMAGE "${ARTIFACT_ARTIFACT_ROOT}/kernel/vmlinux")
    set_property(TARGET "${ARTIFACT_TARGET}" PROPERTY
        UML_KERNEL_MODULES_BUILTIN
        "${ARTIFACT_ARTIFACT_ROOT}/kernel/modules.builtin")

    # The bridge import check reads the policy and every bridge header. A
    # header added with new declarations also changes identity.env, which
    # re-runs configure and this glob.
    file(GLOB _uml_bridge_headers "${ARTIFACT_COMMON_ROOT}/abi/darwin_uml_*.h")
    uml_watch_artifact_inputs(
        "${ARTIFACT_ARTIFACT_ROOT}/kernel-artifact.json"
        "${_uml_abi_metadata}"
        "${_uml_verifier}"
        "${ARTIFACT_COMMON_ROOT}/artifacts/contract_checks.py"
        "${ARTIFACT_COMMON_ROOT}/artifacts/kernel_artifact.py"
        "${ARTIFACT_COMMON_ROOT}/abi/bridge_imports.py"
        "${ARTIFACT_COMMON_ROOT}/abi/import-policy.tsv"
        ${_uml_bridge_headers}
        "${ARTIFACT_ARTIFACT_ROOT}/kernel/vmlinux"
        "${ARTIFACT_ARTIFACT_ROOT}/kernel/config"
        "${ARTIFACT_ARTIFACT_ROOT}/kernel/kernel.release"
        "${ARTIFACT_ARTIFACT_ROOT}/kernel/modules.builtin"
        "${ARTIFACT_ARTIFACT_ROOT}/kernel/modules.builtin.modinfo")
endfunction()

# Import the bundle that an accepted-*.env descriptor names. CACHE_VARIABLE
# holds the bundle's directory: it follows the descriptor and defaults to the
# bundle below .build/kernel/artifacts, until a caller pins it
# (uml_cache_accepted_artifact_path). The release series is always 6.18.
# The descriptor repeats the bundle's identity for the commit that accepted it,
# and the import fails when the bundle it names disagrees. A pinned bundle with
# another directory name is not the descriptor's, so it is not compared.
function(uml_import_accepted_kernel_artifact)
    set(oneValueArgs
        TARGET DESCRIPTOR CACHE_VARIABLE DESCRIPTION FAMILY SLICE)
    cmake_parse_arguments(ACCEPTED "" "${oneValueArgs}" "" ${ARGN})
    uml_read_artifact_env(
        "${ACCEPTED_DESCRIPTOR}"
        UML_KERNEL_ARTIFACT_DIRECTORY_NAME
        _uml_accepted_directory_name)
    uml_watch_artifact_inputs("${ACCEPTED_DESCRIPTOR}")
    uml_cache_accepted_artifact_path(
        "${ACCEPTED_CACHE_VARIABLE}"
        "${PROJECT_SOURCE_DIR}/.build/kernel/artifacts/${_uml_accepted_directory_name}"
        "${ACCEPTED_DESCRIPTION}")

    cmake_path(GET CMAKE_CURRENT_FUNCTION_LIST_DIR PARENT_PATH
        _uml_accepted_common_root)
    string(REGEX REPLACE "/+$" "" _uml_selected_root
        "${${ACCEPTED_CACHE_VARIABLE}}")
    cmake_path(GET _uml_selected_root FILENAME _uml_selected_name)
    set(_uml_descriptor_check)
    if(_uml_selected_name STREQUAL _uml_accepted_directory_name)
        set(_uml_descriptor_check DESCRIPTOR "${ACCEPTED_DESCRIPTOR}")
    endif()
    uml_import_kernel_artifact(
        TARGET "${ACCEPTED_TARGET}"
        ARTIFACT_ROOT "${${ACCEPTED_CACHE_VARIABLE}}"
        COMMON_ROOT "${_uml_accepted_common_root}"
        EXPECTED_FAMILY "${ACCEPTED_FAMILY}"
        EXPECTED_SLICE "${ACCEPTED_SLICE}"
        EXPECTED_RELEASE_SERIES 6.18
        ${_uml_descriptor_check})
endfunction()
