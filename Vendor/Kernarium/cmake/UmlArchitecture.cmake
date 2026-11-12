# SPDX-License-Identifier: GPL-2.0-or-later
# Explicit architectural ownership metadata consumed by the File API audit.

include_guard(GLOBAL)

function(uml_declare_architecture_target TARGET LAYER)
    if(NOT TARGET "${TARGET}")
        message(FATAL_ERROR "Cannot declare architecture for unknown target: ${TARGET}")
    endif()
    if(NOT LAYER MATCHES "^(api|platform|memory|cpu|device|runtime|emulator|app)$")
        message(FATAL_ERROR "Unknown UML architecture layer '${LAYER}' for ${TARGET}")
    endif()
    get_property(_existing TARGET "${TARGET}" PROPERTY UML_ARCHITECTURE_LAYER)
    if(_existing)
        message(FATAL_ERROR "Architecture layer already declared for ${TARGET}: ${_existing}")
    endif()
    set(_engine_kind "-")
    if(ARGC GREATER 2)
        set(_engine_kind "${ARGV2}")
    endif()
    if(LAYER STREQUAL "cpu")
        if(NOT _engine_kind MATCHES "^(native|gadget|shared|mixed)$")
            message(FATAL_ERROR
                "CPU architecture target ${TARGET} requires an explicit "
                "engine kind: native, gadget, shared, or mixed")
        endif()
    elseif(NOT _engine_kind STREQUAL "-")
        message(FATAL_ERROR
            "Non-CPU architecture target ${TARGET} cannot declare engine "
            "kind ${_engine_kind}")
    endif()
    set_property(TARGET "${TARGET}" PROPERTY UML_ARCHITECTURE_LAYER "${LAYER}")
    set_property(TARGET "${TARGET}" PROPERTY UML_CPU_ENGINE_KIND
        "${_engine_kind}")
    set_property(GLOBAL APPEND PROPERTY UML_ARCHITECTURE_TARGETS "${TARGET}")
endfunction()

function(_uml_collect_directory_targets DIRECTORY OUTPUT)
    get_property(_directory_targets DIRECTORY "${DIRECTORY}"
        PROPERTY BUILDSYSTEM_TARGETS)
    set(_all_targets ${_directory_targets})
    get_property(_subdirectories DIRECTORY "${DIRECTORY}"
        PROPERTY SUBDIRECTORIES)
    foreach(_subdirectory IN LISTS _subdirectories)
        _uml_collect_directory_targets("${_subdirectory}" _child_targets)
        list(APPEND _all_targets ${_child_targets})
    endforeach()
    set(${OUTPUT} "${_all_targets}" PARENT_SCOPE)
endfunction()

function(uml_write_architecture_manifest)
    get_property(_targets GLOBAL PROPERTY UML_ARCHITECTURE_TARGETS)
    list(REMOVE_DUPLICATES _targets)
    list(SORT _targets)
    _uml_collect_directory_targets("${PROJECT_SOURCE_DIR}"
        _configured_targets)
    foreach(_configured_target IN LISTS _configured_targets)
        get_property(_source_directory TARGET "${_configured_target}"
            PROPERTY SOURCE_DIR)
        cmake_path(IS_PREFIX PROJECT_SOURCE_DIR "${_source_directory}"
            NORMALIZE _first_party_target)
        # Match inside the tree, so that a checkout below a directory named
        # darwin does not make every target a Darwin one.
        file(RELATIVE_PATH _relative_directory "${PROJECT_SOURCE_DIR}"
            "${_source_directory}")
        if(_first_party_target AND
           _relative_directory MATCHES "^darwin(/|$)")
            list(FIND _targets "${_configured_target}" _declared_index)
            if(_declared_index EQUAL -1)
                message(FATAL_ERROR
                    "Darwin target has no architecture declaration: "
                    "${_configured_target} (${_source_directory})")
            endif()
        endif()
    endforeach()
    set(_manifest "${CMAKE_BINARY_DIR}/uml-architecture-targets.tsv")
    file(WRITE "${_manifest}"
        "# target\tlayer\ttarget_kind\tengine_kind\tdependencies\n")
    foreach(_target IN LISTS _targets)
        get_property(_layer TARGET "${_target}" PROPERTY UML_ARCHITECTURE_LAYER)
        if(NOT _layer)
            message(FATAL_ERROR "Architecture target has no layer: ${_target}")
        endif()
        get_property(_type TARGET "${_target}" PROPERTY TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY")
            set(_kind "interface")
        else()
            set(_kind "codemodel")
        endif()
        get_property(_engine_kind TARGET "${_target}"
            PROPERTY UML_CPU_ENGINE_KIND)
        if(NOT _engine_kind)
            set(_engine_kind "-")
        endif()

        set(_architecture_dependencies)
        foreach(_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_property(_links TARGET "${_target}" PROPERTY "${_property}")
            foreach(_link IN LISTS _links)
                if(NOT TARGET "${_link}")
                    continue()
                endif()
                get_property(_aliased_target TARGET "${_link}" PROPERTY ALIASED_TARGET)
                if(_aliased_target)
                    set(_link "${_aliased_target}")
                endif()
                list(FIND _targets "${_link}" _architecture_dependency_index)
                if(NOT _architecture_dependency_index EQUAL -1)
                    list(APPEND _architecture_dependencies "link@${_link}")
                endif()
            endforeach()
        endforeach()
        list(REMOVE_DUPLICATES _architecture_dependencies)
        list(SORT _architecture_dependencies)
        string(JOIN "," _dependency_text ${_architecture_dependencies})
        if(NOT _dependency_text)
            set(_dependency_text "-")
        endif()
        file(APPEND "${_manifest}"
            "${_target}\t${_layer}\t${_kind}\t${_engine_kind}\t"
            "${_dependency_text}\n")
    endforeach()
endfunction()
