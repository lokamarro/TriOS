# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME source-tree-cxx-only
    COMMAND ${CMAKE_COMMAND}
        -DAUDIT_SCRIPT=${PROJECT_SOURCE_DIR}/scripts/audit-source-tree.cmake
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/source-tree-audit-fixture
        -P ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_source_tree_audit.cmake
    LABELS audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME xcframework-output-safety
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_xcframework_destination.sh
        ${PROJECT_SOURCE_DIR}/scripts/build-xcframework.sh
    LABELS audit;package
    TIMEOUT 60)

uml_add_tooling_test(
    NAME git-identity-environment-contract
    COMMAND
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_git_identity_contract.sh
    LABELS audit;kernel;dependency;android
    TIMEOUT 120)

uml_add_tooling_test(
    NAME patch-series-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_patch_series.py
    LABELS audit;kernel;dependency;android
    TIMEOUT 450)

uml_add_tooling_test(
    NAME apple-xcframework-staging
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_create_apple_xcframework.sh
        ${PROJECT_SOURCE_DIR}/scripts/create-apple-xcframework.py
    LABELS audit;package
    TIMEOUT 60)

# The XCFramework, Kernarium framework and Kernarium app packaging run this
# check on every iOS product; the fixtures show it finds FEXCore as a symbol,
# an archive member, a nested framework and a stripped allocation name.
uml_add_tooling_test(
    NAME ios-products-carry-no-fexcore
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_check_no_fexcore.py
        --checker ${PROJECT_SOURCE_DIR}/scripts/check-no-fexcore.py
    LABELS audit;package
    TIMEOUT 120)

# The software renderer is the Simulator's alone. The Kernarium framework
# packaging runs this check on every device slice, and requires it in the
# Simulator's GL provider; the fixtures show it finds either Gallium screen as a
# symbol, an archive member or a nested slice, the provider's marker once the
# symbols are gone, and that a reference alone proves nothing.
uml_add_tooling_test(
    NAME software-renderer-is-simulator-only
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_check_software_renderer.py
        --checker ${PROJECT_SOURCE_DIR}/scripts/check-software-renderer.py
    LABELS audit;package
    TIMEOUT 120)

# A bundle's privacy manifest declares every required-reason API the bundle
# uses. The framework packaging and the Andarium App Store build run this
# check; the fixtures show it finds a C symbol, an Objective-C class, a
# selector with no symbol, a missing or malformed manifest and an
# Info.plist with an empty version or no MinimumOSVersion.
uml_add_tooling_test(
    NAME ios-privacy-manifests
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_check_privacy_manifests.py
        --checker ${PROJECT_SOURCE_DIR}/scripts/check-privacy-manifests.py
    LABELS audit;package
    TIMEOUT 180)

# A Mac run of an iPad application is refused by Gatekeeper when the build's
# products are in a system temporary directory, so the first build phase of each
# application's target refuses that directory for a Mac destination and no other.
uml_add_tooling_test(
    NAME apple-run-location
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_check_run_location.py
        --checker ${PROJECT_SOURCE_DIR}/scripts/check-run-location.py
    LABELS audit;package
    TIMEOUT 60)

# scripts/build-xcframework.sh takes its exact public API from the headers
# through this check, so a public change needs no second list.
uml_add_tooling_test(
    NAME xcframework-public-api-check
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_check_public_api.py
        --checker ${PROJECT_SOURCE_DIR}/scripts/check-public-api.py
        --headers ${PROJECT_SOURCE_DIR}/darwin/api/include/uml
    LABELS audit;package
    TIMEOUT 60)

uml_add_tooling_test(
    NAME json-input-duplicate-key-policy
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_json_input_policy.py
        --validator
            ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/validate_json_inputs.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME repository-environment-portability
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_environment_portability.py
        --checker
            ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/check_environment_portability.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS audit
    TIMEOUT 180)

# The licensing rule is by location (LICENSES/README.md): every source file
# carries an SPDX header with an identifier allowed where it lives, none uses a
# deprecated identifier, and LICENSE and LICENSES/ hold the text of every
# identifier in use. The fixtures mutate a header, a text and an exception.
uml_add_tooling_test(
    NAME repository-license-headers
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_license_headers.py
        --checker
            ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/check_license_headers.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS audit
    TIMEOUT 90)

# One shared adb server serves every first-party script, which names its guest
# with -s: a private server outlives a killed script and keeps retrying the
# host's USB devices. The fixtures show the checker finds each of the three
# ways to pick or stop a server, honours the allowlist rule by rule, and
# rejects an entry that matches nothing; the tree must then be clean.
uml_add_tooling_test(
    NAME repository-adb-shared-server
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_adb_server_policy.py
        --checker
            ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/check_adb_server_policy.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME engine-kind-link-audit
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_engine_kind_link_audit.py
        --cmake ${CMAKE_COMMAND}
        --audit ${PROJECT_SOURCE_DIR}/scripts/audit-cmake-file-api.cmake
    LABELS audit;cpu
    TIMEOUT 240)

# Every Apple product builds at the toolchain's one deployment target, so a
# tree that caches another target, or is handed one, must fail to configure.
uml_add_tooling_test(
    NAME apple-deployment-floor
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_apple_deployment_floor.py
        --cmake ${CMAKE_COMMAND}
        --toolchain ${PROJECT_SOURCE_DIR}/cmake/toolchains/Apple.cmake
    LABELS audit
    TIMEOUT 120)

# An Xcode update installs another SDK or rewrites the selected one. A tree
# configured before it must reconfigure at its next build and take its SDK
# libraries and frameworks from the SDK installed now.
uml_add_tooling_test(
    NAME apple-sdk-update
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/cmake/test_apple_sdk_update.py
        --cmake ${CMAKE_COMMAND}
        --make-program ${CMAKE_MAKE_PROGRAM}
        --toolchain ${PROJECT_SOURCE_DIR}/cmake/toolchains/Apple.cmake
        --repository ${PROJECT_SOURCE_DIR}
    LABELS audit
    TIMEOUT 120)
