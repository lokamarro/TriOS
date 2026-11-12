// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

#include "sme_encodings.hpp"

namespace uml::cpu::arm64::detail {

/*
 * Decoding against the generated SVE/SME encoding table. Both engines
 * classify a word here, so they agree by construction:
 *
 * - a word in the SVE or SME encoding space decodes to the row of
 *   sme_encodings that matches it, if any;
 * - the virtual CPU implements that row when the CPU has the row's
 *   streaming-mode requirement and at least its minimum streaming vector
 *   length; any other word of the space is UNDEFINED;
 * - an implemented row executes only in the PSTATE.SM and PSTATE.ZA state
 *   its access checks demand, and traps otherwise;
 * - an FP&SIMD word is AdvSIMD, which traps while PSTATE.SM is 1, or FP.
 *
 * The features are the profile's, as SmeFeatureMask bits; svl_bytes is the
 * streaming vector length, which nothing here assumes. A row's
 * non-streaming requirement, which only an SVE profile decides, has no
 * helper: no engine executes an SVE form outside streaming mode, apart from
 * the SM = 0 family (sm0_vector_length.hpp), which the engines decode
 * themselves.
 */

/* True for a word in the SVE (op0 x, op1 0010) or SME (op0 1, op1 0000) space. */
[[nodiscard]] constexpr bool sme_encoding_space(std::uint32_t word) noexcept
{
	return (word & 0x1e00'0000U) == 0x0400'0000U ||
		(word & 0x9e00'0000U) == 0x8000'0000U;
}

/* The row whose encoding the word matches, or nullptr for a word of no row. */
[[nodiscard]] constexpr const SmeEncoding *find_sme_encoding(
	std::uint32_t word) noexcept
{
	const std::uint32_t bucket = word >> sme_encoding_bucket_shift;
	for (std::uint32_t slot = sme_encoding_buckets[bucket];
	     slot < sme_encoding_buckets[bucket + 1U]; ++slot) {
		const SmeEncoding &row =
			sme_encodings[sme_encoding_bucket_rows[slot]];
		if ((word & row.mask) == row.value)
			return &row;
	}
	return nullptr;
}

/* Whether a CPU with these features and this SVL implements the row. */
[[nodiscard]] constexpr bool sme_encoding_implemented(const SmeEncoding &row,
	SmeFeatureMask features, std::uint32_t svl_bytes) noexcept
{
	return row.requirement != 0U &&
		(row.requirement & features) == row.requirement &&
		svl_bytes >= row.min_svl_bytes;
}

/* The implemented row a word decodes to, or nullptr: the word is UNDEFINED. */
[[nodiscard]] constexpr const SmeEncoding *decode_sme(std::uint32_t word,
	SmeFeatureMask features, std::uint32_t svl_bytes) noexcept
{
	const SmeEncoding *row = find_sme_encoding(word);
	if (row == nullptr || !sme_encoding_implemented(*row, features, svl_bytes))
		return nullptr;
	return row;
}

/*
 * Whether an implemented row executes with this PSTATE.SM and PSTATE.ZA;
 * otherwise it takes the SME (or, for an SVE row outside streaming mode,
 * the SVE) access trap, which Linux reports as SIGILL.
 */
[[nodiscard]] constexpr bool sme_mode_permits(
	const SmeEncoding &row, bool streaming, bool za) noexcept
{
	if ((row.mode & sme_mode_streaming) != 0U && !streaming)
		return false;
	return (row.mode & sme_mode_za) == 0U || za;
}

/* A row's operands, as a pointer into sme_encoding_operands and a count. */
[[nodiscard]] constexpr const SmeOperand *sme_encoding_operands_of(
	const SmeEncoding &row) noexcept
{
	return sme_encoding_operands.data() + row.operand_begin;
}

/* A row's memory shape, or nullptr for a row without memory access. */
[[nodiscard]] constexpr const SmeMemory *sme_encoding_memory_of(
	const SmeEncoding &row) noexcept
{
	if (row.memory == 0U)
		return nullptr;
	return &sme_encoding_memory[row.memory - 1U];
}

/* The element size of a memory form for this word, in bits. */
[[nodiscard]] constexpr std::uint32_t sme_memory_esize(
	const SmeMemory &memory, std::uint32_t word) noexcept
{
	if (memory.esize_shift.width == 0U)
		return memory.esize;
	return static_cast<std::uint32_t>(memory.esize)
		<< sme_segment_value(memory.esize_shift, word);
}

enum class FpSimdStreaming : std::uint8_t {
	unclassified,	// no row: the decode rejects the word
	fp,		// executes in streaming mode
	advsimd,	// traps while PSTATE.SM is 1
};

/*
 * The streaming-mode class of an FP&SIMD word. The table evaluates the
 * execute checks as on a CPU without FEAT_FPRCVT, which the virtual CPU
 * omits: with it, scalar FCVT*, SCVTF and UCVTF become FP.
 */
[[nodiscard]] constexpr FpSimdStreaming classify_fp_simd_streaming(
	std::uint32_t word) noexcept
{
	const std::uint32_t bucket = word >> sme_encoding_bucket_shift;
	for (std::uint32_t slot = fp_simd_streaming_buckets[bucket];
	     slot < fp_simd_streaming_buckets[bucket + 1U]; ++slot) {
		const FpSimdStreamingClass &row =
			fp_simd_streaming_classes[fp_simd_streaming_bucket_rows[slot]];
		if ((word & row.mask) == row.value)
			return row.advsimd ? FpSimdStreaming::advsimd
					   : FpSimdStreaming::fp;
	}
	return FpSimdStreaming::unclassified;
}

} // namespace uml::cpu::arm64::detail
