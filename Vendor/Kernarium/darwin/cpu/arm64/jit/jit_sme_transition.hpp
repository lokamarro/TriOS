// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/sme_profile.hpp"

#include <cstdint>

/*
 * The host streaming-mode transitions of SM=1 JIT translations, in
 * jit_sme_transition.S. Generated code calls them; they have no C calling
 * convention: x15 is CpuState, enter takes the host mode in w9, and each
 * changes only x9-x13 and x30.
 */
extern "C" void uml_arm64_jit_sme_enter() noexcept;
extern "C" void uml_arm64_jit_sme_leave() noexcept;
/* The SM=1 counterpart of uml_arm64_jit_tlb_translate_preserving, with its
 * register contract. */
extern "C" void uml_arm64_jit_tlb_translate_streaming() noexcept;

namespace uml::cpu::arm64::jit {

/* The JIT's own CpuState::sme_host_mode bit beside the shared sme_host_*
 * bits: guest ZT0 is resident in host ZT0. jit_sme_transition.S tests it as
 * bit 4. */
inline constexpr std::uint8_t jit_sme_host_zt0_resident = 0x10U;

/*
 * The host mode a translation of guest SVCR mode `mode` runs in. SM=1
 * translations keep the whole guest vector state resident: Z and P, and
 * with ZA set, ZA and (for an SME2 profile) ZT0. Every other translation,
 * SM=0/ZA=1 included, runs with host SME off, which is 0.
 */
[[nodiscard]] constexpr std::uint8_t
jit_sme_host_mode(std::uint8_t mode, bool sme2) noexcept {
  if ((mode & svcr_sm) == 0U)
    return 0;
  auto host = static_cast<std::uint8_t>(sme_host_streaming |
                                        sme_host_streaming_resident);
  if ((mode & svcr_za) != 0U)
    host = static_cast<std::uint8_t>(
        host | sme_host_za | sme_host_za_resident |
        (sme2 ? jit_sme_host_zt0_resident : 0U));
  return host;
}

static_assert(jit_sme_host_mode(svcr_za, true) == 0);
static_assert(jit_sme_host_mode(svcr_sm, true) == 0x05);
static_assert(jit_sme_host_mode(svcr_mask, false) == 0x0f);
static_assert(jit_sme_host_mode(svcr_mask, true) == 0x1f);
static_assert((jit_sme_host_zt0_resident &
               (sme_host_streaming | sme_host_za |
                sme_host_streaming_resident | sme_host_za_resident)) == 0);

} // namespace uml::cpu::arm64::jit
