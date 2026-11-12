# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.25)

project(uml_emulator_external_consumer LANGUAGES C CXX OBJC)

set(UML_EMULATOR_PACKAGE_PREFIX "" CACHE PATH
    "Installed UMLEmulator package prefix")
set(UML_EMULATOR_LIBRARY "" CACHE FILEPATH
    "Raw UMLEmulator archive used only for final-slice verification")
set(UML_EMULATOR_HEADERS "" CACHE PATH
    "Raw UMLEmulator include root used only for final-slice verification")
set(UML_EMULATOR_XCFRAMEWORK "" CACHE PATH
    "Final UMLEmulator XCFramework used for Xcode slice-selection verification")
set(UML_EXPECTED_GUEST_SLICE "arm64" CACHE STRING
    "Expected installed package userspace ISA slice")

if(UML_EMULATOR_PACKAGE_PREFIX)
    if(UML_EMULATOR_LIBRARY OR UML_EMULATOR_HEADERS OR
       UML_EMULATOR_XCFRAMEWORK)
        message(FATAL_ERROR
            "Select exactly one installed package, raw final slice, or XCFramework")
    endif()
    if(NOT IS_ABSOLUTE "${UML_EMULATOR_PACKAGE_PREFIX}")
        message(FATAL_ERROR
            "UML_EMULATOR_PACKAGE_PREFIX must be absolute")
    endif()
    find_package(UMLEmulator CONFIG REQUIRED
        PATHS "${UML_EMULATOR_PACKAGE_PREFIX}/lib/cmake/UMLEmulator"
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH)
    if(NOT "${UMLEmulator_APPLE_PLATFORM}" STREQUAL
       "${UML_APPLE_PLATFORM}")
        message(FATAL_ERROR
            "Installed package platform ${UMLEmulator_APPLE_PLATFORM} does not match ${UML_APPLE_PLATFORM}")
    endif()
    if(NOT "${UMLEmulator_GUEST_SLICE}" STREQUAL
       "${UML_EXPECTED_GUEST_SLICE}")
        message(FATAL_ERROR
            "Installed package slice ${UMLEmulator_GUEST_SLICE} does not match ${UML_EXPECTED_GUEST_SLICE}")
    endif()
    set(_uml_emulator_consumer_target UML::Emulator)
elseif(UML_EMULATOR_XCFRAMEWORK)
    if(UML_EMULATOR_LIBRARY OR UML_EMULATOR_HEADERS)
        message(FATAL_ERROR
            "Select exactly one installed package, raw final slice, or XCFramework")
    endif()
    if(NOT IS_ABSOLUTE "${UML_EMULATOR_XCFRAMEWORK}" OR
       NOT IS_DIRECTORY "${UML_EMULATOR_XCFRAMEWORK}")
        message(FATAL_ERROR
            "UML_EMULATOR_XCFRAMEWORK must name an existing absolute XCFramework")
    endif()
    add_library(uml_emulator_external INTERFACE)
    target_link_libraries(uml_emulator_external INTERFACE
        "${UML_EMULATOR_XCFRAMEWORK}"
        "$<$<LINK_LANGUAGE:C,OBJC>:c++>"
        "-framework Foundation"
        compression)
    set(_uml_emulator_consumer_target uml_emulator_external)
else()
    if(NOT IS_ABSOLUTE "${UML_EMULATOR_LIBRARY}" OR
       NOT EXISTS "${UML_EMULATOR_LIBRARY}")
        message(FATAL_ERROR
            "UML_EMULATOR_LIBRARY must name an existing absolute archive")
    endif()
    if(NOT IS_ABSOLUTE "${UML_EMULATOR_HEADERS}" OR
       NOT EXISTS "${UML_EMULATOR_HEADERS}/uml/emulator.h" OR
       NOT EXISTS "${UML_EMULATOR_HEADERS}/uml/abi_identity.h")
        message(FATAL_ERROR
            "UML_EMULATOR_HEADERS must contain both public headers")
    endif()
    add_library(uml_emulator_external STATIC IMPORTED GLOBAL)
    set_target_properties(uml_emulator_external PROPERTIES
        IMPORTED_LOCATION "${UML_EMULATOR_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${UML_EMULATOR_HEADERS}"
        INTERFACE_LINK_LIBRARIES
            "$<$<LINK_LANGUAGE:C,OBJC>:c++>;-framework Foundation;compression")
    set(_uml_emulator_consumer_target uml_emulator_external)
endif()

add_executable(uml-emulator-external-consumer main.cpp)
target_compile_features(uml-emulator-external-consumer PRIVATE cxx_std_20)
target_compile_options(uml-emulator-external-consumer PRIVATE
    -Wall -Wextra -Werror)
target_compile_definitions(uml-emulator-external-consumer PRIVATE
    UML_EXPECTED_PLATFORM="${UML_APPLE_PLATFORM}")
target_link_libraries(uml-emulator-external-consumer PRIVATE
    ${_uml_emulator_consumer_target})

add_executable(uml-emulator-external-c-consumer main.c)
target_compile_features(uml-emulator-external-c-consumer PRIVATE c_std_11)
target_compile_options(uml-emulator-external-c-consumer PRIVATE
    -Wall -Wextra -Werror)
target_compile_definitions(uml-emulator-external-c-consumer PRIVATE
    UML_EXPECTED_PLATFORM="${UML_APPLE_PLATFORM}")
target_link_libraries(uml-emulator-external-c-consumer PRIVATE
    ${_uml_emulator_consumer_target})

# The Objective-C consumer compiles the same source as the C one: a copy with
# a .m name is compiled as Objective-C.
configure_file(main.c ${CMAKE_CURRENT_BINARY_DIR}/objc_consumer.m COPYONLY)
add_executable(uml-emulator-external-objc-consumer
    ${CMAKE_CURRENT_BINARY_DIR}/objc_consumer.m)
set_target_properties(uml-emulator-external-objc-consumer PROPERTIES
    OBJC_STANDARD 11
    OBJC_STANDARD_REQUIRED YES
    OBJC_EXTENSIONS NO)
target_compile_options(uml-emulator-external-objc-consumer PRIVATE
    -Wall -Wextra -Werror)
target_compile_definitions(uml-emulator-external-objc-consumer PRIVATE
    UML_EXPECTED_PLATFORM="${UML_APPLE_PLATFORM}")
target_link_libraries(uml-emulator-external-objc-consumer PRIVATE
    ${_uml_emulator_consumer_target})
