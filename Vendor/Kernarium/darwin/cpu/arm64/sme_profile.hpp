// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/detail/sme_encoding_types.hpp"
#include "cpu/host_capabilities.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

namespace uml::cpu::arm64 {

/* The SME profile of the virtual CPU and the state it adds, shared by both
 * engines, the runtime's state transfer and the tests.
 *
 * The profile is a property of the virtual CPU, not of the host: FEAT_SME
 * with FEAT_SME_F64F64 and FEAT_SME_I16I64 (and the sub-features FEAT_SME
 * implies), plus FEAT_SME2 where the host has it. A host that lacks a part
 * gets none of it, and a host with more (FA64, SME2p1) still gets exactly
 * this. The streaming vector length (SVL) is the host's, whatever it is:
 * native execution needs guest SVL = host SVL, and EL0 cannot change it, so
 * every size below is a function of one SVL fixed at start-up. */
enum class SmeProfile : std::uint8_t {
	none,
	sme,
	sme2,
};

/* How far an engine implements the profile: nothing (every SVE and SME
 * word is the illegal-instruction exit), the control core, all of it but
 * the SME2 forms, or all of it. Both engines are at complete (checked in
 * engine_factory.cpp), and a guest sees FEAT_SME2 only from an engine at
 * sme2 (sme_host_for_support). */
enum class SmeSupport : std::uint8_t {
	none,
	control,
	complete,
	sme2,
};

/* A streaming vector length an SME implementation can have, in bytes. */
[[nodiscard]] constexpr bool valid_streaming_vector_length(
	unsigned bytes) noexcept
{
	return bytes >= 16U && bytes <= 256U && (bytes & (bytes - 1U)) == 0U;
}

/* The profile the virtual CPU implements on `host`. The prober reports the
 * SME tokens only where the guest-SME policy allows it, so this is the policy
 * too; nothing here depends on the value of SVL. */
[[nodiscard]] inline SmeProfile sme_profile(const HostCapabilities &host) noexcept
{
	if (!host.has(ArmFeature::sme) || !host.has(ArmFeature::sme_f64f64) ||
	    !host.has(ArmFeature::sme_i16i64) ||
	    !valid_streaming_vector_length(host.sme_svl_bytes))
		return SmeProfile::none;
	return host.has(ArmFeature::sme2) ? SmeProfile::sme2 : SmeProfile::sme;
}

[[nodiscard]] inline bool implements_sme(const HostCapabilities &host) noexcept
{
	return sme_profile(host) != SmeProfile::none;
}

/* The host an engine at `support` presents the virtual CPU for: `host`
 * less FEAT_SME2 below sme2, so its identification registers and the
 * guest's HWCAPs, which derive from the profile, advertise no SME2 form
 * the engine would take as illegal. A host without SME2, which includes
 * every host while the guest-SME policy is off, comes back unchanged. */
[[nodiscard]] inline HostCapabilities sme_host_for_support(
	HostCapabilities host, SmeSupport support) noexcept
{
	if (support < SmeSupport::sme2)
		host.supported.reset(static_cast<std::size_t>(ArmFeature::sme2));
	return host;
}

/* The encoding table names its requirement bits after the ArmFeature
 * tokens; this is the one place that translates between them. The SVE
 * profile's withheld FEAT_SVE_BitPerm and FEAT_SVE_SM4 have table bits but
 * no token. */
[[nodiscard]] inline detail::SmeFeatureMask sme_feature_mask(
	const ArmFeatureSet &features) noexcept
{
	const auto bit = [&](ArmFeature feature, detail::SmeFeatureMask mask) {
		return features[static_cast<std::size_t>(feature)] ? mask :
			detail::SmeFeatureMask{};
	};
	return static_cast<detail::SmeFeatureMask>(
		bit(ArmFeature::sme, detail::sme_feature_sme) |
		bit(ArmFeature::sme2, detail::sme_feature_sme2) |
		bit(ArmFeature::sme_f64f64, detail::sme_feature_sme_f64f64) |
		bit(ArmFeature::sme_i16i64, detail::sme_feature_sme_i16i64) |
		bit(ArmFeature::bf16, detail::sme_feature_bf16) |
		bit(ArmFeature::i8mm, detail::sme_feature_i8mm) |
		bit(ArmFeature::sve, detail::sme_feature_sve) |
		bit(ArmFeature::sve2, detail::sme_feature_sve2) |
		bit(ArmFeature::sve_aes, detail::sme_feature_sve_aes) |
		bit(ArmFeature::sve_pmull128, detail::sme_feature_sve_pmull128) |
		bit(ArmFeature::sve_sha3, detail::sme_feature_sve_sha3));
}

/* The table rows the virtual CPU implements on `host` in streaming mode,
 * as their requirement bits: none without the SME profile; otherwise the
 * profile's parts, SME2 only in the SME2 profile, and BF16 and I8MM where
 * the virtual CPU has them, which is where the host does. No SVE token
 * enters it: streaming-mode requirements name no SVE feature. */
[[nodiscard]] inline detail::SmeFeatureMask implemented_sme_features(
	const HostCapabilities &host) noexcept
{
	const SmeProfile profile = sme_profile(host);
	if (profile == SmeProfile::none)
		return {};
	ArmFeatureSet features;
	for (const ArmFeature feature : {ArmFeature::sme,
		     ArmFeature::sme_f64f64, ArmFeature::sme_i16i64})
		features.set(static_cast<std::size_t>(feature));
	if (profile == SmeProfile::sme2)
		features.set(static_cast<std::size_t>(ArmFeature::sme2));
	for (const ArmFeature feature : {ArmFeature::bf16, ArmFeature::i8mm})
		if (host.has(feature))
			features.set(static_cast<std::size_t>(feature));
	return sme_feature_mask(features);
}

/* SVCR: PSTATE.SM in bit 0 and PSTATE.ZA in bit 1. The lazy-transfer bits
 * (CpuState::sme_loaded and sme_dirty) name the state each mode makes live
 * with the same bits: Z/P while SM is set, ZA/ZT0 while ZA is. */
inline constexpr std::uint8_t svcr_sm = 1U;
inline constexpr std::uint8_t svcr_za = 2U;
inline constexpr std::uint8_t svcr_mask = svcr_sm | svcr_za;

/* CpuState::sme_host_mode, engine-private: the host's own SM and ZA, and
 * whether guest Z/P (JIT) or ZA/ZT0 are resident in host registers. */
inline constexpr std::uint8_t sme_host_streaming = 1U;
inline constexpr std::uint8_t sme_host_za = 2U;
inline constexpr std::uint8_t sme_host_streaming_resident = 4U;
inline constexpr std::uint8_t sme_host_za_resident = 8U;

/* ResetSVEState(): the FPSR every change of PSTATE.SM leaves. */
inline constexpr std::uint32_t streaming_mode_change_fpsr = 0x0800'009fU;

/* The SME register file a vCPU carries and the task storage the kernel keeps
 * have one layout: Z0-Z31 of SVL bytes, P0-P15 of SVL/8 bytes, ZA as its SVL
 * horizontal vectors of SVL bytes, then the 64-byte ZT0. Every part starts at
 * a multiple of SVL, so `mul vl` addressing reaches it from a base register
 * at its start. The bridge's DARWIN_UML_GUEST_SME_STATE_SIZE(svl) equals
 * sme_storage_size(svl). */
inline constexpr unsigned sme_z_registers = 32U;
inline constexpr unsigned sme_p_registers = 16U;
inline constexpr std::size_t sme_zt0_bytes = 64U;
inline constexpr std::size_t sme_p_offset_vl = sme_z_registers;
inline constexpr std::size_t sme_za_offset_vl =
	sme_p_offset_vl + sme_p_registers / 8U;

[[nodiscard]] constexpr std::size_t sme_z_offset(std::size_t svl,
	unsigned n) noexcept
{
	return n * svl;
}

[[nodiscard]] constexpr std::size_t sme_p_offset(std::size_t svl,
	unsigned n) noexcept
{
	return sme_p_offset_vl * svl + n * (svl / 8U);
}

[[nodiscard]] constexpr std::size_t sme_za_offset(std::size_t svl,
	unsigned row) noexcept
{
	return sme_za_offset_vl * svl + row * svl;
}

[[nodiscard]] constexpr std::size_t sme_zt0_offset(std::size_t svl) noexcept
{
	return sme_za_offset(svl, 0) + svl * svl;
}

[[nodiscard]] constexpr std::size_t sme_storage_size(std::size_t svl) noexcept
{
	return sme_zt0_offset(svl) + sme_zt0_bytes;
}

/* Every part of the file starts on an SVL boundary and on a cache line for
 * any SVL. */
inline constexpr std::size_t sme_register_file_alignment = 256U;

static_assert(sme_storage_size(64) == 6336U);
static_assert(sme_storage_size(16) == 16U * 34U + 16U * 16U + 64U);
static_assert(sme_storage_size(256) == 256U * 34U + 256U * 256U + 64U);

/* A guest write of SVCR (SMSTART and SMSTOP in their three forms, MSR SVCR):
 * the bits in `mask` take their values from `value`. The architecture's
 * rules (SetPSTATE_SM, SetPSTATE_ZA, ResetSVEState, ResetSMEState):
 * - SM 0->1 zeroes Z and P, and 1->0 zeroes Z and P too, which outside
 *   streaming mode leaves the V registers zero; either sets FPSR to
 *   0x0800009f. The virtual CPU has no FFR and no non-streaming P to zero;
 * - ZA 0->1 zeroes ZA and ZT0; 1->0 leaves them ARBITRARY, and the next 0->1
 *   zeroes them anyway;
 * - writing the value a bit already holds changes nothing.
 * A task without SME storage has SVCR 0; a write that would set SM or ZA
 * there applies nothing and asks for storage instead (the sme_access exit,
 * which retires nothing). */
struct SvcrWrite final {
	std::uint8_t svcr{};
	/* SM 0->1: zero Z and P; V reads as their low bytes from now on. */
	bool zero_streaming{};
	/* SM 1->0: zero the V registers. */
	bool zero_vectors{};
	/* SM changed: FPSR := streaming_mode_change_fpsr. */
	bool reset_fpsr{};
	/* ZA 0->1: zero ZA and ZT0. */
	bool zero_za{};
	bool needs_storage{};
};

[[nodiscard]] constexpr SvcrWrite svcr_write(std::uint8_t current,
	std::uint8_t mask, std::uint8_t value, bool storage) noexcept
{
	current &= svcr_mask;
	const auto next = static_cast<std::uint8_t>(
		((current & ~mask) | (value & mask)) & svcr_mask);
	const auto enabled = static_cast<std::uint8_t>(next & ~current);
	if (!storage && enabled != 0U)
		return {.svcr = current, .needs_storage = true};
	const bool streaming_changed = ((next ^ current) & svcr_sm) != 0U;
	return {
		.svcr = next,
		.zero_streaming = (enabled & svcr_sm) != 0U,
		.zero_vectors = streaming_changed && (next & svcr_sm) == 0U,
		.reset_fpsr = streaming_changed,
		.zero_za = (enabled & svcr_za) != 0U,
	};
}

static_assert(svcr_write(0, svcr_mask, svcr_mask, true).zero_streaming);
static_assert(svcr_write(0, svcr_mask, svcr_mask, true).zero_za);
static_assert(svcr_write(svcr_mask, svcr_sm, 0, true).zero_vectors);
static_assert(svcr_write(svcr_mask, svcr_sm, 0, true).svcr == svcr_za);
static_assert(!svcr_write(svcr_sm, svcr_sm, svcr_sm, true).reset_fpsr);
static_assert(svcr_write(0, svcr_za, svcr_za, false).needs_storage);
static_assert(!svcr_write(0, svcr_za, 0, false).needs_storage);

/* The host's own SVCR. MRS SVCR is UNDEFINED without FEAT_SME in hardware,
 * so only code that has seen HostCapabilities::sme_hardware may read it; the
 * register is named by its encoding, so no target needs the SME assembler
 * extension. */
[[nodiscard]] inline std::uint64_t read_host_svcr() noexcept
{
	std::uint64_t value;
	asm volatile("mrs %0, S3_3_C4_C2_2" : "=r"(value));
	return value;
}

/* Everything but SM=1 translated code runs with the host out of streaming
 * mode and ZA off, which is also what AAPCS64 requires of a caller of
 * ordinary functions. True on a host without FEAT_SME, which never reads
 * SVCR, and for a profile a test injects, which never claims the hardware. */
[[nodiscard]] inline bool host_streaming_state_clear(
	const HostCapabilities &host) noexcept
{
	return !host.sme_hardware || read_host_svcr() == 0U;
}

/* Debug builds trap at a C++ boundary the host crosses in streaming mode or
 * with ZA on; release builds compile nothing. The runtime checks each engine
 * entry and return; the engines check their own leave sequences, which are
 * the only code that changes the host's mode. */
inline void debug_check_host_streaming_state(
	[[maybe_unused]] const HostCapabilities &host) noexcept
{
#ifndef NDEBUG
	if (!host_streaming_state_clear(host))
		__builtin_trap();
#endif
}

/* A vCPU's SME register file, sme_storage_size(svl) zeroed bytes allocated
 * once at start-up for the host's SVL, or nothing when the run does not
 * implement SME. CpuState points at it; its owner outlives every entry. */
class SmeRegisterFile final {
public:
	SmeRegisterFile() noexcept = default;

	explicit SmeRegisterFile(unsigned svl)
		: bytes_{static_cast<std::byte *>(::operator new(
			  sme_storage_size(svl),
			  std::align_val_t{sme_register_file_alignment}))},
		  svl_{svl}
	{
		std::memset(bytes_.get(), 0, sme_storage_size(svl));
	}

	[[nodiscard]] static SmeRegisterFile for_host(const HostCapabilities &host)
	{
		return implements_sme(host) ? SmeRegisterFile{host.sme_svl_bytes} :
			SmeRegisterFile{};
	}

	[[nodiscard]] std::byte *data() const noexcept
	{
		return bytes_.get();
	}

	[[nodiscard]] unsigned svl() const noexcept
	{
		return svl_;
	}

private:
	struct Release final {
		void operator()(std::byte *bytes) const noexcept
		{
			::operator delete(bytes,
				std::align_val_t{sme_register_file_alignment});
		}
	};

	std::unique_ptr<std::byte, Release> bytes_;
	unsigned svl_{};
};

} // namespace uml::cpu::arm64
