// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu_state.hpp"

namespace uml::cpu::arm64 {

void restore_native_fp_environment(CpuState &cpu) noexcept;

} // namespace uml::cpu::arm64

