# SPDX-License-Identifier: GPL-2.0-or-later

# Every macOS executable shares one bundle identifier. Metal keeps its shader
# cache below the process's bundle identifier, and a process without one shares
# the cache of every other command-line process of the user. The Linux and the
# Android executables compile the same Mesa and KosmicKrisp shaders, so they
# share a cache as well.
set(UML_MACOS_APP_BUNDLE_IDENTIFIER "com.imxieyi.uml-darwin")

function(uml_add_macos_embedded_kernel_app
        TARGET_NAME MAIN_SOURCE ARTIFACT_TARGET)
    if(NOT TARGET "${ARTIFACT_TARGET}")
        message(FATAL_ERROR
            "${TARGET_NAME} requires the artifact target ${ARTIFACT_TARGET}")
    endif()
    get_target_property(_uml_kernel_image "${ARTIFACT_TARGET}" UML_KERNEL_IMAGE)
    if(NOT _uml_kernel_image)
        message(FATAL_ERROR "${ARTIFACT_TARGET} has no kernel ELF image")
    endif()

    # The loader aliases the ELF's executable pages from the section this
    # links in, as Kernarium's runtime does, so the image has to be safe for
    # that.
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -B
            "${PROJECT_SOURCE_DIR}/Kernarium/scripts/verify-signed-kernel-carrier.py"
            --image "${_uml_kernel_image}"
        RESULT_VARIABLE _uml_carrier_result
        OUTPUT_VARIABLE _uml_carrier_output
        ERROR_VARIABLE _uml_carrier_error)
    if(NOT _uml_carrier_result EQUAL 0)
        message(FATAL_ERROR
            "${TARGET_NAME}: the kernel is unsafe for the signed carrier:\n"
            "${_uml_carrier_output}${_uml_carrier_error}")
    endif()

    add_executable("${TARGET_NAME}"
        launcher.hpp
        main.cpp
        "${MAIN_SOURCE}")
    uml_declare_architecture_target("${TARGET_NAME}" app)
    target_include_directories("${TARGET_NAME}" PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/emulator/src
        ${PROJECT_SOURCE_DIR}/darwin/device)
    add_dependencies("${TARGET_NAME}"
        "${ARTIFACT_TARGET}"
        uml_dependency_prefix_verify)
    set_property(TARGET "${TARGET_NAME}" APPEND PROPERTY LINK_DEPENDS
        "${_uml_kernel_image}"
        "${PROJECT_SOURCE_DIR}/dependencies/verify-runtime-dependencies.sh"
        "${UML_DEPENDENCY_PREFIX}/share/uml-dependencies/receipt.tsv")
    set(_uml_gl_stage_commands)
    foreach(_uml_gl_framework IN LISTS UML_APPLE_GPU_GL_FRAMEWORK_NAMES)
        set_property(TARGET "${TARGET_NAME}" APPEND PROPERTY LINK_DEPENDS
            "${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/${_uml_gl_framework}.framework/${_uml_gl_framework}"
            "${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/${_uml_gl_framework}.framework/Info.plist")
        list(APPEND _uml_gl_stage_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${UML_APPLE_GPU_FRAMEWORK_DIRECTORY}/${_uml_gl_framework}.framework
                $<TARGET_FILE_DIR:${TARGET_NAME}>/Frameworks/${_uml_gl_framework}.framework)
    endforeach()
    set(_uml_kosmickrisp_stage_commands)
    if(TARGET uml_third_party_kosmickrisp)
        set_property(TARGET "${TARGET_NAME}" APPEND PROPERTY LINK_DEPENDS
            "${UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK}/KosmicKrisp"
            "${UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK}/Info.plist")
        list(APPEND _uml_kosmickrisp_stage_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK}
                $<TARGET_FILE_DIR:${TARGET_NAME}>/Frameworks/KosmicKrisp.framework)
    endif()
    target_link_libraries("${TARGET_NAME}" PRIVATE
        UML::Emulator
        UML::AudioDevice
        UML::BatteryDevice
        UML::VideoDevice
        UML::ConsoleDevice
        UML::VsockDevice
        UML::GpuDevice
        UML::GpuMetalShaderCache
        UML::GpuMesaShaderCache
        UML::VirtioInput)
    if(TARGET uml_network_provider)
        target_link_libraries("${TARGET_NAME}" PRIVATE UML::NetworkProvider)
    endif()
    target_compile_definitions("${TARGET_NAME}" PRIVATE
        UML_APP_HAS_SLIRP=$<BOOL:${UML_BUILD_NETWORK_PROVIDER}>)
    # The executable carries its property list in __TEXT,__info_plist, where
    # CoreFoundation reads the identifier of a process that is not an
    # application bundle.
    set(UML_MACOS_APP_NAME "${TARGET_NAME}")
    set(_uml_info_plist "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}-Info.plist")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in" "${_uml_info_plist}"
        @ONLY)
    set_property(TARGET "${TARGET_NAME}" APPEND PROPERTY LINK_DEPENDS
        "${_uml_info_plist}")
    set_property(GLOBAL APPEND PROPERTY UML_MACOS_APP_TARGETS "${TARGET_NAME}")
    target_link_options("${TARGET_NAME}" PRIVATE
        "LINKER:-export_dynamic"
        # The embedded ELF resolves this fixed-arity ABI with dlsym(). These
        # symbols have no native Mach-O callers, so force-load the deliberately
        # aggregated emulator product at the application boundary.
        "LINKER:-force_load,$<TARGET_FILE:uml_emulator>"
        # The kernel ELF is a read-only, host-page aligned __TEXT section:
        # the emulator runs it in place instead of copying it into the heap,
        # and the loader aliases its executable pages from it.
        "LINKER:-sectcreate,__TEXT,__uml_elf,${_uml_kernel_image}"
        "LINKER:-sectalign,__TEXT,__uml_elf,0x4000"
        "LINKER:-sectcreate,__TEXT,__info_plist,${_uml_info_plist}")

    # `<name>-dsym` writes the executable's dSYM beside it, with the kernel's
    # function symbols at their place in __TEXT,__uml_elf, where the runtime
    # moves a panic's kernel frames (runtime/kernel_crash_frames). It is not
    # part of the default build.
    add_custom_target("${TARGET_NAME}-dsym"
        COMMAND xcrun dsymutil "$<TARGET_FILE:${TARGET_NAME}>"
            -o "$<TARGET_FILE:${TARGET_NAME}>.dSYM"
        COMMAND "${Python3_EXECUTABLE}" -B
            "${PROJECT_SOURCE_DIR}/scripts/kernel-dsym-symbols.py"
            --dsym "$<TARGET_FILE:${TARGET_NAME}>.dSYM/Contents/Resources/DWARF/$<TARGET_FILE_NAME:${TARGET_NAME}>"
            --elf "${_uml_kernel_image}"
        VERBATIM)
    uml_declare_architecture_target("${TARGET_NAME}-dsym" app)
    add_dependencies("${TARGET_NAME}-dsym" "${TARGET_NAME}")

    set_target_properties("${TARGET_NAME}" PROPERTIES
        UML_BUNDLE_IDENTIFIER "${UML_MACOS_APP_BUNDLE_IDENTIFIER}"
        RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/Frameworks"
        INSTALL_RPATH_USE_LINK_PATH FALSE)
    add_custom_command(TARGET "${TARGET_NAME}" POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            $<TARGET_FILE_DIR:${TARGET_NAME}>/Frameworks
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:${TARGET_NAME}>/Frameworks
        ${_uml_gl_stage_commands}
        ${_uml_kosmickrisp_stage_commands}
        COMMAND ${PROJECT_SOURCE_DIR}/dependencies/verify-runtime-dependencies.sh
            --binary $<TARGET_FILE:${TARGET_NAME}>
            --apple-gpu-prefix ${UML_APPLE_GPU_DEPENDENCY_PREFIX}
            --framework-directory $<TARGET_FILE_DIR:${TARGET_NAME}>/Frameworks
        COMMENT "Staging and checking the ${TARGET_NAME} Apple GPU runtime closure"
        VERBATIM)
endfunction()

if(UML_BUILD_CLI)
    uml_add_macos_embedded_kernel_app(
        uml-darwin linux_main.cpp "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}")
    # Linux guests boot the accepted base archive for their userspace
    # architecture plus a per-run archive the launcher writes; the Android
    # launcher boots Android's own ramdisk and embeds neither.
    include("${PROJECT_SOURCE_DIR}/tools/rootfs/boot-ramdisk/BootBase.cmake")
    if(UML_MAINLINE_KERNEL_SLICE STREQUAL "x86_64")
        uml_select_boot_base(x86_64 _uml_boot_base)
    else()
        uml_select_boot_base(aarch64 _uml_boot_base)
    endif()
    set(_uml_boot_init "${PROJECT_SOURCE_DIR}/tools/rootfs/boot-ramdisk/init")
    target_link_libraries(uml-darwin PRIVATE UML::BootInitramfs)
    target_link_options(uml-darwin PRIVATE
        "LINKER:-sectcreate,__TEXT,__uml_boot_base,${_uml_boot_base}"
        "LINKER:-sectcreate,__TEXT,__uml_boot_init,${_uml_boot_init}")
    set_property(TARGET uml-darwin APPEND PROPERTY LINK_DEPENDS
        "${_uml_boot_base}" "${_uml_boot_init}")
endif()

if(UML_BUILD_ANDROID17_CLI)
    uml_add_macos_embedded_kernel_app(
        uml-darwin-android17 android17_main.cpp
        "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}")
    target_link_libraries(uml-darwin-android17 PRIVATE UML::AndroidBoot)
endif()
