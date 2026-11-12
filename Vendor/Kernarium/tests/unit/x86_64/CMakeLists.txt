# SPDX-License-Identifier: GPL-2.0-or-later

# uml_add_x86_64_test_binary(<target> <source> [GADGETS] [JIT] [RUNNER]
#     [LIBRARIES <libraries>...] [DEFINITIONS <definitions>...]
#     [DEPENDS <targets>...])
#
# Builds <target> from <source> on the x86 shared target, with the gadget
# engine for GADGETS and the FEXCore JIT for JIT, then any LIBRARIES. A RUNNER
# lands in bin/ for the tooling tests and, with JIT, sees the arm64 tests'
# register oracle, which checks FEXCore's emitted code.
function(uml_add_x86_64_test_binary TARGET SOURCE)
    cmake_parse_arguments(PARSE_ARGV 2 _x86 "GADGETS;JIT;RUNNER" ""
        "LIBRARIES;DEFINITIONS;DEPENDS")
    add_executable(${TARGET} ${SOURCE})
    target_link_libraries(${TARGET} PRIVATE uml_x86_64_target)
    if(_x86_GADGETS)
        target_link_libraries(${TARGET} PRIVATE uml_x86_64_gadgets)
    endif()
    if(_x86_JIT)
        target_link_libraries(${TARGET} PRIVATE uml_x86_64_jit
            uml_cpu_executable_memory UML::ThirdPartyFEXCore)
    endif()
    if(_x86_LIBRARIES)
        target_link_libraries(${TARGET} PRIVATE ${_x86_LIBRARIES})
    endif()
    target_link_libraries(${TARGET} PRIVATE UML::CpuContract)
    target_include_directories(${TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/darwin)
    if(_x86_RUNNER)
        set_target_properties(${TARGET} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
        if(_x86_JIT)
            target_include_directories(${TARGET} PRIVATE
                ${PROJECT_SOURCE_DIR}/tests/unit/arm64)
        endif()
    endif()
    if(_x86_DEFINITIONS)
        target_compile_definitions(${TARGET} PRIVATE ${_x86_DEFINITIONS})
    endif()
    if(_x86_DEPENDS)
        add_dependencies(${TARGET} ${_x86_DEPENDS})
    endif()
endfunction()

uml_add_x86_64_test_binary(uml_x86_64_gadget_bootstrap_tests
    x86_64_gadget_bootstrap_tests.cpp
    GADGETS LIBRARIES uml_test_allocation_failure)
uml_add_unit_test(
    NAME x86-64-gadget-bootstrap
    COMMAND uml_x86_64_gadget_bootstrap_tests
    LABELS cpu;x86-64)

# What translated code reads a link by: the {stream, count} pair and the
# guest bounds in front of the stream.
uml_add_x86_64_test_binary(uml_x86_64_gadget_link_layout_tests
    x86_64_gadget_link_layout_tests.cpp GADGETS)
uml_add_unit_test(
    NAME x86-64-gadget-link-layout
    COMMAND uml_x86_64_gadget_link_layout_tests
    LABELS cpu;x86-64)

# What a cached translation is: a body in the address space's arena, and a
# program that is a view of it.
uml_add_x86_64_test_binary(uml_x86_64_gadget_body_tests
    x86_64_gadget_body_tests.cpp GADGETS)
uml_add_unit_test(
    NAME x86-64-gadget-bodies
    COMMAND uml_x86_64_gadget_body_tests
    LABELS cpu;x86-64;tsan)

# The same suite with every executor keeping its translations to itself
# (DARWIN_UML_GADGET_SHARED_BODIES=0); the plain run shares them.
uml_add_unit_test(
    NAME x86-64-gadget-bootstrap-private
    COMMAND uml_x86_64_gadget_bootstrap_tests private
    LABELS cpu;x86-64)

# Executors that share an address space's translations: what sharing does,
# the writer's protocol, and executors on their own threads.
uml_add_x86_64_test_binary(uml_x86_64_gadget_shared_tests
    x86_64_gadget_shared_tests.cpp GADGETS)
uml_add_unit_test(
    NAME x86-64-gadget-shared-bodies
    COMMAND uml_x86_64_gadget_shared_tests bodies
    LABELS cpu;x86-64)
uml_add_unit_test(
    NAME x86-64-gadget-shared-store
    COMMAND uml_x86_64_gadget_shared_tests store
    LABELS cpu;x86-64)
uml_add_unit_test(
    NAME x86-64-gadget-shared-concurrent
    COMMAND uml_x86_64_gadget_shared_tests concurrent
    LABELS cpu;x86-64;tsan)
uml_add_unit_test(
    NAME x86-64-gadget-shared-bound
    COMMAND uml_x86_64_gadget_shared_tests bound
    LABELS cpu;x86-64;tsan)

# The cache's handling of destroyed address-space generations alone, cheap
# enough for the sanitizer trees.
uml_add_unit_test(
    NAME x86-64-gadget-dead-generations
    COMMAND uml_x86_64_gadget_bootstrap_tests dead-generations
    LABELS cpu;x86-64)

# The cache's set index alone: it is header-only, so the test links no gadget
# object.
add_executable(uml_x86_64_gadget_cache_index_tests
    x86_64_gadget_cache_index_tests.cpp)
target_link_libraries(uml_x86_64_gadget_cache_index_tests PRIVATE
    UML::CpuContract)
target_include_directories(uml_x86_64_gadget_cache_index_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(
    NAME x86-64-gadget-cache-index
    COMMAND uml_x86_64_gadget_cache_index_tests
    LABELS cpu;x86-64)

# How a single rounds to a half, which VCVTPS2PH's flags follow, against an
# independent derivation, over every finite single.
uml_add_x86_64_test_binary(uml_x86_64_half_round_tests
    x86_64_half_round_tests.cpp LIBRARIES UML::ThirdPartySoftFloat)
uml_add_unit_test(
    NAME x86-64-half-round
    COMMAND uml_x86_64_half_round_tests
    LABELS cpu;x86-64
    PROCESSORS 4)

# What the model decides in integers -- whether a square root is exact, and
# whether an integer fits the float it converts to -- against SoftFloat's flags.
uml_add_x86_64_test_binary(uml_x86_64_fp_model_tests
    x86_64_fp_model_tests.cpp LIBRARIES UML::ThirdPartySoftFloat)
uml_add_unit_test(
    NAME x86-64-fp-model
    COMMAND uml_x86_64_fp_model_tests
    LABELS cpu;x86-64
    PROCESSORS 4)

uml_add_x86_64_test_binary(uml_x86_64_sse_exactness_tests
    x86_64_sse_exactness_tests.cpp GADGETS)
uml_add_unit_test(
    NAME x86-64-sse-exactness
    COMMAND uml_x86_64_sse_exactness_tests
    LABELS cpu;x86-64)

# The flags an Intel Xeon leaves in a masked MXCSR, against the engine's
# ordinary path.
uml_add_x86_64_test_binary(uml_x86_64_masked_flags_tests
    x86_64_masked_flags_tests.cpp GADGETS DEPENDS uml_x86_64_masked_flags)
uml_add_unit_test(
    NAME x86-64-masked-flags
    COMMAND uml_x86_64_masked_flags_tests "${UML_X86_64_MASKED_FLAGS}"
    LABELS cpu;x86-64)
# The same cases through the gadget engine's checked path, which corrects
# the flags the ordinary path leaves.
uml_add_unit_test(
    NAME x86-64-masked-flags-checked
    COMMAND uml_x86_64_masked_flags_tests --checked "${UML_X86_64_MASKED_FLAGS}"
    LABELS cpu;x86-64)

uml_add_x86_64_test_binary(uml_x86_64_code_page_registry_tests
    code_page_registry_tests.cpp)
uml_add_unit_test(
    NAME x86-64-code-page-registry
    COMMAND uml_x86_64_code_page_registry_tests
    LABELS cpu;x86-64)

# K instruction traces and complementary FEX assembly programs are normalized
# into source tables. CMake generates the pack without either upstream project
# or an x86 assembler; the ordinary unit test consumes only that pack.
set(_corpus "${UML_X86_64_INSTRUCTION_CORPUS}")
uml_add_x86_64_test_binary(uml_x86_64_fex_asm_runner fex_asm_runner.cpp
    GADGETS RUNNER DEPENDS uml_x86_64_instruction_corpus)
uml_add_unit_test(
    NAME x86-64-instruction-corpus
    COMMAND uml_x86_64_fex_asm_runner "${_corpus}"
    LABELS cpu;x86-64
    TIMEOUT 900)
# The instructions a run retires before each kind of stop, which the JIT
# runner checks against the same counts.
uml_add_unit_test(
    NAME x86-64-retirement
    COMMAND uml_x86_64_fex_asm_runner --retirement
    LABELS cpu;x86-64)

# The same corpus through the macOS-only x86-64 JIT. The engine does not yet
# implement every instruction class the gadget engine does; each record it
# fails names its known class in the exclusion list, and a record that starts
# passing fails the run until its row is removed.
if(TARGET uml_x86_64_jit)
    set(_exclusions "${CMAKE_CURRENT_SOURCE_DIR}/jit_corpus_exclusions.tsv")
    uml_add_x86_64_test_binary(uml_x86_64_fex_asm_jit_runner fex_asm_runner.cpp
        JIT RUNNER DEFINITIONS UML_FEX_ASM_JIT=1
        DEPENDS uml_x86_64_instruction_corpus)
    uml_add_unit_test(
        NAME x86-64-jit-instruction-corpus
        COMMAND uml_x86_64_fex_asm_jit_runner --exclusions "${_exclusions}"
            "${_corpus}"
        LABELS cpu;x86-64
        TIMEOUT 300)
    # The same run from a code-size estimate of one byte per IR node: every
    # unit past the first buffer's baseline outgrows it and restarts with a
    # larger one, with the same results.
    uml_add_unit_test(
        NAME x86-64-jit-code-buffer-growth
        COMMAND uml_x86_64_fex_asm_jit_runner --exclusions "${_exclusions}"
            --code-size-per-node 1 --require-growth "${_corpus}"
        LABELS cpu;x86-64
        TIMEOUT 300)
    # No block FEXCore emits for the corpus names x18, which Darwin clears
    # when it interrupts a thread.
    uml_add_unit_test(
        NAME x86-64-jit-emitted-registers
        COMMAND uml_x86_64_fex_asm_jit_runner --exclusions "${_exclusions}"
            --emitted-registers "${_corpus}"
        LABELS cpu;x86-64
        TIMEOUT 300)
    # XE's differential harness through the JIT, against the gadget engine's
    # oracle under every MXCSR rounding, DAZ and FTZ setting, with #XM for a
    # denormal operand, and against the gadget engine under MXCSRs that
    # unmask other exceptions.
    uml_add_x86_64_test_binary(uml_x86_64_sse_exactness_jit_tests
        x86_64_sse_exactness_tests.cpp GADGETS JIT
        DEFINITIONS UML_SSE_EXACTNESS_JIT=1 UML_X86_64_SSE_LOOP_JIT=1)
    uml_add_unit_test(
        NAME x86-64-jit-sse-exactness
        COMMAND uml_x86_64_sse_exactness_jit_tests
        LABELS cpu;x86-64)
    # The masked flags through the JIT.
    uml_add_x86_64_test_binary(uml_x86_64_masked_flags_jit_tests
        x86_64_masked_flags_tests.cpp GADGETS JIT
        DEFINITIONS UML_X86_64_SSE_LOOP_JIT=1 DEPENDS uml_x86_64_masked_flags)
    uml_add_unit_test(
        NAME x86-64-jit-masked-flags
        COMMAND uml_x86_64_masked_flags_jit_tests "${UML_X86_64_MASKED_FLAGS}"
        LABELS cpu;x86-64)
    uml_add_unit_test(
        NAME x86-64-jit-retirement
        COMMAND uml_x86_64_fex_asm_jit_runner --retirement
        LABELS cpu;x86-64)
    # Tiered execution: the corpus and the retirement counts again from every
    # handover of the gadget engine to the JIT, at every instruction boundary
    # of a short case and a ladder of them in a long one, which must end as
    # either engine alone does.
    uml_add_x86_64_test_binary(uml_x86_64_fex_asm_switch_runner
        fex_asm_runner.cpp GADGETS JIT RUNNER
        DEFINITIONS UML_FEX_ASM_JIT=1 UML_FEX_ASM_SWITCH=1
        DEPENDS uml_x86_64_instruction_corpus)
    uml_add_unit_test(
        NAME x86-64-tier-switch-corpus
        COMMAND uml_x86_64_fex_asm_switch_runner
            --exclusions "${CMAKE_CURRENT_SOURCE_DIR}/tier_switch_exclusions.tsv"
            --switch-sweep "${_corpus}"
        LABELS cpu;x86-64
        TIMEOUT 1800)
    uml_add_unit_test(
        NAME x86-64-tier-switch-retirement
        COMMAND uml_x86_64_fex_asm_switch_runner --switch-sweep --retirement
        LABELS cpu;x86-64)
    # Message passing between two engines on two host threads: a space that
    # orders stores never shows the flag without the data, and one that
    # does not shows it, so the check would see the order go missing.
    uml_add_x86_64_test_binary(uml_x86_64_jit_store_order_tests
        x86_64_jit_store_order_tests.cpp JIT)
    uml_add_unit_test(
        NAME x86-64-jit-store-order
        COMMAND uml_x86_64_jit_store_order_tests
        LABELS cpu;x86-64
        PROCESSORS 2
        TIMEOUT 300)
    # An engine makes a gone address space's space over for the next one,
    # which must translate the new bytes, and a destroyed engine leaves no
    # FEXCore mapping behind.
    uml_add_x86_64_test_binary(uml_x86_64_jit_space_tests
        x86_64_jit_space_tests.cpp JIT)
    uml_add_unit_test(
        NAME x86-64-jit-space-recycling
        COMMAND uml_x86_64_jit_space_tests
        LABELS cpu;x86-64
        TIMEOUT 300)
    # x86's protocol for code another processor changes: a store into
    # translated code publishes after it lands, and CPUID and IRET end the
    # run so the runtime hands over the changes before the code after them.
    uml_add_x86_64_test_binary(uml_x86_64_jit_code_change_tests
        x86_64_jit_code_change_tests.cpp JIT)
    uml_add_unit_test(
        NAME x86-64-jit-code-change
        COMMAND uml_x86_64_jit_code_change_tests
        LABELS cpu;x86-64
        TIMEOUT 300)
    # CPUID through the JIT answers every leaf and subleaf as cpuid.hpp does,
    # AVX-VNNI's included.
    uml_add_x86_64_test_binary(uml_x86_64_jit_cpuid_tests
        x86_64_jit_cpuid_tests.cpp JIT)
    uml_add_unit_test(
        NAME x86-64-jit-cpuid
        COMMAND uml_x86_64_jit_cpuid_tests
        LABELS cpu;x86-64
        TIMEOUT 300)
    # VPDPBUSD and its relatives under every lowering the host can run.
    uml_add_x86_64_test_binary(uml_x86_64_jit_vnni_tests
        x86_64_jit_vnni_tests.cpp JIT)
    uml_add_unit_test(
        NAME x86-64-jit-vnni
        COMMAND uml_x86_64_jit_vnni_tests
        LABELS cpu;x86-64
        TIMEOUT 300)
endif()
