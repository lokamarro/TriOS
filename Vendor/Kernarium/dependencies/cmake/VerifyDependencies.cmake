# SPDX-License-Identifier: GPL-2.0-or-later
# Verify the private, source-built dependency prefix without consulting package
# registries or host search paths. Acquisition and compilation remain explicit
# dependencies/prepare.sh and dependencies/build.sh steps.

include_guard(GLOBAL)

function(uml_verify_dependency_prefix)
    cmake_parse_arguments(ARG ""
        "PREFIX;MANIFEST;DEPLOYMENT_TARGET" "" ${ARGN})
    if(NOT ARG_PREFIX OR NOT ARG_MANIFEST OR NOT ARG_DEPLOYMENT_TARGET)
        message(FATAL_ERROR
            "uml_verify_dependency_prefix requires PREFIX, MANIFEST, "
            "and DEPLOYMENT_TARGET")
    endif()

    execute_process(
        COMMAND "${PROJECT_SOURCE_DIR}/dependencies/verify-prefix.sh"
            --prefix "${ARG_PREFIX}"
            --manifest "${ARG_MANIFEST}"
            --deployment-target "${ARG_DEPLOYMENT_TARGET}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR
            "Private dependency prefix verification failed. Run "
            "dependencies/prepare.sh and dependencies/build.sh first.\n"
            "${_output}${_error}")
    endif()
endfunction()
