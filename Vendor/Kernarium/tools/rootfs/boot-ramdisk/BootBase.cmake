# SPDX-License-Identifier: GPL-2.0-or-later
#
# Selects the accepted boot base archive for one guest userspace architecture
# and checks it against its descriptor before a launcher embeds it.

include_guard(GLOBAL)
include("${PROJECT_SOURCE_DIR}/kernel/common/artifacts/KernelArtifact.cmake")

function(uml_select_boot_base ARCHITECTURE OUT_ARCHIVE)
    set(_descriptor
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/accepted-${ARCHITECTURE}.env")
    uml_watch_artifact_inputs("${_descriptor}")
    foreach(_key SCHEMA RELEASE FILE ARCHITECTURE SIZE SHA256)
        uml_read_artifact_env("${_descriptor}" "UML_BOOT_BASE_${_key}"
            "_uml_boot_base_${_key}")
    endforeach()
    if(NOT _uml_boot_base_SCHEMA EQUAL 1)
        message(FATAL_ERROR "Unsupported boot base descriptor: ${_descriptor}")
    endif()
    if(NOT _uml_boot_base_ARCHITECTURE STREQUAL ARCHITECTURE)
        message(FATAL_ERROR
            "${_descriptor} describes ${_uml_boot_base_ARCHITECTURE}, "
            "not ${ARCHITECTURE}")
    endif()
    uml_cache_accepted_artifact_path(
        UML_BOOT_BASE_ARCHIVE
        "${PROJECT_SOURCE_DIR}/.build/rootfs/${_uml_boot_base_RELEASE}/${_uml_boot_base_FILE}"
        "Accepted boot base archive for the guest userspace architecture")
    if(NOT EXISTS "${UML_BOOT_BASE_ARCHIVE}" OR
       IS_DIRECTORY "${UML_BOOT_BASE_ARCHIVE}")
        message(FATAL_ERROR
            "The accepted boot base archive is missing: "
            "${UML_BOOT_BASE_ARCHIVE}. Copy ${_uml_boot_base_FILE} from the "
            "published ${_uml_boot_base_RELEASE} starter release.")
    endif()
    uml_watch_artifact_inputs("${UML_BOOT_BASE_ARCHIVE}")
    file(SIZE "${UML_BOOT_BASE_ARCHIVE}" _size)
    file(SHA256 "${UML_BOOT_BASE_ARCHIVE}" _digest)
    if(NOT _size EQUAL _uml_boot_base_SIZE OR
       NOT _digest STREQUAL _uml_boot_base_SHA256)
        message(FATAL_ERROR
            "${UML_BOOT_BASE_ARCHIVE} does not match ${_descriptor}")
    endif()
    set(${OUT_ARCHIVE} "${UML_BOOT_BASE_ARCHIVE}" PARENT_SCOPE)
endfunction()
