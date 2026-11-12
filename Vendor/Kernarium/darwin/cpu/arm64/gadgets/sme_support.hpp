// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/sme_profile.hpp"

namespace uml::cpu::arm64::gadgets {

/* How much of the SME profile the gadget engine implements: all of it but
 * the SME2 forms. SMSTART, SMSTOP and the SVCR and TPIDR2_EL0 accesses,
 * RDSVL, ADDSVL and ADDSPL, and the ZA and ZT0 loads, stores and zeroings
 * run in every mode the profile allows. In streaming mode so do the SVE,
 * SVE2 and SME forms of the encoding table outside SME2, the FP&SIMD
 * instructions legal there, and every other instruction the engine runs
 * outside it. An SME2 form is the illegal-instruction exit, as a word
 * outside the profile is. */
inline constexpr SmeSupport sme_support = SmeSupport::complete;

} // namespace uml::cpu::arm64::gadgets
