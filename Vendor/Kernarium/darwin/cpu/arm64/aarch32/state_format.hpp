// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/guest_key.hpp"

#include <cstdint>

namespace uml::cpu::arm64::aarch32 {

/* The formats an AArch32 task's program status and FPSCR travel in between
 * the guest kernel and the engines (kernel/common/abi/darwin_uml_abi.h), and
 * the engines' own forms of the parts that need one. Both are architectural
 * layouts, so this header includes nothing of the bridge.
 *
 * The status is arm64 Linux's PSR_AA32_* layout, which is the SPSR of an
 * exception taken from AArch32: N, Z, C and V in 31:28, Q in 27, IT[1:0] in
 * 26:25, DIT in 24, SS in 21, GE in 19:16, IT[7:2] in 15:10, E in 9, A, I
 * and F in 8:6, T in 5, nRW in 4 and M[3:0] in 3:0. */

inline constexpr std::uint32_t psr_nzcv = 0xf000'0000U;
inline constexpr std::uint32_t psr_q = 1U << 27;
inline constexpr std::uint32_t psr_ge = 0x000f'0000U;
inline constexpr std::uint32_t psr_it_low = 0x0600'0000U;
inline constexpr std::uint32_t psr_it_high = 0x0000'fc00U;
inline constexpr std::uint32_t psr_it = psr_it_low | psr_it_high;
inline constexpr std::uint32_t psr_e = 1U << 9;
inline constexpr std::uint32_t psr_t = 1U << 5;
/* M[4:0] of a task in User mode: nRW is set and M[3:0] is 0. */
inline constexpr std::uint32_t psr_mode_mask = 0x1fU;
inline constexpr std::uint32_t psr_mode_user = 0x10U;

/* The fields an engine owns, and so the runtime repacks on every exit from
 * its own state; everything else (A, I, F, SS, DIT, IL, PAN, SSBS, the mode
 * and E) is the kernel's and passes through. T is the key's. */
inline constexpr std::uint32_t psr_engine_owned =
	psr_nzcv | psr_q | psr_ge | psr_it | psr_t;

/* ITSTATE[7:0] of a status: IT[7:2] and IT[1:0]. */
[[nodiscard]] constexpr std::uint8_t psr_itstate(std::uint32_t psr) noexcept
{
	return static_cast<std::uint8_t>(
		((psr >> 10) & 0x3fU) << 2 | ((psr >> 25) & 0x3U));
}

/* The status bits ITSTATE[7:0] occupies. */
[[nodiscard]] constexpr std::uint32_t psr_it_bits(std::uint8_t it) noexcept
{
	return (static_cast<std::uint32_t>(it & 0x3U) << 25) |
		(static_cast<std::uint32_t>(it >> 2) << 10);
}

/* GE[3:0] with each flag widened to the byte lane it governs, the form
 * CpuState::a32_ge_mask holds. */
[[nodiscard]] constexpr std::uint32_t psr_ge_mask(std::uint32_t psr) noexcept
{
	std::uint32_t mask = 0;
	for (unsigned lane = 0; lane < 4; ++lane)
		if ((psr >> (16 + lane) & 1U) != 0U)
			mask |= 0xffU << (lane * 8U);
	return mask;
}

/* The status bits of a lane mask: a flag is set when its byte is not zero. */
[[nodiscard]] constexpr std::uint32_t psr_ge_bits(std::uint32_t mask) noexcept
{
	std::uint32_t bits = 0;
	for (unsigned lane = 0; lane < 4; ++lane)
		if ((mask >> (lane * 8U) & 0xffU) != 0U)
			bits |= 1U << (16 + lane);
	return bits;
}

/* FPSCR is split across the bridge's fpsr and fpcr as arm64 Linux splits it
 * (asm/fpsimd.h VFP_FPSCR_STAT_MASK and VFP_FPSCR_CTRL_MASK): fpsr holds the
 * status half, FPSCR & 0xf800009f, and fpcr the control half, FPSCR &
 * 0x07f79f00. The virtual CPU's FPSCR has no short vectors and no
 * floating-point trap enables, so the control bits it holds are AHP, DN, FZ,
 * RMode and FZ16: Len and Stride read as zero and the trap enables read as
 * zero and ignore writes.
 *
 * FZ16 (bit 19) is outside the kernel's mask, as it is outside arm64
 * Linux's. The host holds it like the other control bits, so it survives
 * system calls and context switches, as it does in the FPCR of a native
 * arm64 task; a signal frame and ptrace, which apply the mask, do not carry
 * it. */
inline constexpr std::uint32_t fpscr_status_mask = 0xf800'009fU;
inline constexpr std::uint32_t fpscr_bridge_control_mask = 0x07f7'9f00U;
inline constexpr std::uint32_t fpscr_fz16 = 1U << 19;
inline constexpr std::uint32_t fpscr_control_mask = 0x07c8'0000U;
/* FPSCR.N, Z, C and V, which live in CpuState::a32_fpscr_nzcv because the
 * hosts implement FPSR[31:28] as RAZ/WI. */
inline constexpr std::uint32_t fpscr_nzcv = 0xf000'0000U;

static_assert(psr_it_bits(psr_itstate(psr_it)) == psr_it);
static_assert(psr_itstate(psr_it_bits(0xa5)) == 0xa5);
static_assert(psr_ge_mask(psr_ge) == 0xffff'ffffU);
static_assert(psr_ge_bits(0x00ff'00ffU) == 0x0005'0000U);
static_assert((fpscr_control_mask & ~(fpscr_bridge_control_mask | fpscr_fz16)) ==
	0U);
static_assert((fpscr_status_mask & fpscr_bridge_control_mask) == 0U);
static_assert((psr_engine_owned & (psr_e | psr_mode_mask)) == 0U);

} // namespace uml::cpu::arm64::aarch32
