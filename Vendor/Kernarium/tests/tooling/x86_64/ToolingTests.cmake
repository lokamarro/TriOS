# SPDX-License-Identifier: GPL-2.0-or-later

# The corpus producers accept the prepared FEX source, which carries the
# macOS prefix's embedding series, only while their inputs are the pinned ones.
uml_add_tooling_test(
    NAME x86-64-fex-pinned-source
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/x86_64/test_fex_pinned_source.py
        --pinned-source ${PROJECT_SOURCE_DIR}/tools/fex-tests/pinned_source.py
    LABELS x86-64
    TIMEOUT 60)

# The FEX campaign parses the gadget runner's result line, so its contract is
# checked against the runner itself. The runner exists only in unit trees that
# build the x86-64 guest.
if(TARGET uml_x86_64_fex_asm_runner)
    uml_add_tooling_test(
        NAME x86-64-fex-asm-campaign-runner
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/x86_64/test_fex_asm_campaign.py
            --runner $<TARGET_FILE:uml_x86_64_fex_asm_runner>
            --campaign ${PROJECT_SOURCE_DIR}/tools/fex-tests/run-asm-corpus.py
        LABELS cpu;x86-64
        TIMEOUT 120)
    # Repository corpus cases reach the runner through the producer's
    # configuration encoder and are accepted only on a matching native run.
    uml_add_tooling_test(
        NAME x86-64-repository-corpus-cases
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/x86_64/test_x86_repository_cases.py
            --runner $<TARGET_FILE:uml_x86_64_fex_asm_runner>
            --producer ${PROJECT_SOURCE_DIR}/tools/x86-tests/build-unit-corpus.py
        LABELS cpu;x86-64
        TIMEOUT 120)
endif()

# The pinned x86-64 gadget region (darwin/cpu/x86_64/gadgets/engine/region.inc)
# in the linked images this tree builds: the product, an image composed from
# the gadget objects alone and one linked through the emulator archive behind
# the arm64 region.
set(_uml_x86_64_gadget_region_images)
if(TARGET uml_x86_64_gadgets)
    foreach(_uml_x86_64_gadget_region_image IN ITEMS
            uml-darwin
            uml_x86_64_fex_asm_runner
            uml_emulator_api_tests)
        if(TARGET ${_uml_x86_64_gadget_region_image})
            list(APPEND _uml_x86_64_gadget_region_images
                $<TARGET_FILE:${_uml_x86_64_gadget_region_image}>)
        endif()
    endforeach()
endif()
if(_uml_x86_64_gadget_region_images AND APPLE)
    uml_add_tooling_test(
        NAME x86-64-gadget-region-layout
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/x86_64/test_gadget_region_layout.py
            ${_uml_x86_64_gadget_region_images}
        LABELS cpu;x86-64
        TIMEOUT 120)
endif()

# fit_slots.py reports the slot each group of the engine needs from an assembly
# of engine.S with the flags of the tree's Ninja build.
if(_uml_x86_64_gadget_region_images AND APPLE AND CMAKE_GENERATOR MATCHES "Ninja")
    uml_add_tooling_test(
        NAME x86-64-gadget-fit-slots
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/x86_64/test_fit_slots.py
            --tool ${PROJECT_SOURCE_DIR}/darwin/cpu/x86_64/gadgets/engine/fit_slots.py
            --region ${PROJECT_SOURCE_DIR}/darwin/cpu/x86_64/gadgets/engine/region.inc
            --build-dir ${CMAKE_BINARY_DIR}
            --ninja ${CMAKE_MAKE_PROGRAM}
        LABELS cpu;x86-64
        TIMEOUT 600)
endif()

# The manual x86-64 guest runners: their options, their refusals and the
# compiler commands that pin their probes' bytes, without a guest. Each runs
# under python3 -I, which leaves a script's own directory off sys.path.
uml_add_tooling_test(
    NAME x86-64-guest-runners
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/x86_64/test_guest_runners.py
        --runners ${PROJECT_SOURCE_DIR}/tests/integration/x86_64
    LABELS x86-64
    TIMEOUT 120)
