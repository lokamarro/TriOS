# SPDX-License-Identifier: GPL-2.0-or-later

# The only owner of executable memory: JIT mappings, the per-thread write
# window and instruction-cache publication. Its engine kind lets the JIT
# engines link it and keeps the gadget engines and shared CPU code, which stay
# data-only, from doing so.
add_library(uml_cpu_executable_memory OBJECT executable_memory.cpp)
uml_declare_architecture_target(uml_cpu_executable_memory cpu native)
