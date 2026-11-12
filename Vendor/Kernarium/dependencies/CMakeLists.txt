# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT UML_TARGET_MACOS)
    message(FATAL_ERROR
        "Source-built desktop dependencies are currently supported only on macOS")
endif()

set(_uml_dependency_verifier
    "${CMAKE_CURRENT_SOURCE_DIR}/verify-prefix.sh")
set(_uml_dependency_preparer
    "${CMAKE_CURRENT_SOURCE_DIR}/prepare.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}/series-field.sh")
set(_uml_dependency_receipt
    "${UML_DEPENDENCY_PREFIX}/share/uml-dependencies/receipt.tsv")
set(_uml_dependency_manifest
    "${CMAKE_CURRENT_SOURCE_DIR}/manifest.tsv")
# A series records the tree it produces, so its series.tsv changes whenever
# the patched source does. prepare.sh validates the series owners.
include(cmake/VerifyManifestProduct.cmake)
uml_manifest_patch_series(
    MANIFEST "${_uml_dependency_manifest}"
    SOURCE_ROOT "${PROJECT_SOURCE_DIR}"
    LAYERS _uml_dependency_patch_inputs)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_uml_dependency_manifest}"
    "${_uml_dependency_preparer}"
    "${_uml_dependency_verifier}"
    "${_uml_dependency_receipt}"
    ${_uml_dependency_patch_inputs})

uml_verify_dependency_prefix(
    PREFIX "${UML_DEPENDENCY_PREFIX}"
    MANIFEST "${_uml_dependency_manifest}"
    DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")

add_custom_target(uml_dependency_prefix_verify
    COMMAND "${_uml_dependency_verifier}"
        --prefix "${UML_DEPENDENCY_PREFIX}"
        --manifest "${_uml_dependency_manifest}"
        --deployment-target "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    DEPENDS
        "${_uml_dependency_manifest}"
        "${_uml_dependency_preparer}"
        "${_uml_dependency_verifier}"
        "${_uml_dependency_receipt}"
        ${_uml_dependency_patch_inputs}
    COMMENT "Revalidating the private dependency prefix before linking"
    VERBATIM)

find_package(Threads REQUIRED)
include(cmake/UmlThirdPartyTargets.cmake)
uml_define_third_party_targets("${UML_DEPENDENCY_PREFIX}")
