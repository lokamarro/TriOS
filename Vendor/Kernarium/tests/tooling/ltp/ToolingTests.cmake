# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME ltp-runner-parser
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/ltp/run_guest_ltp_tests.py
    LABELS kernel;ltp
    TIMEOUT 120)
