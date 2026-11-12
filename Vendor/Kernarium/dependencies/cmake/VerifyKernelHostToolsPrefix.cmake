# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED PREFIX)
    message(FATAL_ERROR "PREFIX is required for kernel host tool verification")
endif()
if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR
        "SOURCE_ROOT is required for kernel host tool verification")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/VerifyManifestProduct.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/VerifyPrefix.cmake")

set(_required
    "${PREFIX}/bin/pahole"
    "${PREFIX}/include/elf.h"
    "${PREFIX}/include/gelf.h"
    "${PREFIX}/include/libelf.h"
    "${PREFIX}/include/nlist.h"
    "${PREFIX}/lib/libelf.a"
    "${PREFIX}/lib/pkgconfig/libelf.pc")
uml_require_prefix_files(LABEL "Kernel host tools" FILES ${_required})

set(_manifest "${SOURCE_ROOT}/dependencies/manifest.tsv")
uml_verify_dependency_manifest_product(
    SOURCE "${_manifest}"
    INSTALLED "${PREFIX}/share/uml-dependencies/manifest.tsv"
    PRODUCT kernel-host-tools
    LABEL "Kernel host tools")
uml_verify_source_trees(
    INSTALLED "${PREFIX}/share/uml-dependencies/source-trees.tsv"
    LABEL "Kernel host tools"
    SOURCE_ROOT "${SOURCE_ROOT}"
    PRODUCT kernel-host-tools)

uml_verify_provenance_copy(
    LABEL "Kernel host tools"
    SOURCE "${SOURCE_ROOT}/dependencies/kernel-host-tools/archives.tsv"
    INSTALLED "${PREFIX}/share/uml-dependencies/kernel-host-tools-archives.tsv")
uml_verify_apple_binary(PATH "${PREFIX}/bin/pahole")

# The prefix must report the pinned dwarves release as its pahole version,
# since Kbuild derives its BTF options from that version.
file(STRINGS "${_manifest}" _dwarves_rows REGEX "^dwarves\t")
list(LENGTH _dwarves_rows _dwarves_count)
if(NOT _dwarves_count EQUAL 1 OR
        NOT _dwarves_rows MATCHES "^dwarves\t([0-9]+\\.[0-9]+)\t")
    message(FATAL_ERROR "The manifest pins no single dwarves release")
endif()
set(_expected_version "v${CMAKE_MATCH_1}")
execute_process(
    COMMAND "${PREFIX}/bin/pahole" --version
    RESULT_VARIABLE _pahole_status
    OUTPUT_VARIABLE _pahole_version
    ERROR_VARIABLE _pahole_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _pahole_status EQUAL 0 OR
        NOT _pahole_version STREQUAL _expected_version)
    message(FATAL_ERROR
        "pahole reports '${_pahole_version}${_pahole_error}', "
        "expected ${_expected_version}")
endif()
