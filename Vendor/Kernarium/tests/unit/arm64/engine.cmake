# SPDX-License-Identifier: GPL-2.0-or-later
#
# This family manifest is included by the ARM64 unit-test directory.
    # Compose ordinary tests from the production engine targets. Focused JIT
    # diagnostic tests substitute the counter-enabled executor variant;
    # common conformance stays on the ordinary runtime path.
    set(UML_STATIC_RUNTIME_TEST_BASE_OBJECTS
        $<TARGET_OBJECTS:uml_host_topology>
        $<TARGET_OBJECTS:uml_run_routing>
        $<TARGET_OBJECTS:uml_static_runtime>
        $<TARGET_OBJECTS:uml_cpu_engine_factory>
        $<TARGET_OBJECTS:uml_arm64_gadget_engine>
        $<TARGET_OBJECTS:uml_arm64_jit_engine>
        $<TARGET_OBJECTS:uml_arm64_gadget_executor>
        $<TARGET_OBJECTS:uml_cpu_executable_memory>
        $<TARGET_OBJECTS:uml_arm64_jit>
        $<TARGET_OBJECTS:uml_arm64_aarch32_decoder>
        $<TARGET_OBJECTS:uml_arm64_aarch32_semantics>
        $<TARGET_OBJECTS:uml_arm64_jit_test_support>
        $<TARGET_OBJECTS:uml_static_guest_memory>
        $<TARGET_OBJECTS:uml_runtime_context>)
    if(TARGET uml_x86_64_gadgets)
        list(APPEND UML_STATIC_RUNTIME_TEST_BASE_OBJECTS
            $<TARGET_OBJECTS:uml_x86_64_target>
            $<TARGET_OBJECTS:uml_x86_64_gadgets>)
        # Static-runtime test binaries compose production OBJECT carriers
        # directly, so carry the x87 archives that would otherwise propagate
        # through the production component target.
        link_libraries(UML::ThirdPartyCephes128)
    endif()
    set(UML_STATIC_RUNTIME_TEST_OBJECTS
        ${UML_STATIC_RUNTIME_TEST_BASE_OBJECTS}
        $<TARGET_OBJECTS:uml_arm64_jit_executor>)
    set(UML_STATIC_RUNTIME_DIAGNOSTIC_TEST_OBJECTS
        ${UML_STATIC_RUNTIME_TEST_BASE_OBJECTS}
        $<TARGET_OBJECTS:uml_arm64_jit_diagnostics_executor>)
    # uml_add_arm64_unit_test() OBJECTS for tests that enter the static
    # runtime, with and without the guest engine selector, and with the
    # diagnostics executor.
    set(UML_ARM64_RUNTIME_TEST_OBJECTS
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_aarch32_decoder>
        $<TARGET_OBJECTS:uml_arm64_aarch32_semantics>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    set(UML_ARM64_RUNTIME_DIAGNOSTIC_TEST_OBJECTS
        $<TARGET_OBJECTS:uml_runtime_guest_selector>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_aarch32_decoder>
        $<TARGET_OBJECTS:uml_arm64_aarch32_semantics>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_DIAGNOSTIC_TEST_OBJECTS})
    set(UML_ARM64_STATIC_TEST_OBJECTS
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_aarch32_decoder>
        $<TARGET_OBJECTS:uml_arm64_aarch32_semantics>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        $<TARGET_OBJECTS:uml_static_mmu>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    # The tests that read the AArch32 library-exit counters, with the
    # semantic library variant that has them in place of the library.
    set(UML_ARM64_COUNTING_RUNTIME_TEST_OBJECTS
        ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    list(TRANSFORM UML_ARM64_COUNTING_RUNTIME_TEST_OBJECTS REPLACE
        "uml_arm64_aarch32_semantics>"
        "uml_arm64_aarch32_semantics_exit_counting>")

    # The instruction corpus is zlib-compressed. Reuse the checked private
    # zlib when desktop dependencies are enabled; otherwise import only the
    # same pinned archive without broadening production linkage.
    if(TARGET UML::ThirdPartyZlib)
        set(_uml_arm64_corpus_zlib UML::ThirdPartyZlib)
    else()
        set(_uml_arm64_corpus_zlib_archive
            "${UML_DEPENDENCY_PREFIX}/lib/libz.a")
        set(_uml_arm64_corpus_zlib_include
            "${UML_DEPENDENCY_PREFIX}/include")
        if(NOT EXISTS "${_uml_arm64_corpus_zlib_archive}" OR
           NOT EXISTS "${_uml_arm64_corpus_zlib_include}/zlib.h")
            message(FATAL_ERROR
                "the A64 instruction corpus requires the pinned private zlib")
        endif()
        add_library(uml_arm64_corpus_zlib STATIC IMPORTED)
        set_target_properties(uml_arm64_corpus_zlib PROPERTIES
            IMPORTED_LOCATION "${_uml_arm64_corpus_zlib_archive}"
            INTERFACE_INCLUDE_DIRECTORIES
                "${_uml_arm64_corpus_zlib_include}")
        set(_uml_arm64_corpus_zlib uml_arm64_corpus_zlib)
    endif()

    # Focused tests that need engine hooks or host-private entry state run
    # their cases through the production gadget and JIT engines with this
    # harness.
    add_library(uml_arm64_engine_test_support STATIC
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_engine_test_support.cpp)
    target_link_libraries(uml_arm64_engine_test_support PRIVATE
        UML::Emulator)
    target_link_libraries(uml_arm64_engine_test_support PUBLIC UML::BridgeAbi)
    target_include_directories(uml_arm64_engine_test_support PUBLIC
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64)

    # The generated A64 instruction corpus runs through the production gadget
    # and JIT engines in one process. tests/unit/arm64/corpus/README.md owns
    # the pack format and its regeneration.
    add_executable(uml_arm64_instruction_corpus_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/instruction_corpus_runner.cpp)
    target_link_libraries(uml_arm64_instruction_corpus_tests PRIVATE
        UML::BridgeAbi
        UML::Emulator
        ${_uml_arm64_corpus_zlib})
    target_include_directories(uml_arm64_instruction_corpus_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    add_dependencies(uml_arm64_instruction_corpus_tests uml_arm64_instruction_corpus)
    uml_add_unit_test(NAME arm64-instruction-corpus
        COMMAND uml_arm64_instruction_corpus_tests
            ${UML_ARM64_INSTRUCTION_CORPUS})
    # The same pack translated as for a host with FEAT_AFP and every feature
    # implements_afp() needs, which selects the FEAT_AFP twins on any host.
    # Without the hardware feature only the cases whose FPCR leaves FIZ, AH
    # and NEP clear run, and the twins must match the base entries on them.
    uml_add_unit_test(NAME arm64-instruction-corpus-afp-twins
        COMMAND uml_arm64_instruction_corpus_tests
            ${UML_ARM64_INSTRUCTION_CORPUS}
            --assume-host-feature afp --assume-host-feature fp16
            --assume-host-feature fhm --assume-host-feature bf16
            --assume-host-feature frintts --assume-host-feature fcma
            --assume-host-feature jscvt)

    # A virtual CPU with FEAT_SME and without FEAT_SVE executes CNT, INC, DEC,
    # RDVL, ADDVL and ADDPL outside streaming mode for a vector length of 16
    # bytes, a deliberate departure from the architecture whose results are
    # the project's (arm64-sm0-vl), and takes every other SVE word there as
    # UNDEFINED (arm64-absent-sve). The records hold an SVL, so they run on
    # any host with the SME profile assumed; the engines translate the family
    # to plain A64.
    uml_add_unit_test(NAME arm64-instruction-corpus-sm0-vl-profile
        COMMAND uml_arm64_instruction_corpus_tests
            ${UML_ARM64_INSTRUCTION_CORPUS}
            --record arm64-sm0-vl --record arm64-absent-sve
            --record arm64-absent-sme
            --assume-host-feature sme --assume-host-feature sme2
            --assume-host-feature sme_f64f64
            --assume-host-feature sme_i16i64 --assume-sme-svl 64)

    # Neither the gadget region nor a JIT translation of the corpus names
    # x18, which Darwin clears when it interrupts a thread. The scan
    # translates through the JIT's test API.
    target_sources(uml_arm64_instruction_corpus_tests PRIVATE
        $<TARGET_OBJECTS:uml_arm64_jit_test_support>)
    uml_add_unit_test(NAME arm64-emitted-registers
        COMMAND uml_arm64_instruction_corpus_tests
            ${UML_ARM64_INSTRUCTION_CORPUS}
            --emitted-registers)

    uml_add_arm64_unit_test(engine_test_support NAME arm64-engine-harness
        NO_OBJECTS)
    uml_add_arm64_unit_test(deferred_fp_state NO_OBJECTS)

    add_executable(uml_tlb_sync_range_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/tlb_sync_range_tests.cpp)
    target_link_libraries(uml_tlb_sync_range_tests PRIVATE UML::BridgeAbi)
    uml_add_unit_test(NAME uml-tlb-sync-ranges COMMAND uml_tlb_sync_range_tests)

    uml_add_arm64_unit_test(gadget_tlb_layout WITHOUT_HARNESS NO_OBJECTS)

	add_executable(uml_cpu_engine_contract_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/cpu_engine_contract_tests.cpp)
	target_link_libraries(uml_cpu_engine_contract_tests PRIVATE
		UML::BridgeAbi
		UML::Emulator
		uml_test_allocation_failure)
	target_include_directories(uml_cpu_engine_contract_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin
		${PROJECT_SOURCE_DIR}/darwin/emulator)
	uml_add_unit_test(NAME cpu-engine-contract COMMAND uml_cpu_engine_contract_tests)

    uml_add_arm64_unit_test(pc_alignment_contract WITHOUT_HARNESS NO_OBJECTS
        LABELS tsan)
    target_link_libraries(uml_arm64_pc_alignment_contract_tests PRIVATE
        UML::Emulator)

    uml_add_arm64_unit_test(floating_exception WITHOUT_HARNESS NO_OBJECTS)

    uml_add_arm64_unit_test(floating_conversion WITHOUT_HARNESS NO_OBJECTS)

    uml_add_arm64_unit_test(floating_trap_control)

    uml_add_arm64_unit_test(floating_trap_runtime WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(gadget_terminal_count WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_STATIC_TEST_OBJECTS})
    target_compile_definitions(uml_arm64_gadget_terminal_count_tests PRIVATE
        UML_GADGET_CACHE_SLOTS=${UML_GADGET_CACHE_SLOT_COUNT}
        UML_GADGET_RESIDENT_GENERATIONS=${UML_GADGET_RESIDENT_GENERATIONS})
    # Every memory transfer's fault names its instruction by the position in
    # its operand; the executor turns that back into the PC.
    uml_add_arm64_unit_test(gadget_memory_fault_position WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_STATIC_TEST_OBJECTS})
    # The SME control core and the generated streaming forms, outside
    # streaming mode through injected profiles, and in it on a host with
    # FEAT_SME.
    uml_add_arm64_unit_test(gadget_sme WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_STATIC_TEST_OBJECTS})
    target_link_libraries(uml_arm64_gadget_sme_tests PRIVATE
        uml_arm64_streaming_forms)

    uml_add_arm64_unit_test(gadget_successor_hoist_structure WITHOUT_HARNESS)
    uml_add_arm64_unit_test(static_runtime WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_STATIC_TEST_OBJECTS} LABELS tsan)
    uml_add_arm64_unit_test(static_boundary_state WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_STATIC_TEST_OBJECTS})
    uml_add_arm64_unit_test(static_translation WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_STATIC_TEST_OBJECTS})
    uml_add_arm64_unit_test(tlb WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_STATIC_TEST_OBJECTS})

    # The executable-memory owner's window near the image.
    uml_add_arm64_unit_test(jit_memory_window NAME arm64-jit-memory-window
        WITHOUT_HARNESS OBJECTS $<TARGET_OBJECTS:uml_cpu_executable_memory>)

    foreach(JIT_CACHE_TEST IN ITEMS cache_geometry shared_translation_cache)
        uml_add_arm64_unit_test(jit_${JIT_CACHE_TEST} WITHOUT_HARNESS NO_OBJECTS
            LABELS tsan)
        target_link_libraries(uml_arm64_jit_${JIT_CACHE_TEST}_tests PRIVATE
            uml_cpu_jit_component)
    endforeach()
    target_link_libraries(uml_arm64_jit_shared_translation_cache_tests PRIVATE
        uml_test_allocation_failure)

    uml_add_arm64_unit_test(jit_executor_pool WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_DIAGNOSTIC_TEST_OBJECTS} LABELS tsan)
    uml_add_arm64_unit_test(single_step WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_executor_pool_multislot WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_DIAGNOSTIC_TEST_OBJECTS} LABELS tsan)
    foreach(JIT_POOL_TEST IN ITEMS executor_pool executor_pool_multislot)
        target_compile_definitions(uml_arm64_jit_${JIT_POOL_TEST}_tests PRIVATE
            UML_JIT_RUNTIME_DIAGNOSTICS=1)
    endforeach()

    # These keep their underscores in the test name: arm64-jit-cache_validation.
    foreach(JIT_TEST IN ITEMS
            runtime
            capability
            cache_validation
            barrier_codegen
            barrier_runtime
            illegal_runtime
            publication_gate
            reservation_alias
            exclusive_completion)
        set(_jit_test_objects ${UML_ARM64_RUNTIME_TEST_OBJECTS})
        if(JIT_TEST STREQUAL "cache_validation")
            set(_jit_test_objects ${UML_ARM64_RUNTIME_DIAGNOSTIC_TEST_OBJECTS})
        endif()
        uml_add_arm64_unit_test(jit_${JIT_TEST} NAME arm64-jit-${JIT_TEST}
            WITHOUT_HARNESS OBJECTS ${_jit_test_objects})
    endforeach()
    target_compile_definitions(uml_arm64_jit_cache_validation_tests PRIVATE
        UML_JIT_RUNTIME_DIAGNOSTICS=1)
    target_link_libraries(uml_arm64_jit_cache_validation_tests PRIVATE
        uml_test_allocation_failure)
    set_property(TEST arm64-jit-publication_gate APPEND PROPERTY LABELS tsan)

    # The C entry points the guest kernel calls live in the guest selector.
    uml_add_arm64_unit_test(advertised_features WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    uml_add_arm64_unit_test(jit_lazy_flags_runtime WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_lazy_flags_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    foreach(JIT_DIAGNOSED_TEST IN ITEMS indirect_link chain_abi)
        uml_add_arm64_unit_test(jit_${JIT_DIAGNOSED_TEST}_codegen WITHOUT_HARNESS
            OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
        uml_add_arm64_unit_test(jit_${JIT_DIAGNOSED_TEST}_runtime WITHOUT_HARNESS
            OBJECTS ${UML_ARM64_RUNTIME_DIAGNOSTIC_TEST_OBJECTS})
        target_compile_definitions(
            uml_arm64_jit_${JIT_DIAGNOSED_TEST}_runtime_tests PRIVATE
            UML_JIT_RUNTIME_DIAGNOSTICS=1)
    endforeach()
    target_sources(uml_arm64_jit_chain_abi_runtime_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/arm64_jit_chain_abi_probe.S)
    foreach(JIT_EXACT_RETIREMENT_TEST IN ITEMS codegen runtime)
        uml_add_arm64_unit_test(
            jit_exact_retirement_${JIT_EXACT_RETIREMENT_TEST} WITHOUT_HARNESS
            OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    endforeach()

    uml_add_arm64_unit_test(jit_fp_link_runtime WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    # Top-byte ignore selects a translation and strips inline (cpu/arm64/jit).
    uml_add_arm64_unit_test(jit_top_byte_ignore WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    # The JIT's AArch32 support (cpu/arm64/jit/a32).
    uml_add_arm64_unit_test(jit_a32_runtime WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_a32_data_processing WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_COUNTING_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_a32_memory WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_COUNTING_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_a32_exclusive WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_COUNTING_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_a32_acquire_release WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_COUNTING_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_a32_encoder WITHOUT_HARNESS NO_OBJECTS)
    uml_add_arm64_unit_test(jit_a32_vector_encoder WITHOUT_HARNESS NO_OBJECTS)
    uml_add_arm64_unit_test(jit_a32_control WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    # The executor's miss, tracked-store, single-step and fetch-fault paths
    # in both instruction sets.
    uml_add_arm64_unit_test(jit_executor_paths WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_a32_vector WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_COUNTING_RUNTIME_TEST_OBJECTS})
    # The AArch32 instruction corpus (aarch32_corpus/README.md) on the JIT.
    uml_add_arm64_unit_test(jit_a32_corpus WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_COUNTING_RUNTIME_TEST_OBJECTS}
        ARGS ${UML_ARM64_AARCH32_CORPUS})
    add_dependencies(uml_arm64_jit_a32_corpus_tests uml_arm64_aarch32_corpus)
    # The exclusive records with the host interrupting the guest's thread
    # throughout. A store-exclusive may fail without cause (a host interrupt
    # ends the monitor the JIT uses) and the corpus's single attempts must
    # tolerate it; this lane makes that happen so the tolerance is measured
    # and not left to the load a gate happens to run under.
    uml_add_unit_test(NAME arm64-jit-a32-corpus-interrupted TIMEOUT 1200
        COMMAND uml_arm64_jit_a32_corpus_tests ${UML_ARM64_AARCH32_CORPUS}
            --record exclusive-qemu --passes 3000 --interrupt)

    # The JIT's SME support (cpu/arm64/jit, sme_support).
    uml_add_arm64_unit_test(jit_sme_mode WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_DIAGNOSTIC_TEST_OBJECTS})
    target_compile_definitions(uml_arm64_jit_sme_mode_tests PRIVATE
        UML_JIT_RUNTIME_DIAGNOSTICS=1)
    uml_add_arm64_unit_test(jit_sme_control WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_sme_register WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_sme_memory WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_sme_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS}
        ARGS ${CMAKE_CURRENT_SOURCE_DIR}/a64_instruction_evidence_bindings.tsv)

    uml_add_arm64_unit_test(counter_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    uml_add_arm64_unit_test(jit_counter_runtime WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    uml_add_arm64_unit_test(jit_register_remap_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    foreach(JIT_ONE_TO_ONE_TEST IN ITEMS base fp_simd)
        uml_add_arm64_unit_test(
            jit_one_to_one_${JIT_ONE_TO_ONE_TEST}_codegen WITHOUT_HARNESS
            NAME arm64-jit-one-to-one-${JIT_ONE_TO_ONE_TEST}-codegen
            OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    endforeach()

    uml_add_arm64_unit_test(jit_tlb_miss_preservation
        OBJECTS
            ${UML_STATIC_RUNTIME_TEST_OBJECTS}
            $<TARGET_OBJECTS:uml_arm64_execution_helpers>)

    # Every inline TLB probe of both engines hits the entry the C++ fill
    # path placed, in each way of its set, and misses another page.
    uml_add_arm64_unit_test(tlb_probe_set WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    uml_add_arm64_unit_test(jit_sp_memory_run_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    uml_add_arm64_unit_test(jit_sp_memory_run_runtime NO_OBJECTS)

    uml_add_arm64_unit_test(jit_gpr_memory_run_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    uml_add_arm64_unit_test(jit_gpr_memory_run_runtime NO_OBJECTS)

    uml_add_arm64_unit_test(jit_simd_memory_run_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})

    uml_add_arm64_unit_test(jit_simd_memory_run_runtime NO_OBJECTS)

    uml_add_arm64_unit_test(jit_mops_run_runtime NO_OBJECTS)

    uml_add_arm64_unit_test(jit_scalar_atomic_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_simd_structure_remap_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_vector_remap_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS}
        SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/arm64_jit_vector_remap_pressure_tests.cpp)
    uml_add_arm64_unit_test(jit_table_list_remap_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_pair_remap_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(jit_control_remap_codegen WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_RUNTIME_TEST_OBJECTS})
    uml_add_arm64_unit_test(gadget_runtime WITHOUT_HARNESS)

    # What the gadget-runtime tests link to drive an executor over a static
    # address space. The static MMU and gadget objects precede the runtime's,
    # unlike UML_ARM64_STATIC_TEST_OBJECTS.
    set(UML_ARM64_GADGET_RUNTIME_TEST_OBJECTS
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_aarch32_decoder>
        $<TARGET_OBJECTS:uml_arm64_aarch32_semantics>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>
        ${UML_STATIC_RUNTIME_TEST_OBJECTS})
    foreach(GADGET_RUNTIME_TEST IN ITEMS cache_validation inline_fallthrough)
        uml_add_arm64_unit_test(gadget_${GADGET_RUNTIME_TEST} WITHOUT_HARNESS
            OBJECTS ${UML_ARM64_GADGET_RUNTIME_TEST_OBJECTS})
    endforeach()
    # The AArch32 gadget tests read the library-exit counters, which only the
    # semantic library's counting variant has.
    set(UML_ARM64_COUNTING_GADGET_RUNTIME_TEST_OBJECTS
        ${UML_ARM64_GADGET_RUNTIME_TEST_OBJECTS})
    list(TRANSFORM UML_ARM64_COUNTING_GADGET_RUNTIME_TEST_OBJECTS REPLACE
        "uml_arm64_aarch32_semantics>"
        "uml_arm64_aarch32_semantics_exit_counting>")
    uml_add_arm64_unit_test(gadget_a32_control WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_COUNTING_GADGET_RUNTIME_TEST_OBJECTS})
    # The forms test reads the decoder's stored sample words.
    uml_add_arm64_unit_test(gadget_a32_forms WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_COUNTING_GADGET_RUNTIME_TEST_OBJECTS}
        ARGS ${CMAKE_CURRENT_SOURCE_DIR}/aarch32_decoder)
    # The engine against the AArch32 corpus, at the level it implements.
    uml_add_arm64_unit_test(gadget_a32_corpus WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_COUNTING_GADGET_RUNTIME_TEST_OBJECTS}
        ARGS ${UML_ARM64_AARCH32_CORPUS})
    add_dependencies(uml_arm64_gadget_a32_corpus_tests uml_arm64_aarch32_corpus)
    # The exclusive records with the host interrupting the guest's thread
    # throughout. A store-exclusive may fail without cause (a host interrupt
    # ends the monitor the gadgets use) and the corpus's single attempts must
    # tolerate it; this lane makes that happen so the tolerance is measured
    # and not left to the load a gate happens to run under. The sequences
    # the gadgets translate together run under the same interrupts.
    uml_add_unit_test(NAME arm64-gadget-a32-corpus-interrupted
        COMMAND uml_arm64_gadget_a32_corpus_tests ${UML_ARM64_AARCH32_CORPUS}
            --record exclusive-qemu,sequences-qemu --passes 200 --interrupt)
    target_compile_definitions(uml_arm64_gadget_cache_validation_tests PRIVATE
        UML_GADGET_CACHE_SLOTS=${UML_GADGET_CACHE_SLOT_COUNT})
    # Executors sharing one address space's translations, from several
    # threads at once.
    uml_add_arm64_unit_test(gadget_shared_bodies WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_GADGET_RUNTIME_TEST_OBJECTS}
        LABELS tsan)

    # A shared address space bounds the freed arena it accumulates by renewal.
    # The test links the executor variant that schedules renewals and exposes
    # what a space holds, in place of the production one.
    set(UML_ARM64_GADGET_STRESS_TEST_OBJECTS
        ${UML_ARM64_GADGET_RUNTIME_TEST_OBJECTS})
    list(TRANSFORM UML_ARM64_GADGET_STRESS_TEST_OBJECTS REPLACE
        "uml_arm64_gadget_executor>"
        "uml_arm64_gadget_sweep_stress_executor>")
    uml_add_arm64_unit_test(gadget_shared_bound WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_GADGET_STRESS_TEST_OBJECTS}
        LABELS tsan)
    target_compile_definitions(uml_arm64_gadget_shared_bound_tests PRIVATE
        UML_GADGET_SWEEP_STRESS=1)

    # Executors let go of the spaces of generations the MMU destroyed.
    uml_add_arm64_unit_test(gadget_dead_space WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_GADGET_STRESS_TEST_OBJECTS}
        LABELS tsan)
    target_compile_definitions(uml_arm64_gadget_dead_space_tests PRIVATE
        UML_GADGET_SWEEP_STRESS=1)
    target_link_libraries(uml_arm64_gadget_dead_space_tests PRIVATE
        uml_test_allocation_failure)

    # The process's gadget-cache budget bounds what the executors' address
    # spaces keep, from several threads at once.
    uml_add_arm64_unit_test(gadget_budget WITHOUT_HARNESS
        OBJECTS ${UML_ARM64_GADGET_RUNTIME_TEST_OBJECTS}
        LABELS tsan)
    foreach(GADGET_RUNTIME_TEST IN ITEMS cache_validation shared_bodies
            shared_bound dead_space budget)
        target_compile_definitions(uml_arm64_gadget_${GADGET_RUNTIME_TEST}_tests
            PRIVATE UML_GADGET_RESIDENT_GENERATIONS=${UML_GADGET_RESIDENT_GENERATIONS})
    endforeach()

    # The body arena on its own, against a model of what it hands out.
    uml_add_arm64_unit_test(gadget_body_arena WITHOUT_HARNESS NO_OBJECTS)

    uml_add_arm64_unit_test(gadget_hot_path WITHOUT_HARNESS)
    uml_add_arm64_unit_test(gadget_packed_immediate WITHOUT_HARNESS)
    uml_add_arm64_unit_test(gadget_test_bit_branch WITHOUT_HARNESS)
    uml_add_arm64_unit_test(gadget_nzcv_residency WITHOUT_HARNESS)
    uml_add_arm64_unit_test(gadget_fp_environment WITHOUT_HARNESS)
    uml_add_arm64_unit_test(gadget_afp_twin WITHOUT_HARNESS)

    uml_add_arm64_unit_test(gadget_dependency NAME arm64-gadget-dependencies
        WITHOUT_HARNESS
        SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/arm64_gadget_dependency_reference.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/arm64_gadget_dependency_memory_tests.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/arm64_gadget_bitfield_shift_tests.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/arm64_gadget_vector_dependency_tests.cpp)
    target_link_libraries(uml_arm64_gadget_dependency_tests PRIVATE
        UML::CpuContract uml_arm64_engine_test_support)

    # The test's exit hold must be registered before any destructor it
    # outlasts: keep engine state lazy or in libraries linked after it.
    uml_add_arm64_unit_test(gadget_exit WITHOUT_HARNESS)
    uml_add_arm64_unit_test(gadget_link NAME arm64-gadget-links WITHOUT_HARNESS)
    uml_add_arm64_unit_test(gadget_indirect_link NAME arm64-gadget-indirect-links
        WITHOUT_HARNESS)

    add_executable(uml_gadget_record_table_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/gadget_record_table_tests.cpp)
    target_link_libraries(uml_gadget_record_table_tests PRIVATE
		UML::MemoryHeap)
    target_include_directories(uml_gadget_record_table_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin)
    uml_add_unit_test(NAME gadget-record-table
		COMMAND uml_gadget_record_table_tests)

    # The window of address spaces a gadget cache holds, on its own.
    add_executable(uml_gadget_generation_window_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/gadget_generation_window_tests.cpp)
    target_include_directories(uml_gadget_generation_window_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin)
    uml_add_unit_test(NAME gadget-generation-window
		COMMAND uml_gadget_generation_window_tests)

    # The process's byte budget for the gadget caches, on its own.
    add_executable(uml_gadget_cache_budget_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/gadget_cache_budget_tests.cpp)
    target_include_directories(uml_gadget_cache_budget_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin)
    uml_add_unit_test(NAME gadget-cache-budget
		COMMAND uml_gadget_cache_budget_tests
		LABELS tsan)

    # The limbo a shared address space keeps its unpublished bodies in, on
    # its own, with every allocation of a reclaim failed in turn.
    add_executable(uml_gadget_shared_space_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/gadget_shared_space_tests.cpp)
    target_include_directories(uml_gadget_shared_space_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin)
    target_link_libraries(uml_gadget_shared_space_tests PRIVATE
		uml_test_allocation_failure)
    uml_add_unit_test(NAME gadget-shared-space
		COMMAND uml_gadget_shared_space_tests)

    # What the gadget caches' census records and writes, on its own: it never
    # fails the cache operation it records from, with every allocation it
    # makes failed, and leaves the guest's floating-point state as it was.
    add_executable(uml_gadget_cache_census_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/gadget_cache_census_tests.cpp)
    target_include_directories(uml_gadget_cache_census_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin)
    target_link_libraries(uml_gadget_cache_census_tests PRIVATE
		uml_test_allocation_failure)
    uml_add_unit_test(NAME gadget-cache-census
		COMMAND uml_gadget_cache_census_tests)
