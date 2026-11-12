# SPDX-License-Identifier: GPL-2.0-or-later

if(UML_TARGET_MACOS)
    add_executable(uml_scalar_ceiling_bench EXCLUDE_FROM_ALL
        bench.cpp workloads.S
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_aarch32_decoder>
        $<TARGET_OBJECTS:uml_arm64_aarch32_semantics>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_link_libraries(uml_scalar_ceiling_bench PRIVATE
        UML::BridgeAbi
        UML::CpuContract UML::GadgetRegionLayout UML::MemorySharedBacking)
    target_include_directories(uml_scalar_ceiling_bench PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    target_compile_definitions(uml_scalar_ceiling_bench PRIVATE
        UML_SCALAR_CEILING_GADGETS=1)
endif()
