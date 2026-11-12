# SPDX-License-Identifier: GPL-2.0-or-later

# The macOS-only x86-64 JIT: FEXCore embedded behind the static MMU. Only the
# macOS prefix carries FEXCore, and only a macOS configuration may create this
# target (UML_BUILD_X86_64_JIT). Its engine kind lets it link the shared x86
# target and the executable-memory owner, never the gadget engine.
add_library(uml_x86_64_jit OBJECT fex_engine.cpp)
uml_declare_architecture_target(uml_x86_64_jit cpu native)
target_link_libraries(uml_x86_64_jit
    PUBLIC uml_x86_64_target
    PRIVATE
        UML::CpuContract
        uml_cpu_executable_memory
        UML::ThirdPartyFEXCore)
target_include_directories(uml_x86_64_jit PUBLIC
    ${PROJECT_SOURCE_DIR}/darwin)
