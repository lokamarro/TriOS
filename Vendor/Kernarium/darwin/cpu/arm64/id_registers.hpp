/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "cpu/arm64/aarch32/a32_profile.hpp"
#include "cpu/arm64/floating_controls.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/host_capabilities.hpp"

#include <array>
#include <cstdint>

namespace uml::cpu::arm64::detail {

/* EL0 reads of the AArch64 identification registers are UNDEFINED in hardware.
 * Linux catches that undefined instruction and completes the read with a
 * sanitized value instead (`try_emulate_mrs()` in
 * arch/arm64/kernel/cpufeature.c), so guest userspace executes them
 * unconditionally; AT_HWCAP advertises the facility as HWCAP_CPUID. Wine's
 * unix-side ntdll reads ID_AA64PFR0_EL1 during process start, which is why an
 * engine that reports these as illegal cannot load a Windows program at all.
 *
 * Linux emulates exactly Op0 == 3, Op1 == 0, CRn == 0, and CRm either 0 or in
 * [2, 7]. Everything else in the EL1 encoding space stays illegal here, as it
 * does on arm64.
 */
[[nodiscard]] inline constexpr bool emulated_id_register(
	std::uint32_t instruction) noexcept
{
	if ((instruction & 0xffff'f000U) != 0xd538'0000U)
		return false;
	const unsigned group = (instruction >> 8) & 15U;
	const unsigned selector = (instruction >> 5) & 7U;
	if (group == 0U)
		/* MIDR_EL1, MPIDR_EL1 and REVIDR_EL1 are the only registers Linux
		 * emulates in this group; the rest of it is unallocated. */
		return selector == 0U || selector == 5U || selector == 6U;
	return group >= 2U && group <= 7U;
}

/* This virtual CPU holds no JEP106 implementer code, part number or revision.
 * Architecture 0b1111 is the architectural encoding for "the identification
 * registers describe this implementation", which is exactly what a guest
 * reading MIDR_EL1 here must fall back on. The kernel, which cannot execute
 * this read, prints the same fields in /proc/cpuinfo from a copy of the value
 * (show_arm64_cpuinfo() in the shared kernel series), and the engine smoke
 * tests fail when the two disagree. */
inline constexpr std::uint64_t emulated_midr_el1 = 0x000f'0000ULL;

/* Linux reports the reserved-bit-only affinity rather than the caller's
 * topology (SYS_MPIDR_SAFE_VAL); keep the same value so guest userspace cannot
 * derive a vCPU identity from a read that carries none on arm64 either. */
inline constexpr std::uint64_t emulated_mpidr_el1 = 0x8000'0000ULL;

/* The registers whose fields carry features, by the MRS that reads each into
 * X0. */
inline constexpr std::uint32_t id_aa64pfr0_el1 = 0xd538'0400U;
inline constexpr std::uint32_t id_aa64pfr1_el1 = 0xd538'0420U;
inline constexpr std::uint32_t id_aa64zfr0_el1 = 0xd538'0480U;
inline constexpr std::uint32_t id_aa64smfr0_el1 = 0xd538'04a0U;
inline constexpr std::uint32_t id_aa64isar0_el1 = 0xd538'0600U;
inline constexpr std::uint32_t id_aa64isar1_el1 = 0xd538'0620U;
inline constexpr std::uint32_t id_aa64isar2_el1 = 0xd538'0640U;
inline constexpr std::uint32_t id_aa64mmfr0_el1 = 0xd538'0700U;
inline constexpr std::uint32_t id_aa64mmfr1_el1 = 0xd538'0720U;
inline constexpr std::uint32_t id_aa64mmfr2_el1 = 0xd538'0740U;

/* These values are the virtual CPU's advertised feature set, and AT_HWCAP
 * and AT_HWCAP2 are read from them (`guest_hwcaps()` below). A field reports
 * a feature only where an engine has committed to executing it, on the host
 * predicate its native path uses, never the host's whole feature set: a guest
 * that selects an opcode from either answer must not reach one an engine
 * would treat as illegal. Fields Linux hides from userspace (the exception
 * levels, ID_AA64MMFR0_EL1 except ECV, ID_AA64MMFR1_EL1 except AFP, debug and
 * IMPLEMENTATION DEFINED registers) read as zero here for the same reason
 * they do on arm64: they describe state EL0 cannot act on.
 */
[[nodiscard]] inline std::uint64_t emulated_id_register_value(
	std::uint32_t instruction) noexcept
{
	const unsigned group = (instruction >> 8) & 15U;
	const unsigned selector = (instruction >> 5) & 7U;
	const auto &host = ::uml::cpu::host_capabilities();
	const auto field = [](std::uint64_t value, unsigned position) noexcept {
		return value << position;
	};
	if (group == 0U) {
		if (selector == 0U)
			return emulated_midr_el1;
		if (selector == 5U)
			return emulated_mpidr_el1;
		/* REVIDR_EL1 is IMPLEMENTATION DEFINED, and Linux emulates it
		 * with zero. */
		return 0;
	}
	if (group == 4U && selector == 0U) {
		/* ID_AA64PFR0_EL1. An FP or AdvSIMD field of zero already means
		 * "implemented"; one additionally means half precision, whose
		 * native scalar and vector paths both engines gate on this
		 * probe. SVE stays zero: the virtual CPU has no non-streaming
		 * SVE. DIT: PSTATE.DIT is settable and readable. */
		std::uint64_t value = 0;
		if (host.fp16)
			value |= field(1, 16) | field(1, 20);
		if (host.has(ArmFeature::dit))
			value |= field(1, 48);
		return value;
	}
	if (group == 4U && selector == 1U) {
		/* ID_AA64PFR1_EL1.SME, its one field this virtual CPU sets:
		 * FEAT_SME, or FEAT_SME2 in the SME2 profile. */
		switch (sme_profile(host)) {
		case SmeProfile::none:
			return 0;
		case SmeProfile::sme:
			return field(1, 24);
		case SmeProfile::sme2:
			return field(2, 24);
		}
		return 0;
	}
	if (group == 4U && selector == 4U) {
		/* ID_AA64ZFR0_EL1 is zero: the virtual CPU has no
		 * non-streaming SVE, and Linux reads the register only with
		 * it. */
		return 0;
	}
	if (group == 4U && selector == 5U) {
		/* ID_AA64SMFR0_EL1, every field arm64 Linux shows EL0, for the
		 * profile (sme_profile.hpp): F32F32, B16F32, F16F32, I8I32,
		 * F64F64 and I16I64 always; SME2 adds SMEver 1, I16I32 and
		 * BI32I32. FA64, B16B16, F16F16, the FP8 fields and LUTv2 stay
		 * zero. Nothing here depends on the streaming vector length. */
		const auto profile = sme_profile(host);
		if (profile == SmeProfile::none)
			return 0;
		std::uint64_t value = field(1, 32) | field(1, 34) |
			field(1, 35) | field(15, 36) | field(1, 48) |
			field(15, 52);
		if (profile == SmeProfile::sme2)
			value |= field(1, 33) | field(5, 44) | field(1, 56);
		return value;
	}
	if (group == 6U && selector == 0U) {
		/* ID_AA64ISAR0_EL1. The crypto, CRC32, LSE, RDM, dot-product
		 * and FHM families run as the host's own instructions. TS is
		 * CFINV, RMIF and SETF, then AXFLAG and XAFLAG. */
		std::uint64_t value = 0;
		if (host.aes)
			value |= field(host.pmull ? 2 : 1, 4);
		if (host.sha1)
			value |= field(1, 8);
		if (host.sha2 || host.sha512)
			value |= field(host.sha512 ? 2 : 1, 12);
		if (host.crc32)
			value |= field(1, 16);
		if (host.has(ArmFeature::lse))
			value |= field(2, 20);
		if (host.has(ArmFeature::rdm))
			value |= field(1, 28);
		if (host.sha3)
			value |= field(1, 32);
		if (host.has(ArmFeature::dotprod))
			value |= field(1, 44);
		if (host.has(ArmFeature::fhm))
			value |= field(1, 48);
		if (host.has(ArmFeature::flag_m))
			value |= field(
				host.has(ArmFeature::flag_m2) ? 2 : 1, 52);
		return value;
	}
	if (group == 6U && selector == 1U) {
		/* ID_AA64ISAR1_EL1. Both engines correct the Z flag FJCVTZS
		 * sets on some cores, such as the M1, for a subnormal FPCR.FZ
		 * flushes, so JSCVT follows the host. The gadget bank has a
		 * native RCpc load and store for every size. DPB stays zero
		 * because no engine implements DC CVAP, and the
		 * pointer-authentication fields because the virtual CPU has no
		 * FEAT_PAuth. */
		std::uint64_t value = 0;
		if (host.has(ArmFeature::jscvt))
			value |= field(1, 12);
		if (host.has(ArmFeature::fcma))
			value |= field(1, 16);
		if (host.has(ArmFeature::lrcpc))
			value |= field(
				host.has(ArmFeature::lrcpc2) ? 2 : 1, 20);
		if (host.has(ArmFeature::frintts))
			value |= field(1, 32);
		if (host.has(ArmFeature::sb))
			value |= field(1, 36);
		if (host.has(ArmFeature::bf16))
			value |= field(1, 44);
		if (host.has(ArmFeature::i8mm))
			value |= field(1, 52);
		return value;
	}
	if (group == 6U && selector == 2U)
		/* ID_AA64ISAR2_EL1. Both engines execute FEAT_MOPS, one host
		 * memmove or memset per page region, and FEAT_CSSC, natively or
		 * as base sequences, on every host; RPRES, the 12-bit estimates
		 * under FPCR.AH, follows the host. */
		return field(1, 16) | field(1, 52) |
			(implements_rpres(host) ? field(1, 4) : 0);
	if (group == 7U && selector == 0U)
		/* ID_AA64MMFR0_EL1. ECV, the one field Linux shows EL0: both
		 * engines read CNTVCTSS_EL0 with the host's instruction. */
		return host.has(ArmFeature::ecv) ? field(1, 60) : 0;
	if (group == 7U && selector == 1U)
		/* ID_AA64MMFR1_EL1. AFP, the one field Linux shows EL0, on the
		 * predicate that names the host features exact FIZ, AH and
		 * NEP take. */
		return implements_afp(host) ? field(1, 44) : 0;
	if (group == 7U && selector == 2U)
		/* ID_AA64MMFR2_EL1.AT, FEAT_LSE2: an LSE atomic on an address
		 * that is not naturally aligned is single-copy atomic rather
		 * than a fault. */
		return host.has(ArmFeature::lse2) ? field(1, 32) : 0;
	/* Every remaining register in the emulated space describes AArch32
	 * state or an extension no engine executes, or is state Linux keeps
	 * hidden from EL0. */
	return 0;
}

/* A bit of AT_HWCAP or AT_HWCAP2 and the field it is read from: bit
 * `hwcap`, numbered as the kernel's KERNEL_HWCAP_* (AT_HWCAP's as 0-63,
 * AT_HWCAP2's as 64-127), is set where the `width`-bit field at `shift`
 * holds at least `minimum`. */
struct HwcapField final {
	std::uint32_t id_register;
	unsigned shift;
	unsigned minimum;
	unsigned hwcap;
	unsigned width{4};
};

/* arm64 Linux's own derivation (arm64_elf_hwcaps in
 * arch/arm64/kernel/cpufeature.c), restricted to the fields the values
 * above can set. */
inline constexpr HwcapField hwcap_fields[]{
	{id_aa64pfr0_el1, 16, 1, 9},		/* FP: FPHP */
	{id_aa64pfr0_el1, 20, 1, 10},		/* AdvSIMD: ASIMDHP */
	{id_aa64pfr0_el1, 48, 1, 24},		/* DIT */
	/* SME. Linux reads the ID_AA64SMFR0_EL1 bits only with SME
	 * (has_sme_feature()); that register is zero without it here. */
	{id_aa64pfr1_el1, 24, 1, 64 + 23},	/* SME */
	{id_aa64smfr0_el1, 56, 1, 64 + 37},	/* SMEver: SME2 */
	{id_aa64smfr0_el1, 52, 15, 64 + 24},	/* I16I64 */
	{id_aa64smfr0_el1, 48, 1, 64 + 25, 1},	/* F64F64 */
	{id_aa64smfr0_el1, 44, 5, 64 + 39},	/* I16I32 */
	{id_aa64smfr0_el1, 36, 15, 64 + 26},	/* I8I32 */
	{id_aa64smfr0_el1, 35, 1, 64 + 27, 1},	/* F16F32 */
	{id_aa64smfr0_el1, 34, 1, 64 + 28, 1},	/* B16F32 */
	{id_aa64smfr0_el1, 33, 1, 64 + 40, 1},	/* BI32I32 */
	{id_aa64smfr0_el1, 32, 1, 64 + 29, 1},	/* F32F32 */
	{id_aa64isar0_el1, 4, 1, 3},		/* AES */
	{id_aa64isar0_el1, 4, 2, 4},		/* AES: PMULL */
	{id_aa64isar0_el1, 8, 1, 5},		/* SHA1 */
	{id_aa64isar0_el1, 12, 1, 6},		/* SHA2 */
	{id_aa64isar0_el1, 12, 2, 21},		/* SHA2: SHA512 */
	{id_aa64isar0_el1, 16, 1, 7},		/* CRC32 */
	{id_aa64isar0_el1, 20, 2, 8},		/* Atomic: ATOMICS */
	{id_aa64isar0_el1, 28, 1, 12},		/* RDM: ASIMDRDM */
	{id_aa64isar0_el1, 32, 1, 17},		/* SHA3 */
	{id_aa64isar0_el1, 44, 1, 20},		/* DP: ASIMDDP */
	{id_aa64isar0_el1, 48, 1, 23},		/* FHM: ASIMDFHM */
	{id_aa64isar0_el1, 52, 1, 27},		/* TS: FLAGM */
	{id_aa64isar0_el1, 52, 2, 64 + 7},	/* TS: FLAGM2 */
	{id_aa64isar1_el1, 12, 1, 13},		/* JSCVT */
	{id_aa64isar1_el1, 16, 1, 14},		/* FCMA */
	{id_aa64isar1_el1, 20, 1, 15},		/* LRCPC */
	{id_aa64isar1_el1, 20, 2, 26},		/* LRCPC: ILRCPC */
	{id_aa64isar1_el1, 32, 1, 64 + 8},	/* FRINTTS: FRINT */
	{id_aa64isar1_el1, 36, 1, 29},		/* SB */
	{id_aa64isar1_el1, 44, 1, 64 + 14},	/* BF16 */
	{id_aa64isar1_el1, 52, 1, 64 + 13},	/* I8MM */
	{id_aa64isar2_el1, 4, 1, 64 + 21},	/* RPRES */
	{id_aa64isar2_el1, 16, 1, 64 + 43},	/* MOPS */
	{id_aa64isar2_el1, 52, 1, 64 + 34},	/* CSSC */
	{id_aa64mmfr0_el1, 60, 1, 64 + 19},	/* ECV */
	{id_aa64mmfr1_el1, 44, 1, 64 + 20},	/* AFP */
	{id_aa64mmfr2_el1, 32, 1, 25},		/* AT: USCAT */
};

/* AT_HWCAP and AT_HWCAP2 for a host, which `darwin_uml_guest_hwcap()` and
 * `darwin_uml_guest_hwcap2()` return to the guest kernel. Each word is read
 * from the identification registers exactly as arm64 Linux reads it, so a
 * guest gets the same answer from MRS and from its auxiliary vector. The
 * registers are read in a scope for `host`, because the kernel asks on a
 * thread where no engine need have installed one. */
[[nodiscard]] inline std::array<unsigned long, 2> guest_hwcaps(
	const HostCapabilities &host) noexcept
{
	/* FP and ASIMD, whose fields are never 0b1111 here; EVTSTRM, the
	 * promise that WFE wakes once per event-stream period, which holds
	 * because both engines complete WFE at once; and CPUID, the emulation
	 * of these reads itself, which Linux also sets unconditionally. */
	std::array<unsigned long, 2> words{
		(1UL << 0) | (1UL << 1) | (1UL << 2) | (1UL << 11), 0};
	const HostCapabilitiesScope scope{host};
	for (const auto &bit : hwcap_fields)
		if (((emulated_id_register_value(bit.id_register) >>
		      bit.shift) & ((1U << bit.width) - 1U)) >= bit.minimum)
			words[bit.hwcap / 64U] |= 1UL << (bit.hwcap % 64U);
	return words;
}

/* The AArch32 identification registers of the virtual CPU. EL0 cannot read
 * them -- MRC of an ID register is UNDEFINED at PL0, and arm64 Linux emulates
 * only the AArch64 MRS space -- so they exist to derive the compat HWCAPs
 * (guest_compat_hwcaps), as arm64 Linux does, and to document the profile.
 * The values follow a Cortex-A72 in AArch32 state, adjusted to the profile
 * (cpu/arm64/aarch32/a32_profile.hpp): the integer ISA with the Armv8
 * acquire/release group, VFP and Advanced SIMD, and the Armv8 additions,
 * each optional one where its A64 host predicate holds (a32_features). */
enum class A32IdRegister : std::uint8_t {
	midr,
	pfr0,
	pfr1,
	pfr2,
	isar0,
	isar1,
	isar2,
	isar3,
	isar4,
	isar5,
	isar6,
	mvfr0,
	mvfr1,
	mvfr2,
};

[[nodiscard]] inline std::uint32_t a32_id_register_value(
	const HostCapabilities &host, A32IdRegister which) noexcept
{
	using aarch32::Feature;
	const auto features = a32_features(host);
	const auto field = [](unsigned value, unsigned position) noexcept {
		return static_cast<std::uint32_t>(value) << position;
	};
	const auto flag = [](bool present) noexcept {
		return present ? 1U : 0U;
	};
	switch (which) {
	case A32IdRegister::midr:
		/* One identity for both views. */
		return static_cast<std::uint32_t>(emulated_midr_el1);
	case A32IdRegister::pfr0:
		/* State0 A32, State1 T32 with Thumb-2, State2 trivial Jazelle
		 * (BXJ is BX); State3 (ThumbEE) and DIT are absent. */
		return field(1, 0) | field(3, 4) | field(1, 8);
	case A32IdRegister::pfr1:
		/* GenTimer: the generic timer registers at EL0. */
		return field(1, 16);
	case A32IdRegister::pfr2:
		/* No SSBS. */
		return 0;
	case A32IdRegister::isar0:
		/* Divide 2 (SDIV and UDIV in A32 and T32), Debug, CmpBranch,
		 * BitField, BitCount; Swap 0, since SWP is UNDEFINED. */
		return field(2, 24) | field(1, 20) | field(1, 12) |
			field(1, 8) | field(1, 4);
	case A32IdRegister::isar1:
		/* Jazelle, Interwork 3, Immediate, IfThen, Extend 2, Except_AR,
		 * Except, Endian (SETEND is UNDEFINED at EL0). */
		return field(1, 28) | field(3, 24) | field(1, 20) | field(1, 16) |
			field(2, 12) | field(1, 8) | field(1, 4) | field(1, 0);
	case A32IdRegister::isar2:
		/* Reversal 2, PSR_AR, MultU 2, MultS 3, Mult 2, MemHint 4, and
		 * LoadStore 2 (LDRD, STRD, LDA and STL). */
		return field(2, 28) | field(1, 24) | field(2, 20) | field(3, 16) |
			field(2, 12) | field(4, 4) | field(2, 0);
	case A32IdRegister::isar3:
		/* TrueNOP, T32Copy, TabBranch, SynchPrim 2 (LDREXD), SVC, SIMD 3
		 * (the Armv6 packed forms), Saturate; T32EE 0. */
		return field(1, 24) | field(1, 20) | field(1, 16) | field(2, 12) |
			field(1, 8) | field(3, 4) | field(1, 0);
	case A32IdRegister::isar4:
		/* Barrier, SMC, Writeback, WithShifts 4, Unpriv 2. */
		return field(1, 16) | field(1, 12) | field(1, 8) | field(4, 4) |
			field(2, 0);
	case A32IdRegister::isar5:
		/* VCMA, RDM, CRC32, SHA2, SHA1, AES (2 with PMULL), SEVL. */
		return field(flag(features.has(Feature::fcma)), 28) |
			field(flag(features.has(Feature::rdm)), 24) |
			field(flag(features.has(Feature::crc32)), 16) |
			field(flag(features.has(Feature::sha2)), 12) |
			field(flag(features.has(Feature::sha1)), 8) |
			field(features.has(Feature::aes) ? (features.has(Feature::pmull) ? 2 : 1) : 0, 4) |
			field(1, 0);
	case A32IdRegister::isar6:
		/* I8MM, BF16, SB, FHM, DP and JSCVT; SPECRES stays 0. */
		return field(flag(features.has(Feature::i8mm)), 24) |
			field(flag(features.has(Feature::bf16)), 20) |
			field(flag(features.has(Feature::sb)), 12) |
			field(flag(features.has(Feature::fhm)), 8) |
			field(flag(features.has(Feature::dotprod)), 4) |
			field(flag(features.has(Feature::jscvt)), 0);
	case A32IdRegister::mvfr0:
		/* FPRound, FPSqrt, FPDivide, FPDP 2 and FPSP 2 (VFPv3 and later)
		 * and SIMDReg 2 (32 doubleword registers); FPShVec 0, since
		 * short vectors are RAZ/WI, and FPTrap 0, since the trap enables
		 * are. */
		return field(1, 28) | field(1, 20) | field(1, 16) |
			field(2, 8) | field(2, 4) | field(2, 0);
	case A32IdRegister::mvfr1:
		/* FPFtZ, FPDNaN, SIMDLS, SIMDInt, SIMDSP, SIMDFMAC (VFPv4), and
		 * the half-precision fields: FPHP 1 for the single-precision
		 * conversions, 2 with the double-precision ones, 3 with FP16
		 * arithmetic; SIMDHP 1 for the conversions, 2 with arithmetic. */
		return field(1, 28) | field(features.has(Feature::fp16) ? 3 : 2, 24) |
			field(features.has(Feature::fp16) ? 2 : 1, 20) | field(1, 16) |
			field(1, 12) | field(1, 8) | field(1, 4) | field(1, 0);
	case A32IdRegister::mvfr2:
		/* FPMisc 4 (VSEL, VMAXNM, VMINNM, VRINT, VCVT with a rounding
		 * mode) and SIMDMisc 3 for their Advanced SIMD forms. */
		return field(4, 4) | field(3, 0);
	}
	return 0;
}

/* A bit of AT_HWCAP or AT_HWCAP2 of a 32-bit task, and the field it is read
 * from; HwcapField's, for the AArch32 identification registers. */
struct A32HwcapField final {
	A32IdRegister id_register;
	unsigned shift;
	unsigned minimum;
	unsigned hwcap;
};

/* arm64 Linux's compat_elf_hwcaps (arch/arm64/kernel/cpufeature.c), bit
 * numbers as COMPAT_HWCAP_* with COMPAT_HWCAP2_* as 32 + n, less the SSBS bit
 * the profile has no register for and NEON, which is three fields at once
 * (compat_has_neon, below). */
inline constexpr A32HwcapField a32_hwcap_fields[]{
	{A32IdRegister::mvfr1, 28, 1, 16},	/* SIMDFMAC: VFPv4 */
	{A32IdRegister::mvfr0, 8, 2, 6},	/* FPDP: VFP */
	{A32IdRegister::mvfr0, 8, 2, 13},	/* FPDP: VFPv3 */
	{A32IdRegister::mvfr1, 24, 3, 22},	/* FPHP: FPHP */
	{A32IdRegister::mvfr1, 20, 2, 23},	/* SIMDHP: ASIMDHP */
	{A32IdRegister::isar5, 4, 2, 32 + 1},	/* AES: PMULL */
	{A32IdRegister::isar5, 4, 1, 32 + 0},	/* AES */
	{A32IdRegister::isar5, 8, 1, 32 + 2},	/* SHA1 */
	{A32IdRegister::isar5, 12, 1, 32 + 3},	/* SHA2 */
	{A32IdRegister::isar5, 16, 1, 32 + 4},	/* CRC32 */
	{A32IdRegister::isar6, 4, 1, 24},	/* DP: ASIMDDP */
	{A32IdRegister::isar6, 8, 1, 25},	/* FHM: ASIMDFHM */
	{A32IdRegister::isar6, 12, 1, 32 + 5},	/* SB */
	{A32IdRegister::isar6, 20, 1, 26},	/* BF16: ASIMDBF16 */
	{A32IdRegister::isar6, 24, 1, 27},	/* I8MM */
};

/* COMPAT_ELF_HWCAP_DEFAULT: HALF, THUMB, FAST_MULT, EDSP, TLS, IDIVA, IDIVT
 * and LPAE, which arm64 Linux sets for every 32-bit task; and EVTSTRM, the
 * promise that WFE wakes once per event-stream period, which holds because
 * both engines complete WFE at once. The profile has no SWP, so no
 * COMPAT_HWCAP_SWP, and arm64 Linux never sets VFPD32. */
inline constexpr unsigned long a32_default_hwcap = (1UL << 1) | (1UL << 2) |
	(1UL << 4) | (1UL << 7) | (1UL << 15) | (1UL << 17) | (1UL << 18) |
	(1UL << 20) | (1UL << 21);
inline constexpr unsigned long a32_hwcap_neon = 1UL << 12;

/* AT_HWCAP and AT_HWCAP2 for a 32-bit task, which the kernel returns through
 * `darwin_uml_guest_compat_hwcap()` and `..._hwcap2()`. Each word is read from
 * the AArch32 identification registers exactly as arm64 Linux reads it. */
[[nodiscard]] inline std::array<unsigned long, 2> guest_compat_hwcaps(
	const HostCapabilities &host) noexcept
{
	std::array<unsigned long, 2> words{a32_default_hwcap, 0};
	for (const auto &bit : a32_hwcap_fields)
		if (((a32_id_register_value(host, bit.id_register) >>
		      bit.shift) & 15U) >= bit.minimum)
			words[bit.hwcap / 32U] |= 1UL << (bit.hwcap % 32U);
	/* compat_has_neon(): MVFR1.SIMDSP, SIMDInt and SIMDLS all nonzero. */
	const auto mvfr1 = a32_id_register_value(host, A32IdRegister::mvfr1);
	if (((mvfr1 >> 16) & 15U) != 0U && ((mvfr1 >> 12) & 15U) != 0U &&
	    ((mvfr1 >> 8) & 15U) != 0U)
		words[0] |= a32_hwcap_neon;
	return words;
}

} // namespace uml::cpu::arm64::detail
