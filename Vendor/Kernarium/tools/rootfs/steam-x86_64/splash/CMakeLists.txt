# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.27)
project(KernariumSplash LANGUAGES CXX)
# Built only inside the x86_64 Steam appliance's producer-prepared verified
# toolchain, natively for its guest, with networking disabled.
if(NOT UML_STEAM_VERIFIED_SDK OR NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    message(FATAL_ERROR "Use a Steam rootfs producer and its verified toolchain")
endif()
add_executable(kernarium-screen screen.cpp)
target_compile_features(kernarium-screen PRIVATE cxx_std_20)
target_compile_options(kernarium-screen PRIVATE -Wall -Wextra -Werror)
target_include_directories(kernarium-screen PRIVATE /usr/include/cairo /usr/include/libdrm)
target_link_libraries(kernarium-screen PRIVATE cairo X11 drm gbm EGL GLESv2)
install(TARGETS kernarium-screen RUNTIME DESTINATION libexec)
