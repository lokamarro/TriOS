# SPDX-License-Identifier: GPL-2.0-or-later

# ARM64 white-box tests compose the MMU OBJECT carriers directly. Carry the
# shared-backing archive that MappedBacking references so those raw
# compositions retain the production memory component's link contract.
link_libraries(UML::MemorySharedBacking)
# Tests that enter the runtime without an emulator run route a Context of
# their own.
link_libraries(uml_runtime_test_route)

# The root's warning policy covers C++; these hand-written assembly sources
# opt in to it.
set_source_files_properties(arm64_jit_chain_abi_probe.S arm64-neon-inventory.s
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")

# uml_add_arm64_unit_test(<stem> [NAME <test>] [WITHOUT_HARNESS]
#     [SOURCES <sources>...] [OBJECTS <sources>... | NO_OBJECTS]
#     [LABELS <labels>...])
#
# Builds uml_arm64_<stem>_tests from arm64_<stem>_tests.cpp and any SOURCES,
# composed with the static MMU and gadget engine objects (or OBJECTS instead,
# or none for NO_OBJECTS) and linked to the engine test harness unless
# WITHOUT_HARNESS, then registers it as arm64-<stem with dashes>, or as NAME,
# with any ARGS as its command-line arguments.
function(uml_add_arm64_unit_test STEM)
    cmake_parse_arguments(PARSE_ARGV 1 _arm64_test "WITHOUT_HARNESS;NO_OBJECTS"
        "NAME" "SOURCES;OBJECTS;LABELS;ARGS")
    set(_target uml_arm64_${STEM}_tests)
    if(NOT DEFINED _arm64_test_OBJECTS AND NOT _arm64_test_NO_OBJECTS)
        set(_arm64_test_OBJECTS
            $<TARGET_OBJECTS:uml_static_mmu>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_arm64_aarch32_decoder>
            $<TARGET_OBJECTS:uml_arm64_aarch32_semantics>
            $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    endif()
    add_executable(${_target}
        ${CMAKE_CURRENT_SOURCE_DIR}/arm64_${STEM}_tests.cpp
        ${_arm64_test_SOURCES}
        ${_arm64_test_OBJECTS})
    target_link_libraries(${_target} PRIVATE UML::BridgeAbi)
    # An image that carries the gadget region lays it out as the products do.
    if("$<TARGET_OBJECTS:uml_arm64_gadgets>" IN_LIST _arm64_test_OBJECTS)
        target_link_libraries(${_target} PRIVATE UML::GadgetRegionLayout)
    endif()
    target_include_directories(${_target} PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    if(NOT _arm64_test_WITHOUT_HARNESS)
        target_link_libraries(${_target} PRIVATE uml_arm64_engine_test_support)
    endif()
    if(NOT DEFINED _arm64_test_NAME)
        string(REPLACE "_" "-" _arm64_test_NAME "arm64-${STEM}")
    endif()
    set(_labels)
    if(DEFINED _arm64_test_LABELS)
        set(_labels LABELS ${_arm64_test_LABELS})
    endif()
    uml_add_unit_test(NAME ${_arm64_test_NAME}
        COMMAND ${_target} ${_arm64_test_ARGS} ${_labels})
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/engine.cmake")

# The guest-SME policy, the host prober, the SME state every SME lane
# shares and the identification registers, through the guest selector's
# entry points.
uml_add_arm64_unit_test(sme_foundation WITHOUT_HARNESS
    OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

# The virtual CPU presents no non-streaming SVE: the prober, the engines,
# the identification registers and the HWCAPs, and the reserved predicate
# state's reset.
uml_add_arm64_unit_test(no_sve WITHOUT_HARNESS
    OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

# The SM = 0 vector-length family the virtual CPU (FEAT_SME, no FEAT_SVE)
# executes: its definition and the words both engines translate from.
uml_add_arm64_unit_test(sm0_vector_length WITHOUT_HARNESS)

# The AArch32 foundation: the state CpuState appends, the engines' levels,
# the AArch32 identification registers and compat HWCAPs, and the engines'
# AArch32 entry.
uml_add_arm64_unit_test(a32_foundation WITHOUT_HARNESS
    OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

# The host runtime side of AArch32: the formats an AArch32 task's status and
# FPSCR travel in, the load and save around an engine's AArch32 entry, the
# bridge event of each exit, and the entry points and compat HWCAPs against
# what the run presents.
uml_add_arm64_unit_test(a32_host WITHOUT_HARNESS
    OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

# The tagged translation key both engines' caches share across A64, A32 and
# T32, and the key-aware validation of a translation's saved guest units.
uml_add_arm64_unit_test(a32_guest_key WITHOUT_HARNESS NO_OBJECTS)

# The AArch32 decoder: its generated table, the IT rules, the profile's gates
# and the operands of the Decode pseudocode, against the generator's
# independent answers and LLVM MC on stored samples. It composes the decoder
# alone.
uml_add_arm64_unit_test(aarch32_decoder WITHOUT_HARNESS
    OBJECTS $<TARGET_OBJECTS:uml_arm64_aarch32_decoder>
    ARGS ${CMAKE_CURRENT_SOURCE_DIR}/aarch32_decoder)

# The AArch32 semantic library's own contract: fault ordering, the exclusive
# monitor, the single-instruction entry's IT handling, the events and the
# library-exit counters, which only the library's counting variant has. The
# corpus's model lane (below) covers its answers.
uml_add_arm64_unit_test(aarch32_semantics WITHOUT_HARNESS
    OBJECTS $<TARGET_OBJECTS:uml_arm64_aarch32_decoder>
            $<TARGET_OBJECTS:uml_arm64_aarch32_semantics_exit_counting>)

# Instruction-family tests: base, memory, then FP and SIMD.
uml_add_arm64_unit_test(base_arithmetic_allocation)
uml_add_arm64_unit_test(base_bit_allocation)
uml_add_arm64_unit_test(base_multiply_allocation)
uml_add_arm64_unit_test(base_condition_structure)
uml_add_arm64_unit_test(base_min_max_structure)
uml_add_arm64_unit_test(base_control_pc_relative)
uml_add_arm64_unit_test(base_control_direct_branch)
uml_add_arm64_unit_test(base_control_conditional_branch)
uml_add_arm64_unit_test(base_control_compare_test)
uml_add_arm64_unit_test(base_control_register_branch)
uml_add_arm64_unit_test(gadget_branch_emission)
uml_add_arm64_unit_test(base_crc_gadget)
uml_add_arm64_unit_test(base_flag_structure)
uml_add_arm64_unit_test(base_barrier_structure)
uml_add_arm64_unit_test(base_hint_structure)
uml_add_arm64_unit_test(base_store_hint_gadget)
uml_add_arm64_unit_test(base_system_register NAME arm64-base-system-registers)
uml_add_arm64_unit_test(base_cache_zero)
uml_add_arm64_unit_test(base_dc_gadget NAME arm64-base-dc_gadget)
uml_add_arm64_unit_test(base_system_operation_gadget
    NAME arm64-base-system_operation_gadget)
uml_add_arm64_unit_test(base_gic_gadget NAME arm64-base-gic_gadget)
uml_add_arm64_unit_test(base_feature_system_gadget
    NAME arm64-base-feature_system_gadget)
uml_add_arm64_unit_test(base_chkfeat_gadget NAME arm64-base-chkfeat_gadget)
uml_add_arm64_unit_test(base_at_gadget NAME arm64-base-at_gadget)
uml_add_arm64_unit_test(base_ic_gadget NAME arm64-base-ic_gadget)
uml_add_arm64_unit_test(base_pstate_gadget)
uml_add_arm64_unit_test(base_range_prefetch)
uml_add_arm64_unit_test(base_mops_gadget)
uml_add_arm64_unit_test(base_immediate_memory_structure)
uml_add_arm64_unit_test(base_load_store_register_structure)
uml_add_arm64_unit_test(base_ordered_behavior)
uml_add_arm64_unit_test(base_ordered_gadget)
# Load/store-exclusive loops make progress whatever the TLB holds.
uml_add_arm64_unit_test(exclusive_progress)
# Only an exception costs a guest its exclusive monitor, in A64, A32 and T32
# on both engines.
uml_add_arm64_unit_test(exclusive_events)
# After an ISB, both engines run the code another vCPU changed and
# synchronized, in A64 and A32.
uml_add_arm64_unit_test(isb_code_change)
uml_add_arm64_unit_test(simd_ordinary_memory_gadget)
uml_add_arm64_unit_test(simd_structure_gadget)
uml_add_arm64_unit_test(simd_baseline_inventory WITHOUT_HARNESS
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/arm64-neon-inventory.s)
uml_add_arm64_unit_test(sme_encoding_table WITHOUT_HARNESS NO_OBJECTS)
uml_add_arm64_unit_test(gadget_simd_destination_residency)
uml_add_arm64_unit_test(simd_widening_gadget)
uml_add_arm64_unit_test(simd_high_narrow_gadget)
uml_add_arm64_unit_test(simd_two_register_gadget)
uml_add_arm64_unit_test(simd_bitwise_gadget)
uml_add_arm64_unit_test(simd_rearrange_gadget_structure
    NAME arm64-simd-rearrange-gadget_structure)
uml_add_arm64_unit_test(basic_floating_allocation)
uml_add_arm64_unit_test(fused_floating_gadget)
uml_add_arm64_unit_test(floating_horizontal_gadget)
uml_add_arm64_unit_test(floating_reciprocal_gadget)
uml_add_arm64_unit_test(floating_complex_gadget)
uml_add_arm64_unit_test(bfloat_conversion_gadget)
uml_add_arm64_unit_test(bfloat_multiply_gadget)
uml_add_arm64_unit_test(bfloat_dot_gadget)
uml_add_arm64_unit_test(bfloat_matrix_gadget)
uml_add_arm64_unit_test(half_fused_long_allocation)
uml_add_arm64_unit_test(simd_aes_gadget)
uml_add_arm64_unit_test(simd_sha3_gadget)
uml_add_arm64_unit_test(simd_pmull_gadget)
uml_add_arm64_unit_test(simd_indexed_gadget)
uml_add_arm64_unit_test(simd_dot_gadget)
uml_add_arm64_unit_test(simd_matrix_gadget)
uml_add_arm64_unit_test(simd_fcvt_precision_gadget)
uml_add_arm64_unit_test(simd_fixed_transform_gadget
    NAME arm64-simd-fixed_transform_gadget)

# The AArch32 instruction corpus: this runner proves each case consistent with
# the decoder and with itself (the engines' lanes, in engine.cmake, run the
# cases). tests/unit/arm64/aarch32_corpus/README.md owns the pack.
add_executable(uml_arm64_aarch32_corpus_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/aarch32_corpus_runner.cpp
    $<TARGET_OBJECTS:uml_arm64_aarch32_decoder>)
target_include_directories(uml_arm64_aarch32_corpus_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
add_dependencies(uml_arm64_aarch32_corpus_tests uml_arm64_aarch32_corpus)
uml_add_unit_test(NAME arm64-aarch32-corpus
    COMMAND uml_arm64_aarch32_corpus_tests ${UML_ARM64_AARCH32_CORPUS})

# The corpus's model lane: the same pack through the AArch32 semantic
# library (cpu/arm64/aarch32/semantics.hpp), at the library's level.
add_executable(uml_arm64_aarch32_semantics_corpus_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/aarch32_semantics_corpus_runner.cpp
    $<TARGET_OBJECTS:uml_arm64_aarch32_decoder>
    $<TARGET_OBJECTS:uml_arm64_aarch32_semantics>)
target_include_directories(uml_arm64_aarch32_semantics_corpus_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
add_dependencies(uml_arm64_aarch32_semantics_corpus_tests uml_arm64_aarch32_corpus)
uml_add_unit_test(NAME arm64-aarch32-semantics-corpus
    COMMAND uml_arm64_aarch32_semantics_corpus_tests ${UML_ARM64_AARCH32_CORPUS})
