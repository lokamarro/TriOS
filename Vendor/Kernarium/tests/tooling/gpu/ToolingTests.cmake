# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME vulkan-probe-guest-safety
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_run_vulkan_probe.py
    LABELS gpu;vulkan;rootfs
    TIMEOUT 120)

uml_add_tooling_test(
    NAME gpu-guest-rootfs-safety
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_run_gpu_guest.py
    LABELS gpu
    TIMEOUT 120)

uml_add_tooling_test(
    NAME gpu-guest-process-ownership-safety
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_guest_process.py
    LABELS gpu;cts
    TIMEOUT 120)

uml_add_tooling_test(
    NAME vk-gl-cts-campaign-safety
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_run_vk_gl_cts.py
    LABELS gpu;cts;rootfs
    TIMEOUT 120)

uml_add_tooling_test(
    NAME opengl-cts-plan-safety
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_opengl_cts_plan.py
    LABELS gpu;cts;rootfs
    TIMEOUT 120)

uml_add_tooling_test(
    NAME gles-cts-plan-safety
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_gles_cts_plan.py
    LABELS gpu;cts;rootfs
    TIMEOUT 120)

uml_add_tooling_test(
    NAME graphics-cts-pool-safety
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_graphics_cts_campaign.py
    LABELS gpu;cts;vulkan;rootfs
    TIMEOUT 120)

uml_add_tooling_test(
    NAME vulkan-cts-campaign-safety
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_run_vk_cts.py
    LABELS gpu;cts;vulkan;rootfs
    TIMEOUT 240)

uml_add_tooling_test(
    NAME cts-shard-resumption-safety
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_cts_shard_resilience.py
    LABELS gpu;cts;vulkan
    TIMEOUT 180)

uml_add_tooling_test(
    NAME vulkan-guest-rootfs-safety
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_vulkan_guest_rootfs.py
    LABELS gpu;cts;vulkan;rootfs
    TIMEOUT 120)

uml_add_tooling_test(
    NAME kde-vnc-launcher-options
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_boot_kde_vnc.py
    LABELS gpu;vnc
    TIMEOUT 120)

uml_add_tooling_test(
    NAME chromium-webgl-aquarium-metrics
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/test_run_chromium_webgl.py
    LABELS gpu;webgl
    TIMEOUT 240)

