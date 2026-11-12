# SPDX-License-Identifier: GPL-2.0-or-later

# Load/store-exclusive loops make progress when the pages they touch share a
# TLB entry and when a copy-on-write break lands between the pair
# (run_exclusive_progress.py), on each engine.
uml_add_freestanding_cxx_payload(UML_GUEST_EXCLUSIVE_PROGRESS_PROBE
    guest-exclusive-progress-probe
    exclusive_progress_probe.cpp
    COMMENT "Building the freestanding arm64 guest exclusive-progress probe"
    ARCH_OPTIONS -march=armv8-a -mgeneral-regs-only NO_PIC)

foreach(_engine IN ITEMS gadgets jit)
    uml_add_guest_integration_test(
        NAME guest-exclusive-progress-${_engine}
        SCRIPT run_exclusive_progress.py
        ARGS --engine ${_engine}
            --probe ${UML_GUEST_EXCLUSIVE_PROGRESS_PROBE}
            --log "${CMAKE_CURRENT_BINARY_DIR}/exclusive-progress-${_engine}.log"
        LABELS guest;cpu;kernel;${_engine}
        TIMEOUT 420
        CPUS 2)
endforeach()
