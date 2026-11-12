# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.27)
project(uml_softfloat_dependency LANGUAGES C CXX)

if(NOT IS_DIRECTORY "${UML_FEX_SOURCE_DIR}")
    message(FATAL_ERROR "UML_FEX_SOURCE_DIR is not a directory")
endif()

set(ARCHITECTURE_arm64 ON)
set(HAS_CLANG_PRESERVE_ALL OFF)
add_subdirectory("${UML_FEX_SOURCE_DIR}/External/SoftFloat-3e"
    softfloat-source)
add_subdirectory("${UML_FEX_SOURCE_DIR}/External/cephes"
    cephes-source)

install(TARGETS softfloat_3e cephes_128bit ARCHIVE DESTINATION lib)
install(DIRECTORY
    "${UML_FEX_SOURCE_DIR}/External/SoftFloat-3e/include/SoftFloat-3e"
    DESTINATION include
    FILES_MATCHING PATTERN "*.h")
install(FILES
    "${UML_FEX_SOURCE_DIR}/External/cephes/include/cephes_128bit.h"
    DESTINATION include)
