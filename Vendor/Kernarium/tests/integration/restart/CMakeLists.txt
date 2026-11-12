# SPDX-License-Identifier: GPL-2.0-or-later

# A guest that restarts itself boots again in the process that runs it
# (run_guest_restart.py). The runner inserts a process audit into the launcher
# to read what only the process can say about itself, and boots a guest whose
# /init restarts at once to see the loop guard end the launch.
add_library(uml_process_audit SHARED process_audit.cpp)
set_target_properties(uml_process_audit PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
# The window's address is the product's own definition.
target_link_libraries(uml_process_audit PRIVATE UML::GuestPageConfiguration)
target_compile_options(uml_process_audit PRIVATE -Wall -Wextra -Werror)
target_compile_features(uml_process_audit PRIVATE cxx_std_20)
add_dependencies(integration-test-binaries uml_process_audit)

uml_add_freestanding_cxx_payload(UML_GUEST_RESTART_IMMEDIATELY_INIT
    guest-restart-immediately-init
    restart_immediately.cpp
    COMMENT "Building the freestanding arm64 guest init that restarts at once"
    ARCH_OPTIONS -march=armv8-a -mgeneral-regs-only NO_PIC)

# Two in-place restarts and a power-off, on each engine, then a guest that
# restarts at once.
uml_add_guest_integration_test(
    NAME guest-restart
    SCRIPT run_guest_restart.py
    ARGS --audit-library $<TARGET_FILE:uml_process_audit>
        --restart-init ${UML_GUEST_RESTART_IMMEDIATELY_INIT}
    LABELS guest;boot;kernel
    TIMEOUT 900
    CPUS 2)
