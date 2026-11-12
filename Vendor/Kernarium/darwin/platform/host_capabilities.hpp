// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace uml::platform {

/* Every A64 feature the virtual CPU names, one X(token, "Label") row each, in
 * enumerator order. The token is the stable machine-readable name shared with
 * the architectural instruction inventory and its execution evidence
 * (tools/arm64-tests/build-unit-corpus.py and
 * tests/tooling/arm64/validate_a64_instruction_inventory.py read these rows);
 * the label is Arm's conventional capitalization, which also names the
 * hw.optional.arm.FEAT_<label> probe. */
#define UML_ARM_INVENTORY_FEATURES(X) \
	X(aes, "AES") \
	X(asmv8p2, "ASMv8p2") \
	X(ats1a, "ATS1A") \
	X(advanced_simd, "AdvSIMD") \
	X(bf16, "BF16") \
	X(brbe, "BRBE") \
	X(bti, "BTI") \
	X(chk, "CHK") \
	X(clrbhb, "CLRBHB") \
	X(cmh, "CMH") \
	X(cmpbr, "CMPBR") \
	X(cpa, "CPA") \
	X(crc32, "CRC32") \
	X(cssc, "CSSC") \
	X(d128, "D128") \
	X(dgh, "DGH") \
	X(dit, "DIT") \
	X(dpb, "DPB") \
	X(dpb2, "DPB2") \
	X(dotprod, "DotProd") \
	X(e3dse, "E3DSE") \
	X(ebep, "EBEP") \
	X(ebf16, "EBF16") \
	X(f16f32dot, "F16F32DOT") \
	X(f16f32mm, "F16F32MM") \
	X(f16mm, "F16MM") \
	X(f8f16mm, "F8F16MM") \
	X(f8f32mm, "F8F32MM") \
	X(faminmax, "FAMINMAX") \
	X(fcma, "FCMA") \
	X(fhm, "FHM") \
	X(floating_point, "FP") \
	X(fp16, "FP16") \
	X(fp8, "FP8") \
	X(fp8dot2, "FP8DOT2") \
	X(fp8dot4, "FP8DOT4") \
	X(fp8fma, "FP8FMA") \
	X(fprcvt, "FPRCVT") \
	X(frintts, "FRINTTS") \
	X(flag_m, "FlagM") \
	X(flag_m2, "FlagM2") \
	X(gcie, "GCIE") \
	X(gcs, "GCS") \
	X(hbc, "HBC") \
	X(hinte, "HINTE") \
	X(i8mm, "I8MM") \
	X(ite, "ITE") \
	X(jscvt, "JSCVT") \
	X(lor, "LOR") \
	X(lrcpc, "LRCPC") \
	X(lrcpc2, "LRCPC2") \
	X(lrcpc3, "LRCPC3") \
	X(ls64, "LS64") \
	X(ls64_accdata, "LS64_ACCDATA") \
	X(ls64_v, "LS64_V") \
	X(lscp, "LSCP") \
	X(lse, "LSE") \
	X(lse128, "LSE128") \
	X(lsfe, "LSFE") \
	X(lsui, "LSUI") \
	X(lut, "LUT") \
	X(mec, "MEC") \
	X(mops, "MOPS") \
	X(mte, "MTE") \
	X(mte2, "MTE2") \
	X(mtetc, "MTETC") \
	X(nmi, "NMI") \
	X(occmo, "OCCMO") \
	X(pan, "PAN") \
	X(pan2, "PAN2") \
	X(pauth, "PAuth") \
	X(pauth_lr, "PAuth_LR") \
	X(pcdphint, "PCDPHINT") \
	X(pmull, "PMULL") \
	X(prfmslc, "PRFMSLC") \
	X(pops, "PoPS") \
	X(ras, "RAS") \
	X(rdm, "RDM") \
	X(rme, "RME") \
	X(rme_gpc3, "RME_GPC3") \
	X(rprfm, "RPRFM") \
	X(sb, "SB") \
	X(sha1, "SHA1") \
	X(sha256, "SHA256") \
	X(sha3, "SHA3") \
	X(sha512, "SHA512") \
	X(sm3, "SM3") \
	X(sm4, "SM4") \
	X(sme, "SME") \
	X(spe, "SPE") \
	X(specres, "SPECRES") \
	X(specres2, "SPECRES2") \
	X(spe_exc, "SPE_EXC") \
	X(ssbs, "SSBS") \
	X(sysinstr128, "SYSINSTR128") \
	X(sysreg128, "SYSREG128") \
	X(the, "THE") \
	X(tlbid, "TLBID") \
	X(tlbios, "TLBIOS") \
	X(tlbirange, "TLBIRANGE") \
	X(tlbiw, "TLBIW") \
	X(trbe_exc, "TRBE_EXC") \
	X(trf, "TRF") \
	X(uao, "UAO") \
	X(vhe, "VHE") \
	X(wfxt, "WFxT") \
	X(xs, "XS") \
	/* The SME profile beyond FEAT_SME itself: SME2 (ZT0, multi-vector and \
	 * predicate-as-counter forms) and the two optional outer-product \
	 * widths. The prober reports these, FEAT_SME and the streaming vector \
	 * length only where the guest-SME policy allows it. */ \
	X(sme2, "SME2") \
	X(sme_f64f64, "SME_F64F64") \
	X(sme_i16i64, "SME_I16I64") \
	/* The non-streaming SVE features: FEAT_SVE and FEAT_SVE2, with \
	 * FEAT_SVE_AES, FEAT_SVE_PMULL128 and FEAT_SVE_SHA3. The virtual CPU \
	 * implements none of them (without_sve), whatever the host reports; \
	 * the tokens stay as the architectural inventory. */ \
	X(sve, "SVE") \
	X(sve2, "SVE2") \
	X(sve_aes, "SVE_AES") \
	X(sve_pmull128, "SVE_PMULL128") \
	X(sve_sha3, "SVE_SHA3")

/* The features after the instruction inventory's. ArmFeature::inventory_count,
 * the number of inventory features, is also the first of them. */
#define UML_ARM_EXTRA_FEATURES(X) \
	X(afp, "AFP") \
	X(lse2, "LSE2") \
	/* FEAT_ECV adds no instruction, only the self-synchronized counter \
	 * registers CNTVCTSS_EL0 and CNTPCTSS_EL0. */ \
	X(ecv, "ECV") \
	/* FEAT_RPRES adds no instruction either: under FPCR.AH it raises the \
	 * single-precision FRECPE and FRSQRTE estimates to 12 bits. */ \
	X(rpres, "RPRES")

enum class ArmFeature : std::size_t {
#define UML_ARM_FEATURE_ENUMERATOR(token, label) token,
	UML_ARM_INVENTORY_FEATURES(UML_ARM_FEATURE_ENUMERATOR)
	UML_ARM_EXTRA_FEATURES(UML_ARM_FEATURE_ENUMERATOR)
#undef UML_ARM_FEATURE_ENUMERATOR
	count,
	inventory_count = afp,
};

inline constexpr std::array arm_feature_labels{
#define UML_ARM_FEATURE_LABEL(token, label) std::string_view{label},
	UML_ARM_INVENTORY_FEATURES(UML_ARM_FEATURE_LABEL)
	UML_ARM_EXTRA_FEATURES(UML_ARM_FEATURE_LABEL)
#undef UML_ARM_FEATURE_LABEL
};

inline constexpr std::array arm_feature_tokens{
#define UML_ARM_FEATURE_TOKEN(token, label) std::string_view{#token},
	UML_ARM_INVENTORY_FEATURES(UML_ARM_FEATURE_TOKEN)
	UML_ARM_EXTRA_FEATURES(UML_ARM_FEATURE_TOKEN)
#undef UML_ARM_FEATURE_TOKEN
};

inline constexpr std::size_t arm_inventory_feature_count =
	static_cast<std::size_t>(ArmFeature::inventory_count);
inline constexpr std::size_t arm_feature_count =
	static_cast<std::size_t>(ArmFeature::count);
static_assert(arm_inventory_feature_count == 115);

using ArmFeatureSet = std::bitset<arm_feature_count>;

struct HostCapabilities final {
	ArmFeatureSet supported;
	bool floating_point{};
	bool advanced_simd{};
	bool fp16{};
	bool cssc{};
	bool aes{};
	bool pmull{};
	bool sha1{};
	bool sha2{};
	bool crc32{};
	bool sha3{};
	bool sha512{};
	bool jscvt_subnormal_flags_correct{};
	/* FEAT_SME in hardware, whatever the guest-SME policy: it only tells
	 * host code that it may read SVCR, and is never a reason to advertise
	 * or execute SME. The prober alone sets it, so a profile a test
	 * builds by hand never claims it. */
	bool sme_hardware{};
	/* The streaming vector length in bytes (a power of two from 16 to
	 * 256) when the SME tokens are reported, else 0. */
	std::uint16_t sme_svl_bytes{};

	[[nodiscard]] bool has(ArmFeature feature) const noexcept
	{
		return supported.test(static_cast<std::size_t>(feature));
	}

	[[nodiscard]] bool supports(
		const ArmFeatureSet &requirements) const noexcept
	{
		return (supported & requirements) == requirements;
	}

	/* Two hosts get the same generated code exactly when their profiles
	 * compare equal, so executors may then share a pool session or a
	 * translation. Memberwise, so padding cannot tell equal profiles
	 * apart. */
	[[nodiscard, gnu::always_inline]] bool operator==(
		const HostCapabilities &) const noexcept = default;
};

/* `host` without the non-streaming SVE tokens: no supported host implements
 * SVE and the virtual CPU presents none, so neither the prober nor an
 * engine's presented profile carries one. */
[[nodiscard]] inline HostCapabilities without_sve(
	HostCapabilities host) noexcept
{
	for (const auto feature : {ArmFeature::sve, ArmFeature::sve2,
		     ArmFeature::sve_aes, ArmFeature::sve_pmull128,
		     ArmFeature::sve_sha3})
		host.supported.reset(static_cast<std::size_t>(feature));
	return host;
}

} // namespace uml::platform
