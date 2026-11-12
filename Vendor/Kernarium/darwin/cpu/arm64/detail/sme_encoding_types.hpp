// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace uml::cpu::arm64::detail {

/*
 * The row types of the generated SVE/SME encoding table
 * (sme_encodings.hpp). Nothing here assumes a vector length: a row that
 * depends on one says so through its minimum SVL or its memory shape, and a
 * consumer reads the length at run time.
 */

/*
 * The profile features a row can need, one bit each. The names are the
 * runtime's ArmFeature tokens; a consumer maps its feature set onto these
 * bits. The withheld extensions FEAT_SVE_BitPerm and FEAT_SVE_SM4 have
 * bits that no row needs until the generator's profile includes them.
 */
using SmeFeatureMask = std::uint16_t;
inline constexpr SmeFeatureMask sme_feature_sme = 1U << 0U;
inline constexpr SmeFeatureMask sme_feature_sme2 = 1U << 1U;
inline constexpr SmeFeatureMask sme_feature_sme_f64f64 = 1U << 2U;
inline constexpr SmeFeatureMask sme_feature_sme_i16i64 = 1U << 3U;
inline constexpr SmeFeatureMask sme_feature_bf16 = 1U << 4U;
inline constexpr SmeFeatureMask sme_feature_i8mm = 1U << 5U;
inline constexpr SmeFeatureMask sme_feature_sve = 1U << 6U;
inline constexpr SmeFeatureMask sme_feature_sve2 = 1U << 7U;
inline constexpr SmeFeatureMask sme_feature_sve_aes = 1U << 8U;
inline constexpr SmeFeatureMask sme_feature_sve_pmull128 = 1U << 9U;
inline constexpr SmeFeatureMask sme_feature_sve_sha3 = 1U << 10U;
inline constexpr SmeFeatureMask sme_feature_sve_bitperm = 1U << 11U;
inline constexpr SmeFeatureMask sme_feature_sve_sm4 = 1U << 12U;

/* The access checks an execute section calls, one bit each. */
using SmeCheckMask = std::uint8_t;
inline constexpr SmeCheckMask sme_check_sve_enabled = 1U << 0U;
inline constexpr SmeCheckMask sme_check_streaming_sve_enabled = 1U << 1U;
inline constexpr SmeCheckMask sme_check_non_streaming_sve_enabled = 1U << 2U;
inline constexpr SmeCheckMask sme_check_streaming_sve_and_za_enabled = 1U << 3U;
inline constexpr SmeCheckMask sme_check_sme_and_za_enabled = 1U << 4U;
inline constexpr SmeCheckMask sme_check_sme_zt0_enabled = 1U << 5U;
inline constexpr SmeCheckMask sme_check_sme_enabled = 1U << 6U;

/*
 * What those checks demand where a row's streaming-mode requirement holds,
 * on a CPU with FEAT_SME and without FEAT_SVE or FEAT_SME_FA64: PSTATE.SM
 * must be 1, PSTATE.ZA must be 1, and the instruction accesses ZT0. A row
 * with none of them executes in either mode under that one requirement; a
 * row that needs PSTATE.SM executes outside streaming mode only through its
 * non-streaming requirement, which names FEAT_SVE. PSTATE.ZA applies in
 * both modes.
 */
using SmeModeMask = std::uint8_t;
inline constexpr SmeModeMask sme_mode_streaming = 1U << 0U;
inline constexpr SmeModeMask sme_mode_za = 1U << 1U;
inline constexpr SmeModeMask sme_mode_zt0 = 1U << 2U;

/*
 * Part of a register number or an immediate, most significant first: a
 * field of the instruction word, or a constant.
 */
struct SmeSegment final {
	std::uint8_t low;
	std::uint8_t width;
	std::uint8_t constant;
	bool field;
};

[[nodiscard]] constexpr SmeSegment sme_field(
	std::uint8_t low, std::uint8_t width) noexcept
{
	return SmeSegment{low, width, 0, true};
}

[[nodiscard]] constexpr SmeSegment sme_constant(
	std::uint8_t width, std::uint8_t value) noexcept
{
	return SmeSegment{0, width, value, false};
}

[[nodiscard]] constexpr std::uint32_t sme_segment_value(
	const SmeSegment &segment, std::uint32_t word) noexcept
{
	if (!segment.field)
		return segment.constant;
	return (word >> segment.low) & ((1U << segment.width) - 1U);
}

enum class SmeOperandKind : std::uint8_t {
	z,
	p,
	v,
	gpr,
	gpr_or_sp,
	za_tile,
};

/*
 * A register operand: the register, or the first register of a group of
 * `count` registers `stride` apart, numbered by concatenating its segments.
 * A group's alignment is in its segments (Zn:'0' names an even register).
 */
struct SmeOperand final {
	SmeOperandKind kind;
	std::uint8_t count;
	std::uint8_t stride;
	std::uint8_t segment_count;
	std::array<SmeSegment, 4> segments;
};

[[nodiscard]] constexpr std::uint32_t sme_operand_number(
	const SmeOperand &operand, std::uint32_t word) noexcept
{
	std::uint32_t number = 0;
	for (std::uint8_t index = 0; index < operand.segment_count; ++index) {
		const SmeSegment &segment = operand.segments[index];
		number = (number << segment.width) |
			sme_segment_value(segment, word);
	}
	return number;
}

enum class SmeMemoryShape : std::uint8_t {
	contiguous,	// LD1*, ST1*, LDNT1*, STNT1*, one or a consecutive group
	strided,	// the SME2 strided multi-vector LD1*/ST1* forms
	structure,	// LD2-LD4, ST2-ST4
	replicate,	// LD1R*: one element to every element
	replicate_quad,	// LD1RQ*: one quadword to every quadword
	fill_z,		// LDR/STR (vector): SVL/8 bytes
	fill_p,		// LDR/STR (predicate): SVL/64 bytes
	za_slice,	// LD1[BHWDQ]/ST1[BHWDQ] (ZA tile slice)
	za_vector,	// LDR/STR (ZA array vector): SVL/8 bytes
	zt0,		// LDR/STR ZT0: 64 bytes
	prefetch,	// PRF[BHWD]: no access, no fault
	// The forms that execute only outside streaming mode:
	gather,		// LD1*, LDNT1*, ST1* and STNT1* with an address per element
	gather_first_fault,	// LDFF1* with an address per element
	first_fault,	// LDFF1* (scalar plus scalar): contiguous, first fault
	non_fault,	// LDNF1*: contiguous, no element faults
	gather_prefetch,	// PRF[BHWD] with an address per element
};

enum class SmeMemoryDirection : std::uint8_t {
	load,
	store,
	prefetch,
};

/*
 * What a form adds to its base, Xn|SP or, for a vector-base gather or
 * scatter (base operand of kind z), each active element of Zn
 * zero-extended from esize bits:
 * - immediate_footprint: the signed or unsigned immediate times the bytes
 *   the access spans, registers x (VL / esize) x msize / 8 (for fill_p
 *   VL/64, for fill_z and za_vector VL/8), "MUL VL";
 * - register_element: the index register times msize / 8;
 * - immediate_element: the immediate times msize / 8;
 * - immediate_16: the immediate times 16;
 * - vector_index: each active element of the index register Zm, its low
 *   index_bits bits zero- or sign-extended (index_sign) and shifted left by
 *   index_shift;
 * - register_byte: the index register Xm (XZR for 31), unscaled.
 * VL is the vector length the shape's SmeVectorLength names.
 */
enum class SmeAddressOffset : std::uint8_t {
	none,
	immediate_footprint,
	register_element,
	immediate_element,
	immediate_16,
	vector_index,
	register_byte,
};

/*
 * The vector length that sizes a memory form: CurrentVL(), which is VL (16
 * bytes for the virtual CPU) outside streaming mode and SVL in it;
 * CurrentSVL(), SVL in either mode (the ZA array vectors); or none (ZT0).
 */
enum class SmeVectorLength : std::uint8_t {
	current,
	streaming,
	none,
};

inline constexpr std::uint8_t sme_no_operand = 0xffU;

/*
 * A memory form's shape. Sizes are in bits. A form whose element size is
 * an instruction field (ST1B's 8 << UInt(size)) has esize_shift.width > 0:
 * its element size is esize << that field. base_operand and index_operand
 * index the row's operands. A gather prefetch's msize is the element its
 * offsets scale by (PRFH's 16 bits). index_bits, index_shift and index_sign
 * describe a vector_index offset and are 0 otherwise.
 */
struct SmeMemory final {
	SmeMemoryShape shape;
	SmeMemoryDirection direction;
	SmeAddressOffset offset;
	std::uint8_t esize;
	SmeSegment esize_shift;
	std::uint8_t msize;
	std::uint8_t registers;
	std::uint8_t stride;
	bool sign_extend;
	bool nontemporal;
	bool immediate_signed;
	std::uint8_t immediate_segment_count;
	std::array<SmeSegment, 2> immediate;
	std::uint8_t base_operand;
	std::uint8_t index_operand;
	SmeVectorLength vector_length;
	std::uint8_t index_bits;
	std::uint8_t index_shift;
	SmeSegment index_sign;
};

/*
 * A form's stable id: SVE-nnnn or SME-nnnn in the architecture oracle's
 * numbering (tests/tooling/arm64/a64_architecture_oracle.py).
 */
inline constexpr std::uint16_t sme_form_sme_section = 0x8000U;

/*
 * One row: the words that match mask/value. requirement is the profile
 * features the virtual CPU needs to implement the row in streaming mode
 * (PSTATE.SM = 1), and non_streaming_requirement those it needs to
 * implement it outside streaming mode; 0 means it never executes in that
 * mode (its features are outside the profile, or its checks trap there). A
 * row whose mode lacks sme_mode_streaming has the same requirement in both
 * modes. min_svl_bytes is the smallest streaming vector length at which the
 * row is allocated, 0 for any. operand_begin/operand_count select its
 * register operands in sme_encoding_operands; memory is 1 + its index in
 * sme_encoding_memory, or 0 for a form without memory access.
 */
struct SmeEncoding final {
	std::uint32_t mask;
	std::uint32_t value;
	std::uint16_t form;
	SmeFeatureMask requirement;
	SmeFeatureMask non_streaming_requirement;
	SmeCheckMask checks;
	SmeModeMask mode;
	std::uint8_t operand_count;
	std::uint16_t operand_begin;
	std::uint16_t memory;
	std::uint16_t min_svl_bytes;
};

/*
 * One FP&SIMD row: whether its words are AdvSIMD, which traps while
 * PSTATE.SM is 1 because the profile lacks FEAT_SME_FA64, or FP, which
 * executes in streaming mode. form is the FPSIMD-nnnn ordinal.
 */
struct FpSimdStreamingClass final {
	std::uint32_t mask;
	std::uint32_t value;
	std::uint16_t form;
	bool advsimd;
};

} // namespace uml::cpu::arm64::detail
