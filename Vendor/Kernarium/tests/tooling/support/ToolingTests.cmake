# SPDX-License-Identifier: GPL-2.0-or-later

# The Mach-O reader that the gadget-layout, gadget-label and bundle-identifier
# tests share, on small synthetic files: those tests need built images, which
# a tooling tree lacks.
uml_add_tooling_test(
    NAME macho-reader
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/support/test_macho.py
    LABELS cpu;emulator
    TIMEOUT 60)
