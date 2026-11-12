# SPDX-License-Identifier: GPL-2.0-or-later

include("${PROJECT_SOURCE_DIR}/kernel/common/artifacts/KernelArtifact.cmake")

set(UML_KERNEL_PRODUCT "mainline-6.18" CACHE STRING
    "Prebuilt kernel product consumed by the selected Apple executable/runtime")
set_property(CACHE UML_KERNEL_PRODUCT PROPERTY STRINGS
    mainline-6.18 android-17)

# Apple targets consume the kernel artifact only; rootfs bundles are selected
# at launch and are not inputs to the emulator build. A descriptor only names
# its bundle, and the import verifies the bundle's own family and slice.
set(_uml_kernel_line_root "${PROJECT_SOURCE_DIR}/kernel/mainline-6.18")
if(UML_KERNEL_PRODUCT STREQUAL "android-17")
    set(_uml_kernel_artifact_target uml_android17_kernel_artifact)
    uml_import_accepted_kernel_artifact(
        TARGET "${_uml_kernel_artifact_target}"
        DESCRIPTOR "${_uml_kernel_line_root}/artifacts/accepted-android-4k.env"
        CACHE_VARIABLE UML_ANDROID17_KERNEL_ARTIFACT_DIR
        DESCRIPTION "Installed accepted Android 17 kernel artifact directory"
        FAMILY android-6.18
        SLICE arm64)
elseif(UML_KERNEL_PRODUCT STREQUAL "mainline-6.18")
    # The root rejects every slice other than arm64 and x86_64.
    if(UML_MAINLINE_KERNEL_SLICE STREQUAL "x86_64")
        set(_uml_mainline_acceptance
            "${_uml_kernel_line_root}/slices/x86_64/artifacts/accepted-4k.env")
    else()
        set(_uml_mainline_acceptance
            "${_uml_kernel_line_root}/artifacts/accepted-4k.env")
    endif()
    set(_uml_kernel_artifact_target uml_mainline_kernel_artifact)
    uml_import_accepted_kernel_artifact(
        TARGET "${_uml_kernel_artifact_target}"
        DESCRIPTOR "${_uml_mainline_acceptance}"
        CACHE_VARIABLE UML_MAINLINE_618_KERNEL_ARTIFACT_DIR
        DESCRIPTION "Installed accepted mainline-6.18 kernel artifact directory"
        FAMILY mainline-6.18
        SLICE "${UML_MAINLINE_KERNEL_SLICE}")
else()
    message(FATAL_ERROR
        "UML_KERNEL_PRODUCT must be mainline-6.18 or android-17")
endif()

set(UML_ACTIVE_KERNEL_ARTIFACT_TARGET "${_uml_kernel_artifact_target}"
    CACHE INTERNAL "Selected read-only kernel artifact target" FORCE)
