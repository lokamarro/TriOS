# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME abi-declaration-identity
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/abi/test_declaration_identity.sh
    LABELS abi;audit
    TIMEOUT 120)
