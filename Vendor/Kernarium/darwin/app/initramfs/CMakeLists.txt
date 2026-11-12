# SPDX-License-Identifier: GPL-2.0-or-later

# Writes a per-run guest initramfs on the host without an emulator.
add_executable(uml-initramfs main.cpp)
uml_declare_architecture_target(uml-initramfs app)
target_link_libraries(uml-initramfs PRIVATE UML::BootInitramfs)
set_target_properties(uml-initramfs PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
