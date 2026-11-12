# SPDX-License-Identifier: GPL-2.0-or-later

# x86-64 code is translated to immutable streams of precompiled host gadgets.
# The macOS dependency prefix provides the exact math archives.
add_library(uml_x86_64_gadgets OBJECT
    engine.S
    exception_checks.cpp
    executor.cpp
    native_builder.cpp
    native_memory.cpp
    program.cpp
    translation_space.cpp
    x87_math.cpp)
uml_declare_architecture_target(uml_x86_64_gadgets cpu gadget)
# Every image that links these objects places their gadget region
# (engine/region.inc).
target_link_libraries(uml_x86_64_gadgets
    PUBLIC uml_x86_64_target UML::BridgeAbi UML::ThirdPartyCephes128
           UML::MemoryHeap
    PRIVATE UML::CpuContract
    INTERFACE UML::GadgetRegionLayout)
target_include_directories(uml_x86_64_gadgets PUBLIC
    ${PROJECT_SOURCE_DIR}/darwin)
target_compile_definitions(uml_x86_64_gadgets PRIVATE
    UML_GADGET_CACHE_CENSUS=$<BOOL:${UML_GADGET_CACHE_CENSUS}>
    UML_GADGET_CACHE_SLOTS=${UML_GADGET_CACHE_SLOT_COUNT}
    UML_GADGET_RESIDENT_GENERATIONS=${UML_GADGET_RESIDENT_GENERATIONS})

# The register-baked scalar bank is enumerated by a generator: one entry
# per operand combination is thousands of entries, which the assembler's
# own macro expansion takes minutes to produce.
set(_uml_x86_64_baked_dir ${CMAKE_CURRENT_BINARY_DIR}/x86_64_baked)
set(_uml_x86_64_baked_generator
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/generate_baked.py)
add_custom_command(
    OUTPUT ${_uml_x86_64_baked_dir}/baked_scalar.inc
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_uml_x86_64_baked_dir}
    COMMAND ${Python3_EXECUTABLE} -B ${_uml_x86_64_baked_generator}
        --output ${_uml_x86_64_baked_dir}/baked_scalar.inc
    DEPENDS ${_uml_x86_64_baked_generator}
    COMMENT "Generating the x86-64 register-baked gadget bank"
    VERBATIM)
set_source_files_properties(engine.S PROPERTIES
    OBJECT_DEPENDS ${_uml_x86_64_baked_dir}/baked_scalar.inc
    COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
target_include_directories(uml_x86_64_gadgets PRIVATE
    ${_uml_x86_64_baked_dir})
