# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME kernel-slice-producer-contract
    COMMAND
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_kernel_slice_contract.sh
    LABELS kernel;audit
    TIMEOUT 240)

uml_add_tooling_test(
    NAME kernel-config-fragments
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_config_fragments.sh
    LABELS kernel;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME kernel-series-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_kernel_series.py
    LABELS kernel;android;audit
    TIMEOUT 450)

uml_add_tooling_test(
    NAME kernel-artifact-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_kernel_artifact_contract.py
        --artifact-module
            ${PROJECT_SOURCE_DIR}/kernel/common/artifacts/kernel_artifact.py
        --schema
            ${PROJECT_SOURCE_DIR}/kernel/common/artifacts/kernel-artifact-manifest.schema.json
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-git-source-preparer
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_materialize_git.sh
    LABELS kernel;dependency
    # It runs a few dozen git processes and takes seconds on a quiet host, but
    # a gate at a load average of 80 starves each of them. The bound only
    # catches a wedged materializer.
    TIMEOUT 300)

uml_add_tooling_test(
    NAME kernel-source-location-config
    COMMAND
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_source_location_config.sh
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-video-protocol-sync
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_video_protocol_sync.py
        --tool ${PROJECT_SOURCE_DIR}/kernel/common/video/sync-protocol.py
        --header ${PROJECT_SOURCE_DIR}/kernel/common/video/virtio_video.h
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-gpu-protocol-sync
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_gpu_protocol_sync.py
        --tool ${PROJECT_SOURCE_DIR}/kernel/common/gpu/sync-protocol.py
        --header ${PROJECT_SOURCE_DIR}/kernel/common/gpu/virtio_gpu_uml.h
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-darwin-host-compat
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_darwin_host_compat.sh
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME apple-shared-kernel-image-policy
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_apple_kernel_image_policy.py
        --artifact-cmake
            ${PROJECT_SOURCE_DIR}/kernel/common/artifacts/KernelArtifact.cmake
        --carrier-verifier
            ${PROJECT_SOURCE_DIR}/Kernarium/scripts/verify-signed-kernel-carrier.py
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-darwin-irq-edge
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_darwin_irq_edge.py
    LABELS kernel
    TIMEOUT 120)

uml_add_tooling_test(
    NAME aarch32-elf32-wrapper
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_make_elf32.py
        --compiler ${CMAKE_CXX_COMPILER}
        --wrapper ${PROJECT_SOURCE_DIR}/tests/integration/aarch32/make_elf32.py
    LABELS kernel;aarch32
    TIMEOUT 60)

uml_add_tooling_test(
    NAME aarch32-elf32-linker
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_link_elf32.py
        --compiler ${CMAKE_CXX_COMPILER}
        --linker ${PROJECT_SOURCE_DIR}/tests/integration/aarch32/link_elf32.py
    LABELS kernel;aarch32
    TIMEOUT 60)
