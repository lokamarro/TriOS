# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME dependency-source-preparer
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_prepare.sh
    LABELS dependency
    # The paired Vulkan coverage prepares two further fixture repositories and
    # replays their series through several rejection cases.
    TIMEOUT 1200)

uml_add_tooling_test(
    NAME dependency-patch-regenerator
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_rebase_patches.sh
    LABELS dependency
    TIMEOUT 120)

uml_add_tooling_test(
    NAME dependency-build-manifest
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_build_manifest.sh
    LABELS dependency
    # Six manifest checks each authenticate every generated patch byte, and the
    # experimental Vulkan product adds two large series to that inventory.
    TIMEOUT 180)

uml_add_tooling_test(
    NAME dependency-product-manifest-verifier
    COMMAND ${CMAKE_COMMAND}
        -DVERIFY_SCRIPT=${PROJECT_SOURCE_DIR}/dependencies/cmake/VerifyManifestProduct.cmake
        -P ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_manifest_product_verifier.cmake
    LABELS dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME dependency-prefix-verifier
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_verify_prefix.sh
    LABELS dependency
    TIMEOUT 600)

uml_add_tooling_test(
    NAME dependency-prefix-publisher
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_publish_prefix.sh
    LABELS dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME dependency-apple-gpu-vulkan-prefix-verifier
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_verify_apple_gpu_vulkan_prefix.sh
    LABELS dependency gpu
    TIMEOUT 450)

uml_add_tooling_test(
    NAME dependency-apple-gpu-software-prefix-verifier
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_verify_apple_gpu_software_prefix.sh
    LABELS dependency gpu
    TIMEOUT 300)

uml_add_tooling_test(
    NAME dependency-ios-network-prefix-verifier
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_verify_ios_network_prefix.sh
    LABELS dependency
    TIMEOUT 120)

uml_add_tooling_test(
    NAME dependency-ios-static-xcframework-verifier
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_verify_ios_static_xcframework.sh
    LABELS dependency
    TIMEOUT 60)

uml_add_tooling_test(
    NAME dependency-kernel-host-tools-prefix-verifier
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_verify_kernel_host_tools_prefix.sh
    LABELS dependency
    TIMEOUT 60)

uml_add_tooling_test(
    NAME dependency-runtime-closure
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_verify_runtime_dependencies.sh
    LABELS dependency
    TIMEOUT 90)

uml_add_tooling_test(
    NAME dependency-verify-read-only
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_verify_read_only.sh
    LABELS dependency
    # Six refused builds each validate the dependency manifest first.
    TIMEOUT 900)

uml_add_tooling_test(
    NAME dependency-relative-paths
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_relative_paths.sh
    LABELS dependency
    TIMEOUT 60)

uml_add_tooling_test(
    NAME dependency-kosmickrisp-spirv
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/dependencies/test_kosmickrisp_spirv.py
    LABELS dependency gpu
    TIMEOUT 60)

# The full comparison ships a checkout of the committed Mesa series to the
# named Debian build host, rebuilds mesa_clc there and compiles both blobs
# again, so it registers only when that host is named.
if(UML_KOSMICKRISP_SPIRV_BUILD_HOST)
    if(NOT UML_KOSMICKRISP_SPIRV_MESA_SOURCE)
        set(UML_KOSMICKRISP_SPIRV_MESA_SOURCE
            "${PROJECT_SOURCE_DIR}/external/sources/mesa")
    endif()
    uml_add_tooling_test(
        NAME dependency-kosmickrisp-spirv-regeneration
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/dependencies/kosmickrisp-spirv/generate.py
            verify --source ${UML_KOSMICKRISP_SPIRV_MESA_SOURCE}
            --host ${UML_KOSMICKRISP_SPIRV_BUILD_HOST}
        LABELS dependency gpu
        TIMEOUT 3600)
endif()
