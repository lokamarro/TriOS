# SPDX-License-Identifier: GPL-2.0-or-later

# The guest no-SVE test (run_guest_no_sve.py): on every host and engine the
# guest shows no SVE.
uml_add_freestanding_cxx_payload(UML_GUEST_NO_SVE_PROBE guest-no-sve-probe
    no_sve_probe.cpp
    COMMENT "Building the freestanding arm64 guest no-SVE probe"
    ARCH_OPTIONS -march=armv8-a -mgeneral-regs-only NO_PIC)

foreach(_engine IN ITEMS gadgets jit)
    uml_add_guest_integration_test(
        NAME guest-no-sve-${_engine}
        SCRIPT run_guest_no_sve.py
        ARGS --name guest-no-sve-${_engine}
            --engine ${_engine}
            --probe ${UML_GUEST_NO_SVE_PROBE}
        LABELS guest;sve;kernel;${_engine}
        TIMEOUT 300
        CPUS 2)
endforeach()
