// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/host_capabilities.hpp"

#include <array>
#include <cstdint>

namespace uml::cpu::arm64 {

/* FEAT_AFP has no instructions of its own: it adds FPCR.FIZ, AH and NEP. The
 * virtual CPU implements it exactly where the host does, because both engines
 * then run every floating-point instruction as the host's own under the
 * guest FPCR, keep the old contents of the register a scalar result merges
 * into under NEP, and route the few operations they model in software (FCVT
 * with FPCR.AHP, FJCVTZS under FPCR.FZ) through models that follow all three
 * controls. Other software models stand in for a host extension, so a host
 * that lacks FEAT_FP16, FEAT_FHM, FEAT_BF16, FEAT_FRINTTS, FEAT_FCMA or
 * FEAT_JSCVT keeps the controls RES0 even if it has FEAT_AFP. This is the
 * one predicate behind HWCAP2_AFP, ID_AA64MMFR1_EL1.AFP, the FPCR write
 * mask, and the engines' choice of code for those controls. */
[[nodiscard]] inline bool implements_afp(const ArmFeatureSet &host) noexcept
{
	constexpr std::array required{
		ArmFeature::afp, ArmFeature::fp16, ArmFeature::fhm,
		ArmFeature::bf16, ArmFeature::frintts, ArmFeature::fcma,
		ArmFeature::jscvt};
	for (const auto feature : required)
		if (!host.test(static_cast<std::size_t>(feature)))
			return false;
	return true;
}

[[nodiscard]] inline bool implements_afp(
	const HostCapabilities &host) noexcept
{
	return implements_afp(host.supported);
}

/* FEAT_RPRES raises the single-precision FRECPE and FRSQRTE estimates to 12
 * bits while FPCR.AH is set. It needs FEAT_AFP, and both engines execute
 * those instructions as the host's own, so the host decides. */
[[nodiscard]] inline bool implements_rpres(
	const HostCapabilities &host) noexcept
{
	return implements_afp(host) && host.has(ArmFeature::rpres);
}

/* The FPCR fields an MSR FPCR or a context restore can set on this host:
 * fpcr_writable_mask, plus FIZ, AH and NEP where the virtual CPU implements
 * FEAT_AFP. */
[[nodiscard]] inline std::uint32_t fpcr_writable_mask_for(
	const ArmFeatureSet &host) noexcept
{
	return implements_afp(host) ?
		fpcr_writable_mask | fpcr_afp_controls : fpcr_writable_mask;
}

[[nodiscard]] inline std::uint32_t fpcr_writable_mask_for(
	const HostCapabilities &host) noexcept
{
	return fpcr_writable_mask_for(host.supported);
}

} // namespace uml::cpu::arm64
