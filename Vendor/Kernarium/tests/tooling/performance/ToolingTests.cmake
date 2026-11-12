# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME performance-runner-cli
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/performance/test_cli.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS performance
    TIMEOUT 30)

uml_add_tooling_test(
    NAME geekbench-guest-script
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/performance/test_geekbench_script.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS performance
    TIMEOUT 60)

uml_add_tooling_test(
    NAME geekbench-image-tool
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/performance/test_geekbench_images.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS performance
    TIMEOUT 120)

uml_add_tooling_test(
    NAME guest-sample-symbolization
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/performance/symbolize_guest_samples_tests.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS performance
    TIMEOUT 30)

uml_add_tooling_test(
    NAME gadget-cache-census-analysis
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/performance/gadget_cache_census_tests.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS performance
    TIMEOUT 30)

uml_add_tooling_test(
    NAME steam-boot-bench-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/performance/steam_boot_bench_tests.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS performance;steam
    TIMEOUT 300)

uml_add_tooling_test(
    NAME android-launch-suite-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/performance/android_launch_suite_tests.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS performance;android
    TIMEOUT 30)
