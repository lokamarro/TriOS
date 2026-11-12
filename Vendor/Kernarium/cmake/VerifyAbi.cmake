# SPDX-License-Identifier: GPL-2.0-or-later
include_guard(GLOBAL)

function(uml_verify_generated_abi)
    execute_process(
        COMMAND "${PROJECT_SOURCE_DIR}/kernel/common/abi/sync.sh" --verify
        RESULT_VARIABLE _uml_abi_result
        OUTPUT_VARIABLE _uml_abi_output
        ERROR_VARIABLE _uml_abi_error)
    if(NOT _uml_abi_result EQUAL 0)
        message(FATAL_ERROR
            "Generated Darwin UML host ABI files are stale. Run "
            "kernel/common/abi/sync.sh.\n${_uml_abi_output}${_uml_abi_error}")
    endif()
endfunction()
