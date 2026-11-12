# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME instruction-corpus-source
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_instruction_corpus_source.py
    LABELS cpu
    TIMEOUT 60)

# The SME image scan and idiom corpus classify the words SME code executes
# at PSTATE.SM = 0 against the SM=0 family, on synthetic ELF fixtures.
uml_add_tooling_test(
    NAME arm64-sme-sm0-scan
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_sme_sm0_scan.py
    LABELS cpu
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-instruction-inventory
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/validate_a64_instruction_inventory.py
    LABELS cpu
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-instruction-corpus-freshness
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_arm64_instruction_corpus.py
        ${UML_ARM64_INSTRUCTION_CORPUS}
    LABELS cpu
    TIMEOUT 600)

uml_add_tooling_test(
    NAME arm64-instruction-corpus-absent-features
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_arm64_instruction_corpus_absent_features.py
        ${UML_ARM64_INSTRUCTION_CORPUS}
    LABELS cpu
    TIMEOUT 300)

uml_add_tooling_test(
    NAME arm64-instruction-corpus-sme-native
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_arm64_instruction_corpus_sme_native.py
        ${UML_ARM64_INSTRUCTION_CORPUS}
    LABELS cpu
    TIMEOUT 300)

uml_add_tooling_test(
    NAME arm64-instruction-corpus-floating-trap-native
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_arm64_instruction_corpus_floating_trap_native.py
        ${UML_ARM64_INSTRUCTION_CORPUS}
    LABELS cpu
    TIMEOUT 300)

uml_add_tooling_test(
    NAME arm64-instruction-corpus-sm0-vl
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_arm64_instruction_corpus_sm0_vl.py
        ${UML_ARM64_INSTRUCTION_CORPUS}
    LABELS cpu
    TIMEOUT 300)

uml_add_tooling_test(
    NAME arm64-instruction-corpus-tbi-memory
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_arm64_instruction_corpus_tbi_memory.py
        ${UML_ARM64_INSTRUCTION_CORPUS}
    LABELS cpu
    TIMEOUT 300)

uml_add_tooling_test(
    NAME arm64-instruction-corpus-tbi-equivalence
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_arm64_instruction_corpus_tbi_equivalence.py
        ${UML_ARM64_INSTRUCTION_CORPUS}
    LABELS cpu
    TIMEOUT 300)

uml_add_tooling_test(
    NAME arm64-instruction-corpus-invalid-encoding
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_arm64_instruction_corpus_invalid_encoding.py
        ${UML_ARM64_INSTRUCTION_CORPUS}
    LABELS cpu
    TIMEOUT 300)

# Opt-in: the pinned LLVM MC is not a managed dependency. A case with a clear
# invalid-encoding bit names a valid encoding, so LLVM must decode its word.
uml_add_tooling_test(
    NAME arm64-instruction-corpus-invalid-encoding-llvm
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_arm64_instruction_corpus_invalid_encoding.py
        ${UML_ARM64_INSTRUCTION_CORPUS} --llvm
    LABELS cpu
    TIMEOUT 300)
set_tests_properties(arm64-instruction-corpus-invalid-encoding-llvm PROPERTIES
    SKIP_RETURN_CODE 77)

# The native oracle records the SME corpus cases on an SME host; this test
# drives its exception path and state transfer with base A64 cases.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_executable(uml_sme_native_oracle
        ${PROJECT_SOURCE_DIR}/tools/arm64-tests/sme_native_oracle.cpp)
    uml_add_tooling_test(
        NAME arm64-sme-native-oracle
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_sme_native_oracle.py
            $<TARGET_FILE:uml_sme_native_oracle>
        LABELS cpu
        TIMEOUT 60)
endif()

# The bounded generator runner's process-group rules (the generators below
# run their commands through it).
uml_add_tooling_test(
    NAME arm64-bounded-generation
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_bounded_generation.py
    LABELS cpu
    TIMEOUT 120)

# The AArch32 decoder's generated tables and test data agree with each other
# (offline; the generator's inputs are not needed).
uml_add_tooling_test(
    NAME arm64-a32-encoding-table
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a32_encoding_table.py
    LABELS cpu
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-a32-instruction-inventory
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/validate_a32_instruction_inventory.py
    LABELS cpu
    TIMEOUT 120)

# The AArch32 corpus's source tables generate the pack their provenance
# records, and its derived tables and coverage agree with them (offline).
uml_add_tooling_test(
    NAME arm64-a32-corpus
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a32_corpus.py
    LABELS cpu
    TIMEOUT 300)

# Opt-in: the pinned Arm XML archive is not a managed dependency.
set(UML_A64_ARCHITECTURE_ARCHIVE "" CACHE FILEPATH
    "Pinned Arm A64 XML archive for the A64 oracle and SME table freshness tests")
uml_add_tooling_test(
    NAME arm64-instruction-oracle-freshness
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_instruction_oracle_freshness.py
        --architecture-archive=${UML_A64_ARCHITECTURE_ARCHIVE}
    LABELS cpu
    TIMEOUT 900)
set_tests_properties(arm64-instruction-oracle-freshness PROPERTIES
    SKIP_RETURN_CODE 77)
uml_add_tooling_test(
    NAME arm64-sme-encoding-table-freshness
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_sme_encoding_table_freshness.py
        --architecture-archive=${UML_A64_ARCHITECTURE_ARCHIVE}
    LABELS cpu
    TIMEOUT 900)
set_tests_properties(arm64-sme-encoding-table-freshness PROPERTIES
    SKIP_RETURN_CODE 77)
set(UML_A32_ARCHITECTURE_ARCHIVE "" CACHE FILEPATH
    "Pinned Arm AArch32 XML archive for the AArch32 table freshness test")
uml_add_tooling_test(
    NAME arm64-a32-encoding-table-freshness
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a32_encoding_table_freshness.py
        --architecture-archive=${UML_A32_ARCHITECTURE_ARCHIVE}
    LABELS cpu
    TIMEOUT 1800)
set_tests_properties(arm64-a32-encoding-table-freshness PROPERTIES
    SKIP_RETURN_CODE 77)
uml_add_tooling_test(
    NAME arm64-a32-corpus-plan-freshness
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a32_corpus_plan_freshness.py
        --architecture-archive=${UML_A32_ARCHITECTURE_ARCHIVE}
    LABELS cpu
    TIMEOUT 900)
set_tests_properties(arm64-a32-corpus-plan-freshness PROPERTIES
    SKIP_RETURN_CODE 77)

# The pinned gadget region (darwin/cpu/arm64/gadgets/engine.S) in the linked
# images this tree builds: the product in an integration tree, and in a unit
# tree an image composed from the gadget objects and one linked through the
# emulator archive.
set(_uml_gadget_region_images)
foreach(_uml_gadget_region_image IN ITEMS
        uml-darwin
        uml_arm64_gadget_hot_path_tests
        uml_arm64_instruction_corpus_tests)
    if(TARGET ${_uml_gadget_region_image})
        list(APPEND _uml_gadget_region_images
            $<TARGET_FILE:${_uml_gadget_region_image}>)
    endif()
endforeach()
if(_uml_gadget_region_images AND APPLE)
    uml_add_tooling_test(
        NAME arm64-gadget-region-layout
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_gadget_region_layout.py
            ${_uml_gadget_region_images}
        LABELS cpu
        TIMEOUT 120)
    # Everything a program translated for streaming mode can reach runs no
    # AdvSIMD instruction and calls no helper while the host streams.
    uml_add_tooling_test(
        NAME arm64-gadget-streaming-safety
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_gadget_streaming_safety.py
            ${_uml_gadget_region_images}
        LABELS cpu
        TIMEOUT 450)
endif()

# Both gadget engines name their private labels with Mach-O's temporary L
# prefix (docs/architecture.md). The engine target name ahead of each object
# makes the tooling build assemble it.
set(_uml_gadget_engine_objects)
foreach(_uml_gadget_engine IN ITEMS uml_arm64_gadgets uml_x86_64_gadgets)
    if(TARGET ${_uml_gadget_engine})
        list(APPEND _uml_gadget_engine_objects ${_uml_gadget_engine}
            "$<FILTER:$<TARGET_OBJECTS:${_uml_gadget_engine}>,INCLUDE,/engine[.]S[.]o$>")
    endif()
endforeach()
if(_uml_gadget_engine_objects)
    uml_add_tooling_test(
        NAME gadget-engine-temporary-labels
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_gadget_engine_labels.py
            ${_uml_gadget_engine_objects}
        LABELS cpu
        TIMEOUT 60)
endif()
