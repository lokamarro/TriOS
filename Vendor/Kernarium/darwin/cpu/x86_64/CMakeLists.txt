# SPDX-License-Identifier: GPL-2.0-or-later

# What every x86-64 engine shares: the guest CPU state, the decoder, CPUID,
# the software TLB entry, the code-page registry, the process-wide lock for
# locked operands across a 16-byte granule, the segment model and the SIMD
# floating-point model. Its engine kind lets each engine link it without
# linking another engine.
add_library(uml_x86_64_target OBJECT
    cpu_state.cpp
    decoder.cpp
    code_page_registry.cpp
    fp_model.cpp
    segments.cpp
    split_operand_lock.cpp)
uml_declare_architecture_target(uml_x86_64_target cpu shared)
target_link_libraries(uml_x86_64_target
    PUBLIC UML::ThirdPartySoftFloat
    PRIVATE UML::CpuContract)
# The model is not translation-path code: it runs as the JIT's cold path always
# compiled it, apart from the interprocedural optimization of the target.
set_source_files_properties(fp_model.cpp PROPERTIES COMPILE_OPTIONS -fno-lto)
target_include_directories(uml_x86_64_target PUBLIC
    ${PROJECT_SOURCE_DIR}/darwin)

add_subdirectory(gadgets)
if(UML_BUILD_X86_64_JIT)
    add_subdirectory(jit)
endif()
