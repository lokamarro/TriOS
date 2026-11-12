# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_test_scratch_directory_tests scratch_directory_tests.cpp)
uml_add_unit_test(NAME unit-test-scratch-directory
    COMMAND uml_test_scratch_directory_tests)
