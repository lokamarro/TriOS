// SPDX-License-Identifier: GPL-2.0-or-later
#include "fp_environment.hpp"

namespace uml::cpu::arm64 {

extern "C" void uml_arm64_restore_native_fp_environment(
	CpuState *cpu);

void restore_native_fp_environment(CpuState &cpu) noexcept
{
	uml_arm64_restore_native_fp_environment(&cpu);
}

} // namespace uml::cpu::arm64
