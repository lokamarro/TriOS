# SPDX-License-Identifier: GPL-2.0-or-later

function(uml_generate_instruction_corpus NAME SOURCE_DIRECTORY PACK_NAME)
    set(_source "${SOURCE_DIRECTORY}/source")
    set(_index "${_source}/index.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_index}")
    file(READ "${_index}" _manifest)
    string(JSON _count LENGTH "${_manifest}" files)
    math(EXPR _last "${_count} - 1")
    set(_inputs "${_index}")
    foreach(_i RANGE 0 ${_last})
        string(JSON _file GET "${_manifest}" files ${_i})
        list(APPEND _inputs "${_source}/${_file}")
    endforeach()
    set(_metadata)
    foreach(_file provenance.json manifest.tsv coverage.tsv excluded.tsv)
        if(EXISTS "${SOURCE_DIRECTORY}/${_file}")
            list(APPEND _inputs "${SOURCE_DIRECTORY}/${_file}")
            list(APPEND _metadata "${CMAKE_CURRENT_BINARY_DIR}/${_file}")
        endif()
    endforeach()
    set(_generator "${PROJECT_SOURCE_DIR}/tools/instruction-corpus/source.py")
    set(_pack "${CMAKE_CURRENT_BINARY_DIR}/${PACK_NAME}")
    add_custom_command(OUTPUT "${_pack}"
        BYPRODUCTS ${_metadata}
        COMMAND ${Python3_EXECUTABLE} -B "${_generator}"
            --source "${_source}" --output "${CMAKE_CURRENT_BINARY_DIR}"
        DEPENDS ${_inputs} "${_generator}"
            "${PROJECT_SOURCE_DIR}/tools/instruction-corpus/loops.py" ${ARGN}
        COMMENT "Generating ${NAME} instruction corpus from source"
        VERBATIM)
    add_custom_target(${NAME} DEPENDS "${_pack}")
    set(${NAME}_PATH "${_pack}" PARENT_SCOPE)
endfunction()
