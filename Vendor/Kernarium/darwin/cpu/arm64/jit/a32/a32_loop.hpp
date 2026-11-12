// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/engine.hpp"

namespace uml::cpu::arm64 {

class JitExecutor;

namespace jit::a32 {

/* The JIT's AArch32 entry (CpuEngine::run_aarch32): runs the task whose PC
 * key (aarch32/guest_key.hpp) is request.cpu.pc until an exit the runtime
 * handles. The PC stays a key in every exit, a fault is reported at its
 * architectural 32-bit address, and a BKPT carries its immediate in the
 * result's syndrome. */
[[nodiscard]] ExecutionResult run_a32_loop(JitExecutor &executor,
                                           ExecutionRequest &request);

} // namespace jit::a32
} // namespace uml::cpu::arm64
