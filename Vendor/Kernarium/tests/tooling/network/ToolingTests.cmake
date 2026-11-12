# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME network-smoke-taxonomy
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/network/test_run_guest_network_smoke.py
    LABELS network;slirp
    TIMEOUT 120)
