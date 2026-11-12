# SPDX-License-Identifier: GPL-2.0-or-later

# The guest SME tests (run_guest_sme.py). guest-sme-<engine> runs on a host
# with SME for an engine whose SME support is control or better and skips,
# naming the reason, otherwise; guest-sme-off-<engine> checks on every host
# that a run with the policy forced off shows no SME.
uml_add_freestanding_cxx_payload(UML_GUEST_SME_PROBE guest-sme-probe
    sme_probe.cpp
    COMMENT "Building the freestanding arm64 guest SME probe"
    ARCH_OPTIONS -march=armv8-a -mgeneral-regs-only NO_PIC)

# Test support only: reports each engine's SME support level.
add_executable(uml-sme-engine-support sme_engine_support.cpp)
target_include_directories(uml-sme-engine-support PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
target_link_libraries(uml-sme-engine-support PRIVATE UML::CpuContract)
add_dependencies(integration-test-binaries uml-sme-engine-support)

foreach(_engine IN ITEMS gadgets jit)
    uml_add_guest_integration_test(
        NAME guest-sme-${_engine}
        SCRIPT run_guest_sme.py
        ARGS --name guest-sme-${_engine}
            --engine ${_engine}
            --probe ${UML_GUEST_SME_PROBE}
            --glibc-probe ${CMAKE_CURRENT_SOURCE_DIR}/glibc_za_probe.py
            --engine-support-tool $<TARGET_FILE:uml-sme-engine-support>
        LABELS guest;sme;kernel;${_engine}
        TIMEOUT 900
        CPUS 2)
    set_tests_properties(guest-sme-${_engine} PROPERTIES SKIP_RETURN_CODE 77)
    uml_add_guest_integration_test(
        NAME guest-sme-off-${_engine}
        SCRIPT run_guest_sme.py
        ARGS --name guest-sme-off-${_engine}
            --engine ${_engine}
            --probe ${UML_GUEST_SME_PROBE}
            --glibc-probe ${CMAKE_CURRENT_SOURCE_DIR}/glibc_za_probe.py
            --modes off
        LABELS guest;sme;kernel;${_engine}
        TIMEOUT 300
        CPUS 2)
endforeach()
