# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_test_require_helper_tests require_helper_tests.cpp)
uml_add_unit_test(NAME unit-test-require-helpers
    COMMAND uml_test_require_helper_tests)
