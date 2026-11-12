# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.27)

if(NOT DEFINED VERIFY_SCRIPT)
    message(FATAL_ERROR "VERIFY_SCRIPT is required")
endif()
include("${VERIFY_SCRIPT}")

if(DEFINED MODE AND MODE STREQUAL "reject")
    uml_verify_dependency_manifest_product(
        SOURCE "${SOURCE_MANIFEST}"
        INSTALLED "${INSTALLED_MANIFEST}"
        PRODUCT apple-gpu
        LABEL fixture)
    message(FATAL_ERROR "Changed product rows were incorrectly accepted")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _suffix)
set(_fixture_root
    "${CMAKE_CURRENT_BINARY_DIR}/manifest-product-fixture-${_suffix}")
file(MAKE_DIRECTORY "${_fixture_root}")

set(_header
    "name\tversion\tproducts\trepository\tcommit\tsource_paths\tpatch_series\n")
set(_gpu_row
    "gpu\t1\tapple-gpu\thttps://example.invalid/gpu.git\t1111111\t.\t-\n")
set(_zstd_row
    "zstd\t1\tios-compression\thttps://example.invalid/zstd.git\t2222222\t.\t-\n")
set(_source_manifest "${_fixture_root}/source.tsv")
set(_unrelated_manifest "${_fixture_root}/unrelated.tsv")
set(_changed_manifest "${_fixture_root}/changed.tsv")
file(WRITE "${_source_manifest}" "${_header}${_gpu_row}${_zstd_row}")
file(WRITE "${_unrelated_manifest}"
    "${_header}${_gpu_row}zstd\t2\tios-compression\thttps://example.invalid/zstd.git\t3333333\t.\t-\n")
file(WRITE "${_changed_manifest}"
    "${_header}gpu\t2\tapple-gpu\thttps://example.invalid/gpu.git\t4444444\t.\t-\n${_zstd_row}")

uml_verify_dependency_manifest_product(
    SOURCE "${_source_manifest}"
    INSTALLED "${_unrelated_manifest}"
    PRODUCT apple-gpu
    LABEL fixture)

# The products column records which prefixes share a row; changing it
# identifies no new build.
set(_shared_manifest "${_fixture_root}/shared.tsv")
file(WRITE "${_shared_manifest}"
    "${_header}gpu\t1\tapple-gpu,other-gpu\thttps://example.invalid/gpu.git\t1111111\t.\t-\n${_zstd_row}")
uml_verify_dependency_manifest_product(
    SOURCE "${_source_manifest}"
    INSTALLED "${_shared_manifest}"
    PRODUCT apple-gpu
    LABEL fixture)

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DVERIFY_SCRIPT=${VERIFY_SCRIPT}
        -DMODE=reject
        -DSOURCE_MANIFEST=${_source_manifest}
        -DINSTALLED_MANIFEST=${_changed_manifest}
        -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE _changed_status
    OUTPUT_VARIABLE _changed_output
    ERROR_VARIABLE _changed_error)
if(_changed_status EQUAL 0)
    message(FATAL_ERROR "Changed product rows were incorrectly accepted")
endif()
set(_changed_diagnostic "${_changed_output}${_changed_error}")
if(NOT _changed_diagnostic MATCHES
   "fixture dependency manifest rows do not match")
    message(FATAL_ERROR
        "Changed product rows had the wrong diagnostic: "
        "${_changed_diagnostic}")
endif()

# A row owns the last layer of its patch series, and a rootfs-vulkan layer is
# below tools/rootfs/vulkan. A product matches whole products-column entries.
set(_series_manifest "${_fixture_root}/series.tsv")
file(WRITE "${_series_manifest}" "${_header}${_gpu_row}"
    "stack\t1\tapple-gpu\thttps://example.invalid/stack.git\t5555555\t.\t"
    "rootfs-vulkan:patches/stack/series.tsv+patches/stack/series.tsv\n"
    "${_zstd_row}"
    "net\t1\tios-network,apple-gpu-extra\thttps://example.invalid/net.git\t"
    "6666666\t.\tpatches/net/series.tsv\n")
function(_expect_series product owned_expected layers_expected)
    set(_product)
    if(product)
        set(_product PRODUCT "${product}")
    endif()
    uml_manifest_patch_series(
        MANIFEST "${_series_manifest}"
        SOURCE_ROOT /source
        ${_product}
        OWNED _owned
        LAYERS _layers)
    if(NOT "${_owned}" STREQUAL "${owned_expected}" OR
       NOT "${_layers}" STREQUAL "${layers_expected}")
        message(FATAL_ERROR
            "Patch series of '${product}' are '${_owned}' and '${_layers}', "
            "expected '${owned_expected}' and '${layers_expected}'")
    endif()
endfunction()
_expect_series(apple-gpu
    "stack;/source/dependencies/patches/stack/series.tsv"
    "/source/tools/rootfs/vulkan/patches/stack/series.tsv;/source/dependencies/patches/stack/series.tsv")
_expect_series(ios-network
    "net;/source/dependencies/patches/net/series.tsv"
    "/source/dependencies/patches/net/series.tsv")
_expect_series(ios-compression "" "")
_expect_series(""
    "stack;/source/dependencies/patches/stack/series.tsv;net;/source/dependencies/patches/net/series.tsv"
    "/source/tools/rootfs/vulkan/patches/stack/series.tsv;/source/dependencies/patches/stack/series.tsv;/source/dependencies/patches/net/series.tsv")

file(REMOVE_RECURSE "${_fixture_root}")
message(STATUS "Dependency product manifest verifier tests passed")
