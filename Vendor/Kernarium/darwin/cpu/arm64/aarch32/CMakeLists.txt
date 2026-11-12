# SPDX-License-Identifier: GPL-2.0-or-later

# The AArch32 decoder: one generated encoding table and the code that reads it
# (README.md). It is engine-neutral and holds no state,
# so the gadget engine, the JIT and the semantic library can all link it; it
# includes no engine.
add_library(uml_arm64_aarch32_decoder OBJECT
    a32_encodings.cpp
    a32_operand_decode.cpp
    decode.cpp
    form_operation.cpp)
uml_declare_architecture_target(uml_arm64_aarch32_decoder cpu shared)
target_include_directories(uml_arm64_aarch32_decoder PUBLIC
    ${PROJECT_SOURCE_DIR}/darwin)

# The AArch32 semantic library (semantics.hpp): the engines' slow paths and
# the corpus's model lane execute any instruction of the profile through it.
# It works on CpuState alone and includes no engine, so it is a shared CPU
# target.
set(_uml_arm64_aarch32_semantics_sources
    semantics.cpp
    semantics_armv8_float.cpp
    semantics_armv8_integer.cpp
    semantics_bf16.cpp
    semantics_control.cpp
    semantics_crypto.cpp
    semantics_integer.cpp
    semantics_memory.cpp
    semantics_neon_integer.cpp
    semantics_neon_shift.cpp
    semantics_table.cpp
    semantics_vector_float.cpp
    semantics_vector_memory.cpp
    semantics_vector_move.cpp)

# The library and its unit-test variant differ in one definition: the variant
# counts the execute_a32() calls per form (semantics_exit_counters.hpp), which
# the tests that gate a lowered form's staying out of the library read.
# Ordinary products and the other tests use the library below, in which the
# counting site is preprocessed out.
function(_uml_add_aarch32_semantics TARGET EXIT_COUNTERS)
    add_library(${TARGET} OBJECT
        ${_uml_arm64_aarch32_semantics_sources} ${ARGN})
    uml_declare_architecture_target(${TARGET} cpu shared)
    target_link_libraries(${TARGET} PRIVATE
        UML::BridgeAbi
        UML::CpuContract)
    target_include_directories(${TARGET} PUBLIC
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_definitions(${TARGET} PRIVATE
        UML_A32_LIBRARY_EXIT_COUNTERS=${EXIT_COUNTERS})
endfunction()

_uml_add_aarch32_semantics(uml_arm64_aarch32_semantics 0)
if(UML_BUILD_UNIT_TESTS)
    _uml_add_aarch32_semantics(uml_arm64_aarch32_semantics_exit_counting 1
        semantics_exit_counters.cpp)
endif()
