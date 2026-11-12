# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

function(uml_record_registered_test SUITE NAME)
    string(TOUPPER "${SUITE}" _suite_upper)
    set(_target_property "UML_${_suite_upper}_COMMAND_TARGETS")
    get_property(_command_targets GLOBAL PROPERTY "${_target_property}")
    set(_test_targets "")
    foreach(_argument IN LISTS ARGN)
        set(_command_target "")
        if(_argument MATCHES "^\\$<TARGET_FILE:([^>]+)>$")
            set(_command_target "${CMAKE_MATCH_1}")
        elseif(TARGET "${_argument}")
            set(_command_target "${_argument}")
        endif()
        if(NOT _command_target OR NOT TARGET "${_command_target}")
            continue()
        endif()
        get_property(_aliased_target TARGET "${_command_target}"
            PROPERTY ALIASED_TARGET)
        if(_aliased_target)
            set(_command_target "${_aliased_target}")
        endif()
        get_target_property(_command_target_imported "${_command_target}"
            IMPORTED)
        get_target_property(_command_target_type "${_command_target}" TYPE)
        if(NOT _command_target_imported AND
           NOT _command_target_type STREQUAL "INTERFACE_LIBRARY")
            list(APPEND _test_targets "${_command_target}")
        endif()
    endforeach()
    list(APPEND _command_targets ${_test_targets})
    list(REMOVE_DUPLICATES _command_targets)
    set_property(GLOBAL PROPERTY "${_target_property}" "${_command_targets}")
    set_property(GLOBAL APPEND PROPERTY UML_REGISTERED_TESTS "${NAME}")
    set_property(GLOBAL APPEND PROPERTY "UML_TEST_${NAME}_COMMAND_TARGETS"
        ${_test_targets})
endfunction()

function(uml_finalize_suite_binary_target SUITE AGGREGATE_TARGET)
    if(NOT TARGET "${AGGREGATE_TARGET}")
        add_custom_target("${AGGREGATE_TARGET}")
    endif()
    string(TOUPPER "${SUITE}" _suite_upper)
    get_property(_command_targets GLOBAL PROPERTY
        "UML_${_suite_upper}_COMMAND_TARGETS")
    foreach(_command_target IN LISTS _command_targets)
        add_dependencies("${AGGREGATE_TARGET}" "${_command_target}")
    endforeach()
endfunction()

# A test preset that selects its tests by name alone owns that list. Build the
# command targets of exactly the registered tests it selects, so the build
# presets paired with it name one aggregate instead of repeating the list.
# CTest and if(MATCHES) share one regular-expression engine. A preset that
# inherits the list must not replace it, or it would run another test set.
function(uml_finalize_test_preset_binary_target TEST_PRESET AGGREGATE_TARGET)
    set(_presets_file "${PROJECT_SOURCE_DIR}/CMakePresets.json")
    set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY
        CMAKE_CONFIGURE_DEPENDS "${_presets_file}")
    file(READ "${_presets_file}" _presets)
    string(JSON _preset_count LENGTH "${_presets}" testPresets)
    math(EXPR _last_preset "${_preset_count} - 1")
    set(_filtered "")
    set(_selector "")
    foreach(_index RANGE ${_last_preset})
        string(JSON _preset GET "${_presets}" testPresets ${_index})
        string(JSON _name GET "${_preset}" name)
        set("_parents_${_name}" "")
        string(JSON _inherits_type ERROR_VARIABLE _inherits_error
            TYPE "${_preset}" inherits)
        if(_inherits_type STREQUAL "STRING")
            string(JSON _parent GET "${_preset}" inherits)
            list(APPEND "_parents_${_name}" "${_parent}")
        elseif(_inherits_type STREQUAL "ARRAY")
            string(JSON _parent_count LENGTH "${_preset}" inherits)
            set(_parent_index 0)
            while(_parent_index LESS _parent_count)
                string(JSON _parent GET "${_preset}" inherits ${_parent_index})
                list(APPEND "_parents_${_name}" "${_parent}")
                math(EXPR _parent_index "${_parent_index} + 1")
            endwhile()
        endif()
        string(JSON _filter ERROR_VARIABLE _error GET "${_preset}" filter)
        if(NOT _error)
            list(APPEND _filtered "${_name}")
        endif()
        if(NOT _name STREQUAL TEST_PRESET)
            continue()
        endif()
        if(NOT _error)
            string(JSON _filter_keys LENGTH "${_filter}")
            string(JSON _include ERROR_VARIABLE _error GET "${_filter}" include)
        endif()
        if(NOT _error)
            string(JSON _include_keys LENGTH "${_include}")
            string(JSON _selector ERROR_VARIABLE _error GET "${_include}" name)
        endif()
        if(_error OR NOT _filter_keys EQUAL 1 OR NOT _include_keys EQUAL 1 OR
           NOT "${_parents_${_name}}" STREQUAL "")
            message(FATAL_ERROR
                "test preset ${TEST_PRESET} must select its tests by "
                "filter.include.name alone")
        endif()
    endforeach()
    if(NOT _selector)
        message(FATAL_ERROR "test preset ${TEST_PRESET} is not defined")
    endif()
    foreach(_name IN LISTS _filtered)
        set(_pending "${_parents_${_name}}")
        set(_seen "")
        while(NOT "${_pending}" STREQUAL "")
            list(POP_FRONT _pending _parent)
            if(_parent STREQUAL TEST_PRESET)
                message(FATAL_ERROR
                    "test preset ${_name} inherits ${TEST_PRESET} but sets its "
                    "own filter, which ${AGGREGATE_TARGET} does not follow")
            endif()
            if(NOT _parent IN_LIST _seen)
                list(APPEND _seen "${_parent}")
                list(APPEND _pending ${_parents_${_parent}})
            endif()
        endwhile()
    endforeach()

    add_custom_target("${AGGREGATE_TARGET}")
    get_property(_tests GLOBAL PROPERTY UML_REGISTERED_TESTS)
    set(_selected_tests "")
    foreach(_test IN LISTS _tests)
        if(NOT _test MATCHES "${_selector}")
            continue()
        endif()
        list(APPEND _selected_tests "${_test}")
        get_property(_test_targets GLOBAL PROPERTY
            "UML_TEST_${_test}_COMMAND_TARGETS")
        foreach(_command_target IN LISTS _test_targets)
            add_dependencies("${AGGREGATE_TARGET}" "${_command_target}")
        endforeach()
    endforeach()
    if(NOT _selected_tests)
        message(FATAL_ERROR
            "test preset ${TEST_PRESET} selects no registered test")
    endif()
endfunction()

set(UML_UNIT_TEST_DEFAULT_TIMEOUT 600)
# Seconds from the SIGINT CTest sends a test at its timeout to the SIGKILL,
# enough for a runner to stop its emulator.
set(UML_TEST_TIMEOUT_GRACE_PERIOD 20)

# A test that runs a built tool, named by a $<TARGET_FILE:...> argument of its
# command, needs that tool before it starts, whichever build the tree came
# from. The tool is built by a CTest fixture of the test's suite, so running
# the test by name after a configure is enough. The check workflows build the
# suite's binaries first and the fixture then finds nothing to do. One build
# runs at a time (RESOURCE_LOCK), because two builds of one tree must not
# overlap.
function(_uml_require_tool_targets SUITE NAME)
    set(_fixtures "")
    foreach(_argument IN LISTS ARGN)
        if(NOT _argument MATCHES "^\\$<TARGET_FILE:([^>]+)>$")
            continue()
        endif()
        set(_tool "${CMAKE_MATCH_1}")
        if(NOT TARGET "${_tool}")
            continue()
        endif()
        get_property(_aliased_tool TARGET "${_tool}" PROPERTY ALIASED_TARGET)
        if(_aliased_tool)
            set(_tool "${_aliased_tool}")
        endif()
        get_target_property(_tool_imported "${_tool}" IMPORTED)
        get_target_property(_tool_type "${_tool}" TYPE)
        if(_tool_imported OR _tool_type STREQUAL "INTERFACE_LIBRARY")
            continue()
        endif()
        set(_fixture "uml-tool-${_tool}")
        get_property(_registered GLOBAL PROPERTY "UML_TOOL_FIXTURE_${_tool}")
        if(NOT _registered)
            set(_build_test "${SUITE}-build-${_tool}")
            add_test(NAME "${_build_test}" COMMAND "${CMAKE_COMMAND}"
                --build "${CMAKE_BINARY_DIR}" --target "${_tool}"
                --config "$<CONFIG>")
            set_tests_properties("${_build_test}" PROPERTIES
                LABELS "${SUITE}"
                FIXTURES_SETUP "${_fixture}"
                RESOURCE_LOCK uml-tool-build
                TIMEOUT 3600
                TIMEOUT_SIGNAL_NAME SIGINT
                TIMEOUT_SIGNAL_GRACE_PERIOD "${UML_TEST_TIMEOUT_GRACE_PERIOD}")
            uml_record_registered_test(${SUITE} "${_build_test}")
            set_property(GLOBAL PROPERTY "UML_TOOL_FIXTURE_${_tool}" TRUE)
        endif()
        list(APPEND _fixtures "${_fixture}")
    endforeach()
    if(_fixtures)
        list(REMOVE_DUPLICATES _fixtures)
        set_tests_properties("${NAME}" PROPERTIES
            FIXTURES_REQUIRED "${_fixtures}")
    endif()
endfunction()

# Registers one test in SUITE (unit, tooling or integration). Its labels are
# the suite, then rootfs for a ROOTFS integration test, then the caller's
# LABELS, and must name exactly one primary suite. Its command targets join
# the suite's binary aggregate.
function(_uml_add_suite_test SUITE)
    cmake_parse_arguments(TEST "ROOTFS" "NAME;WORKING_DIRECTORY;TIMEOUT;PROCESSORS"
        "COMMAND;LABELS;RESOURCE_GROUPS" ${ARGN})
    if(NOT TEST_NAME OR NOT TEST_COMMAND)
        message(FATAL_ERROR
            "uml_add_${SUITE}_test requires NAME and COMMAND")
    endif()

    set(_labels "${SUITE}")
    if(SUITE STREQUAL "unit")
        # Unit tests are native host binaries (or CMake-only setup actions).
        # Keep Python and shell runners in the tooling/integration suites even
        # when a caller attempts to hide one behind an absolute interpreter
        # path.
        foreach(_command_argument IN LISTS TEST_COMMAND)
            if(_command_argument MATCHES "(^|/)(python[0-9.]*|sh|bash|zsh)$" OR
               _command_argument MATCHES "\\.(py|sh)$")
                message(FATAL_ERROR
                    "${TEST_NAME}: unit tests may not invoke Python or shell: "
                    "${_command_argument}")
            endif()
        endforeach()
        # A wedged unit binary fails its suite instead of stalling it. The
        # default stays several times above the slowest unit test measured
        # without an explicit bound, sanitizer presets and a loaded host
        # included; a test that needs longer passes its own TIMEOUT.
        if(NOT TEST_TIMEOUT)
            set(TEST_TIMEOUT "${UML_UNIT_TEST_DEFAULT_TIMEOUT}")
        endif()
        # Scratch files stay in a per-test build-tree directory; see
        # tests/unit/scratch/scratch_directory.hpp. UML_UNIT_TEST_NAME gives
        # each test its registered name.
        set(_environment
            "UML_UNIT_TEST_NAME=${TEST_NAME}"
            "UML_TEST_SCRATCH_DIRECTORY=${CMAKE_BINARY_DIR}/test-scratch/${TEST_NAME}")
    else()
        # Tooling and integration runners import helpers from neighboring
        # source directories. Keep validation from recreating source-adjacent
        # __pycache__ debris.
        set(_environment "PYTHONDONTWRITEBYTECODE=1")
        # A shell runs its traps only once the command it waits for has
        # finished, so a shell test that times out inside a child cannot
        # remove its temporary files. They go to a per-test build-tree
        # directory instead of the host's. Python tests keep $TMPDIR, whose
        # short paths fit Unix-domain sockets, and unwind on the timeout's
        # SIGINT.
        list(GET TEST_COMMAND 0 _command)
        if(SUITE STREQUAL "tooling" AND
           _command MATCHES "(^|/)(sh|bash|zsh)$|\\.sh$")
            set(_scratch "${CMAKE_BINARY_DIR}/test-scratch/${TEST_NAME}")
            file(MAKE_DIRECTORY "${_scratch}")
            list(APPEND _environment "TMPDIR=${_scratch}")
        endif()
    endif()
    if(TEST_ROOTFS)
        if(NOT SUITE STREQUAL "integration")
            message(FATAL_ERROR
                "${TEST_NAME}: only integration tests take ROOTFS")
        endif()
        if(NOT UML_INTEGRATION_ROOTFS)
            message(FATAL_ERROR
                "${TEST_NAME}: ROOTFS integration tests require UML_INTEGRATION_ROOTFS")
        endif()
        list(APPEND _labels rootfs)
    endif()
    list(APPEND _labels ${TEST_LABELS})
    set(_suites)
    foreach(_label IN LISTS _labels)
        if(_label MATCHES "^(unit|tooling|integration)$")
            list(APPEND _suites "${_label}")
        endif()
    endforeach()
    list(LENGTH _suites _suite_count)
    if(NOT _suite_count EQUAL 1)
        message(FATAL_ERROR
            "${TEST_NAME}: exactly one primary suite label is required, got: ${_suites}")
    endif()

    add_test(NAME "${TEST_NAME}" COMMAND ${TEST_COMMAND})
    _uml_require_tool_targets(${SUITE} "${TEST_NAME}" ${TEST_COMMAND})
    # At the timeout, SIGINT rather than SIGKILL lets a Python runner unwind
    # its cleanups and a shell run its INT trap. The test still fails as a
    # timeout, and the SIGKILL follows the grace period.
    set_tests_properties("${TEST_NAME}" PROPERTIES LABELS "${_labels}"
        TIMEOUT_SIGNAL_NAME SIGINT
        TIMEOUT_SIGNAL_GRACE_PERIOD "${UML_TEST_TIMEOUT_GRACE_PERIOD}")
    foreach(_property IN ITEMS WORKING_DIRECTORY TIMEOUT PROCESSORS RESOURCE_GROUPS)
        if(TEST_${_property})
            set_tests_properties("${TEST_NAME}" PROPERTIES
                ${_property} "${TEST_${_property}}")
        endif()
    endforeach()
    set_property(TEST "${TEST_NAME}" APPEND PROPERTY ENVIRONMENT ${_environment})
    uml_record_registered_test(${SUITE} "${TEST_NAME}" ${TEST_COMMAND})
endfunction()

function(uml_add_unit_test)
    _uml_add_suite_test(unit ${ARGN})
endfunction()

function(uml_add_tooling_test)
    _uml_add_suite_test(tooling ${ARGN})
endfunction()

function(uml_add_integration_test)
    _uml_add_suite_test(integration ${ARGN})
endfunction()
