# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.27)
project(UmlVaV4l2 LANGUAGES CXX)
# A guest-side libva backend over the Virtio video V4L2 stateful decoder. It is
# built natively for the guest it runs in (amd64 today), against that guest's
# own libva and libdrm headers, and never as part of the Apple host build.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The VA-API backend is a Linux guest component")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBVA REQUIRED IMPORTED_TARGET libva)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)

add_library(uml_va_v4l2_core OBJECT
    convert.cpp
    driver.cpp
    driver_encode.cpp
    encode.cpp
    encode_map.cpp
    storage.cpp
    v4l2_decoder.cpp
    v4l2_encoder.cpp
    v4l2_io.cpp
    v4l2_pool.cpp
    vp9.cpp)
target_compile_features(uml_va_v4l2_core PUBLIC cxx_std_20)
target_compile_options(uml_va_v4l2_core PUBLIC -Wall -Wextra -Werror)
target_link_libraries(uml_va_v4l2_core PUBLIC PkgConfig::LIBVA PkgConfig::LIBDRM ${CMAKE_DL_LIBS})
target_include_directories(uml_va_v4l2_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_property(TARGET uml_va_v4l2_core PROPERTY POSITION_INDEPENDENT_CODE ON)

# libva loads <LIBVA_DRIVER_NAME>_drv_video.so out of its driver directory.
add_library(uml_drv_video MODULE entry.cpp)
target_link_libraries(uml_drv_video PRIVATE uml_va_v4l2_core)
set_target_properties(uml_drv_video PROPERTIES PREFIX "" OUTPUT_NAME "uml_drv_video")
# Nothing outside the one libva entry point is part of the contract.
target_link_options(uml_drv_video PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/driver.map" "-Wl,--no-undefined")
set_property(TARGET uml_drv_video APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/driver.map")

# The gbm backend module for the virtio-gpu DRM driver: libgbm loads
# virtio_gpu_gbm.so by that driver name and the module adds the planar buffers
# (NV12, P010 and the three-plane layouts) that Debian's libgbm cannot allocate,
# on the device Mesa's own DRI backend makes. The gbm module section of
# README.md owns the contract. It is loaded into every process that uses gbm,
# so it needs no C++ runtime and exports one symbol.
add_library(uml_gbm_planar OBJECT gbm/planar.cpp)
target_compile_features(uml_gbm_planar PUBLIC cxx_std_20)
target_compile_options(uml_gbm_planar PUBLIC -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
target_include_directories(uml_gbm_planar PUBLIC ${LIBDRM_INCLUDE_DIRS})
set_property(TARGET uml_gbm_planar PROPERTY POSITION_INDEPENDENT_CODE ON)

add_library(virtio_gpu_gbm MODULE gbm/module.cpp)
target_link_libraries(virtio_gpu_gbm PRIVATE uml_gbm_planar ${CMAKE_DL_LIBS})
set_target_properties(virtio_gpu_gbm PROPERTIES PREFIX "" OUTPUT_NAME "virtio_gpu_gbm")
target_compile_options(virtio_gpu_gbm PRIVATE -fvisibility=hidden -fno-exceptions -fno-rtti)
target_link_options(virtio_gpu_gbm PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/gbm/module.map" "-Wl,--no-undefined"
    -nostdlib++ -static-libgcc)
set_property(TARGET virtio_gpu_gbm APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gbm/module.map")

add_executable(uml_va_v4l2_tests
    tests/va_v4l2_tests.cpp
    tests/pool_tests.cpp
    tests/fake_v4l2.cpp
    tests/encoder_tests.cpp
    tests/encode_driver_tests.cpp
    tests/annexb_tests.cpp
    tests/gbm_planar_tests.cpp
    tests/fake_gbm.cpp)
target_link_libraries(uml_va_v4l2_tests PRIVATE uml_va_v4l2_core uml_gbm_planar)
# The module test loads the built module and a fake Mesa backend beside the
# test binary.
add_library(fake_dri_gbm MODULE tests/fake_gbm.cpp)
target_compile_definitions(fake_dri_gbm PRIVATE UML_FAKE_GBM_MODULE)
target_compile_features(fake_dri_gbm PRIVATE cxx_std_20)
target_compile_options(fake_dri_gbm PRIVATE -Wall -Wextra -Werror)
target_include_directories(fake_dri_gbm PRIVATE ${LIBDRM_INCLUDE_DIRS})
set_target_properties(fake_dri_gbm PROPERTIES PREFIX "" OUTPUT_NAME "fake_dri_gbm")
add_dependencies(uml_va_v4l2_tests virtio_gpu_gbm fake_dri_gbm)

# The same driver over the in-memory encoder node, for running the probes through
# libva's loader on a host with no Virtio video device.
add_library(fake_drv_video MODULE tests/fake_entry.cpp tests/fake_v4l2.cpp)
target_link_libraries(fake_drv_video PRIVATE uml_va_v4l2_core)
set_target_properties(fake_drv_video PROPERTIES PREFIX "" OUTPUT_NAME "fake_drv_video")
target_link_options(fake_drv_video PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/driver.map" "-Wl,--no-undefined")

# Preloaded into a probe so libva-drm accepts a descriptor that is no DRM node.
add_library(drm_node_shim MODULE tests/drm_shim.cpp)
target_link_libraries(drm_node_shim PRIVATE PkgConfig::LIBDRM)
target_compile_options(drm_node_shim PRIVATE -Wall -Wextra -Werror)
set_target_properties(drm_node_shim PROPERTIES PREFIX "" OUTPUT_NAME "drm_node_shim")

# The guest-side qualification client. It talks to libva, not to this driver
# directly, so it also runs against any other VA-API backend.
add_executable(va_decode_probe tests/va_decode_probe.cpp)
target_link_libraries(va_decode_probe PRIVATE uml_va_v4l2_core)
pkg_check_modules(LIBVA_DRM REQUIRED IMPORTED_TARGET libva-drm)
target_link_libraries(va_decode_probe PRIVATE PkgConfig::LIBVA_DRM)

# The guest-side encode client: Chromium's submission, an Annex B stream out.
add_executable(va_encode_probe tests/va_encode_probe.cpp)
target_link_libraries(va_encode_probe PRIVATE PkgConfig::LIBVA PkgConfig::LIBVA_DRM)
target_compile_options(va_encode_probe PRIVATE -Wall -Wextra -Werror)
target_compile_features(va_encode_probe PRIVATE cxx_std_20)

# The guest-side client of the real libgbm and libEGL for the gbm module.
add_executable(gbm_planar_probe tests/gbm_planar_probe.cpp)
target_link_libraries(gbm_planar_probe PRIVATE ${CMAKE_DL_LIBS})
target_compile_options(gbm_planar_probe PRIVATE -Wall -Wextra -Werror)
target_compile_features(gbm_planar_probe PRIVATE cxx_std_20)

include(GNUInstallDirs)
install(TARGETS uml_drv_video LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/dri)
install(TARGETS virtio_gpu_gbm LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gbm)
# Chromium reads this to use the driver for hardware video encode and decode.
install(FILES chromium.d/kernarium-video DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/chromium.d)
