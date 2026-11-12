# SPDX-License-Identifier: GPL-2.0-or-later

# The guest kernel's AArch32 task support (run_guest_aarch32.py). The armhf
# program is built with the configured compiler's ARM backend and wrapped
# into an ELF32 executable by make_elf32.py, because the toolchain has no
# ARM linker. It is generated here and never committed. The test executes the
# program and checks that the one aligned to 4 KiB only runs.

set(_uml_aarch32_object "${CMAKE_CURRENT_BINARY_DIR}/aarch32_probe.o")
set(UML_GUEST_AARCH32_PROBE "${CMAKE_CURRENT_BINARY_DIR}/guest-aarch32-probe")
set(UML_GUEST_AARCH32_PROBE_4K
    "${CMAKE_CURRENT_BINARY_DIR}/guest-aarch32-probe-4k")
# The same program twice: aligned to 64 KiB, and to 4 KiB only, as 11 files of
# a stock Debian armhf image are.
add_custom_command(
    OUTPUT "${UML_GUEST_AARCH32_PROBE}" "${UML_GUEST_AARCH32_PROBE_4K}"
    COMMAND "${CMAKE_CXX_COMPILER}"
        --target=armv7-unknown-linux-gnueabihf
        -mfpu=neon-vfpv4
        -mfloat-abi=hard
        -c "${CMAKE_CURRENT_SOURCE_DIR}/aarch32_probe.S"
        -o "${_uml_aarch32_object}"
    COMMAND ${Python3_EXECUTABLE} -I
        "${CMAKE_CURRENT_SOURCE_DIR}/make_elf32.py"
        "${_uml_aarch32_object}" "${UML_GUEST_AARCH32_PROBE}"
    COMMAND ${Python3_EXECUTABLE} -I
        "${CMAKE_CURRENT_SOURCE_DIR}/make_elf32.py" --layout 4k
        "${_uml_aarch32_object}" "${UML_GUEST_AARCH32_PROBE_4K}"
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${UML_GUEST_AARCH32_PROBE}
        -P ${PROJECT_SOURCE_DIR}/cmake/VerifyGeneratedFile.cmake
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${UML_GUEST_AARCH32_PROBE_4K}
        -P ${PROJECT_SOURCE_DIR}/cmake/VerifyGeneratedFile.cmake
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/aarch32_probe.S"
        "${CMAKE_CURRENT_SOURCE_DIR}/make_elf32.py"
    COMMENT "Building the armhf AArch32 guest test programs"
    VERBATIM)
add_custom_target(uml-guest-aarch32-probe
    DEPENDS "${UML_GUEST_AARCH32_PROBE}" "${UML_GUEST_AARCH32_PROBE_4K}")
add_dependencies(integration-test-binaries uml-guest-aarch32-probe)

# The timeout is above the runner's longest path: boot, the probe's 900 s
# bound on the gadget engine, and the closing command.
uml_add_guest_integration_test(
    NAME guest-aarch32-kernel
    SCRIPT run_guest_aarch32.py
    ARGS --probe ${CMAKE_CURRENT_SOURCE_DIR}/aarch32_guest_probe.py
        --elf ${UML_GUEST_AARCH32_PROBE}
        --elf-4k ${UML_GUEST_AARCH32_PROBE_4K}
        --aarch64-helper ${CMAKE_CURRENT_SOURCE_DIR}/aarch64_dirty.s
    LABELS guest;kernel;aarch32;smoke
    TIMEOUT 1200
    CPUS 2)

# guest-aarch32-exclusives-<engine> runs the probe's exclusives check alone on
# an engine: every exclusive and ordered form on each of four CPUs, right
# after a 64-bit task left its registers there.
foreach(_engine IN ITEMS gadgets jit)
    uml_add_guest_integration_test(
        NAME guest-aarch32-exclusives-${_engine}
        SCRIPT run_guest_aarch32.py
        ARGS --engine ${_engine} --ncpus 4 --only exclusives
            --probe ${CMAKE_CURRENT_SOURCE_DIR}/aarch32_guest_probe.py
            --elf ${UML_GUEST_AARCH32_PROBE}
            --elf-4k ${UML_GUEST_AARCH32_PROBE_4K}
            --aarch64-helper ${CMAKE_CURRENT_SOURCE_DIR}/aarch64_dirty.s
            --probe-timeout 600
        LABELS guest;kernel;aarch32;${_engine}
        TIMEOUT 900
        CPUS 4)
endforeach()

# The engine probe: a freestanding armhf program (engine_probe/) that the
# AArch32 execution engines run as the guest's /init. Its C++ and assembly
# objects are compiled with the configured compiler's ARM backend and linked
# by link_elf32.py, because the toolchain has no ARM linker. Groups that need
# an extension the host lacks are skipped inside the probe from the HWCAP words
# it sees, so one program serves every host. Every run counts SIGILLs.
set(_uml_a32_probe_directory "${CMAKE_CURRENT_SOURCE_DIR}/engine_probe")
set(_uml_a32_probe_objects "${CMAKE_CURRENT_BINARY_DIR}/engine_probe")
set(_uml_a32_probe_header "${_uml_a32_probe_directory}/engine_probe.hpp")
set(_uml_a32_probe_cxx_flags
    --target=armv7-unknown-linux-gnueabihf
    -mcpu=cortex-a15 -mfpu=none -mfloat-abi=soft
    -std=c++20 -O2 -Wall -Wextra -Werror
    -ffreestanding -fno-builtin -fno-exceptions -fno-rtti
    -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
    -fno-pic -fno-pie -fno-vectorize -fno-slp-vectorize
    -fno-threadsafe-statics)
set(_uml_a32_probe_base_asm_flags
    --target=armv7-unknown-linux-gnueabihf
    -mcpu=cortex-a15 -mfpu=none -mfloat-abi=soft -Wa,--fatal-warnings)
set(_uml_a32_probe_simd_asm_flags
    --target=armv7-unknown-linux-gnueabihf
    -mcpu=cortex-a15 -mfpu=neon-vfpv4 -mfloat-abi=hard -Wa,--fatal-warnings)
set(_uml_a32_probe_complete_asm_flags
    --target=armv8.2a-unknown-linux-gnueabihf
    -march=armv8.2-a+dotprod+fp16fml+crypto+crc+fp16+bf16+i8mm
    -mfloat-abi=hard -Wa,--fatal-warnings)

set(_uml_a32_probe_object_files)
foreach(_name IN ITEMS runtime groups basic interwork signals kuser tls
        cacheflush faults ill ptrace swp atomics absent simd complete)
    set(_source "${_uml_a32_probe_directory}/engine_probe_${_name}.cpp")
    set(_object "${_uml_a32_probe_objects}/${_name}.o")
    add_custom_command(
        OUTPUT "${_object}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_uml_a32_probe_objects}"
        COMMAND "${CMAKE_CXX_COMPILER}" ${_uml_a32_probe_cxx_flags}
            -c "${_source}" -o "${_object}"
        DEPENDS "${_source}" "${_uml_a32_probe_header}"
        COMMENT "Compiling the AArch32 engine probe: ${_name}"
        VERBATIM)
    list(APPEND _uml_a32_probe_object_files "${_object}")
endforeach()
# Assembly: the simd file needs VFP and NEON, the complete ones the Armv8.2
# extensions, and the rest nothing but the integer instruction set.
foreach(_name IN ITEMS start threads basic interwork signals kuser
        cacheflush faults ill ptrace swp atomics simd absent_complete complete)
    if(_name MATCHES "_complete$|^complete$")
        set(_flags ${_uml_a32_probe_complete_asm_flags})
    elseif(_name STREQUAL "simd")
        set(_flags ${_uml_a32_probe_simd_asm_flags})
    else()
        set(_flags ${_uml_a32_probe_base_asm_flags})
    endif()
    set(_source "${_uml_a32_probe_directory}/engine_probe_${_name}.S")
    set(_object "${_uml_a32_probe_objects}/${_name}-s.o")
    add_custom_command(
        OUTPUT "${_object}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_uml_a32_probe_objects}"
        COMMAND "${CMAKE_CXX_COMPILER}" ${_flags}
            -c "${_source}" -o "${_object}"
        DEPENDS "${_source}"
        COMMENT "Assembling the AArch32 engine probe: ${_name}"
        VERBATIM)
    list(APPEND _uml_a32_probe_object_files "${_object}")
endforeach()

set(UML_GUEST_AARCH32_ENGINE_PROBE
    "${CMAKE_CURRENT_BINARY_DIR}/guest-aarch32-engine-probe")
set(UML_GUEST_AARCH32_ENGINE_PROBE_4K
    "${CMAKE_CURRENT_BINARY_DIR}/guest-aarch32-engine-probe-4k")
add_custom_command(
    OUTPUT "${UML_GUEST_AARCH32_ENGINE_PROBE}"
        "${UML_GUEST_AARCH32_ENGINE_PROBE_4K}"
    COMMAND ${Python3_EXECUTABLE} -I
        "${CMAKE_CURRENT_SOURCE_DIR}/link_elf32.py"
        "${UML_GUEST_AARCH32_ENGINE_PROBE}" ${_uml_a32_probe_object_files}
    COMMAND ${Python3_EXECUTABLE} -I
        "${CMAKE_CURRENT_SOURCE_DIR}/link_elf32.py" --layout 4k
        "${UML_GUEST_AARCH32_ENGINE_PROBE_4K}" ${_uml_a32_probe_object_files}
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${UML_GUEST_AARCH32_ENGINE_PROBE}
        -P ${PROJECT_SOURCE_DIR}/cmake/VerifyGeneratedFile.cmake
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${UML_GUEST_AARCH32_ENGINE_PROBE_4K}
        -P ${PROJECT_SOURCE_DIR}/cmake/VerifyGeneratedFile.cmake
    DEPENDS ${_uml_a32_probe_object_files}
        "${CMAKE_CURRENT_SOURCE_DIR}/link_elf32.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/make_elf32.py"
    COMMENT "Linking the AArch32 engine probe"
    VERBATIM)
add_custom_target(uml-guest-aarch32-engine-probe
    DEPENDS "${UML_GUEST_AARCH32_ENGINE_PROBE}"
        "${UML_GUEST_AARCH32_ENGINE_PROBE_4K}")
add_dependencies(integration-test-binaries uml-guest-aarch32-engine-probe)

# guest-aarch32-engine-<engine> runs the probe on an engine. The four-vCPU
# guest gives the exclusives real contention.
foreach(_engine IN ITEMS gadgets jit)
    if(_engine STREQUAL "gadgets")
        set(_timeout 2400)
    else()
        set(_timeout 1200)
    endif()
    uml_add_guest_integration_test(
        NAME guest-aarch32-engine-${_engine}
        SCRIPT run_guest_a32_engine.py
        ARGS --engine ${_engine}
            --probe ${UML_GUEST_AARCH32_ENGINE_PROBE}
            --probe-4k ${UML_GUEST_AARCH32_ENGINE_PROBE_4K}
            --timeout ${_timeout}
        LABELS guest;aarch32;kernel;${_engine}
        TIMEOUT ${_timeout}
        CPUS 4)
endforeach()
