// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/decode_types.hpp"
#include "cpu/host_capabilities.hpp"

#include <cstdint>

namespace uml::cpu::arm64 {

/* The AArch32 EL0 profile of the virtual CPU, shared by both engines, the
 * runtime's state transfer and the tests (docs/architecture.md).
 *
 * The profile is Armv8-A AArch32 at EL0: the A32 and T32 instruction sets
 * with interworking and IT, the Armv8 load-acquire and store-release group,
 * VFPv4-D32 and Advanced SIMD, the Armv8 floating-point additions (VSEL,
 * VMAXNM, VRINT and VCVT with a rounding mode), and, each where the host has
 * the A64 feature the engines lower it onto, CRC32, AES, PMULL, SHA1, SHA2,
 * FP16 arithmetic, the dot product, FHM, BF16, I8MM, SB, RDM, FCMA and
 * JSCVT. It has no SWP, SETEND, short vectors, floating-point trap enables
 * or ThumbEE, and every encoding outside it is UNDEFINED. */

/* Which optional parts of the profile a guest sees on a host, each where the
 * A64 host predicate its lowering uses holds. The AArch32 identification
 * registers (detail::a32_id_register_value) and the compat HWCAPs derive from
 * this and from nothing else. The integer core, the Armv8 load-acquire and
 * store-release group (which arm64 Linux's compat kuser helpers and vDSO may
 * execute), VFPv4-D32, Advanced SIMD and the Armv8 floating-point additions
 * every host supports are always shown. */
[[nodiscard]] inline aarch32::FeatureSet a32_features(
	const HostCapabilities &host) noexcept
{
	using aarch32::Feature;
	aarch32::FeatureSet features;
	const auto add = [&features](bool present, Feature feature) noexcept {
		if (present)
			features = features.with(feature);
	};
	add(host.crc32, Feature::crc32);
	add(host.aes, Feature::aes);
	add(host.aes && host.pmull, Feature::pmull);
	add(host.sha1, Feature::sha1);
	add(host.sha2, Feature::sha2);
	add(host.fp16, Feature::fp16);
	add(host.has(ArmFeature::dotprod), Feature::dotprod);
	add(host.has(ArmFeature::fhm), Feature::fhm);
	add(host.has(ArmFeature::bf16), Feature::bf16);
	add(host.has(ArmFeature::i8mm), Feature::i8mm);
	add(host.has(ArmFeature::sb), Feature::sb);
	add(host.has(ArmFeature::rdm), Feature::rdm);
	add(host.has(ArmFeature::fcma), Feature::fcma);
	add(host.has(ArmFeature::jscvt), Feature::jscvt);
	return features;
}

/* The profile an engine decodes AArch32 against on this host: exactly the
 * optional extensions the guest is shown (a32_features). A form of an
 * extension the host lacks is therefore UNDEFINED, as the HWCAPs and the
 * identification registers say, and an engine never executes it, whether it
 * lowers the extension or runs it in the semantic library. */
[[nodiscard]] inline aarch32::Profile a32_decode_profile(
	const HostCapabilities &host) noexcept
{
	return aarch32::Profile{a32_features(host)};
}

} // namespace uml::cpu::arm64
