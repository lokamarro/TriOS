// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The generated SVE/SME encoding table (cpu/arm64/detail/sme_encodings.hpp)
 * and its decoder. Every row's LLVM MC witness must decode to that row and
 * every FP&SIMD row's witness to its own class. Each row's mode must be the
 * one its ASL access checks demand where its streaming-mode requirement
 * holds (FEAT_SME without FEAT_SVE or FEAT_SME_FA64), and its two
 * requirements must agree with its checks, per field value:
 * CheckSVEEnabled executes in both modes, CheckNonStreamingSVEEnabled only
 * outside streaming mode, the streaming checks only inside it, and the SME
 * checks in both under one requirement. Without an SVE feature, the
 * non-streaming requirement must decide exactly what the streaming
 * requirement and mode decide outside streaming mode, so an engine that
 * reads it changes nothing for a virtual CPU without SVE, which is every
 * one. A set of words assembled by LLVM MC must decode as the architecture
 * describes them. The generator's freshness test pins the table to the Arm
 * release; this test pins the decoder to the table.
 */
#include "cpu/arm64/detail/sme_decode.hpp"
#include "require.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

namespace detail = ::uml::cpu::arm64::detail;
using ::uml::testing::fail;
using ::uml::testing::require;

/* The SME profile's bits, and every bit including the SVE profile's. */
constexpr detail::SmeFeatureMask sme_features = detail::sme_feature_sme |
	detail::sme_feature_sme2 | detail::sme_feature_sme_f64f64 |
	detail::sme_feature_sme_i16i64 | detail::sme_feature_bf16 |
	detail::sme_feature_i8mm;
constexpr detail::SmeFeatureMask sve_features = detail::sme_feature_sve |
	detail::sme_feature_sve2 | detail::sme_feature_sve_aes |
	detail::sme_feature_sve_pmull128 | detail::sme_feature_sve_sha3 |
	detail::sme_feature_sve_bitperm | detail::sme_feature_sve_sm4;
constexpr detail::SmeFeatureMask all_features = sme_features | sve_features;
constexpr detail::SmeCheckMask all_checks = 0x7fU;
constexpr detail::SmeCheckMask streaming_checks =
	detail::sme_check_streaming_sve_enabled |
	detail::sme_check_streaming_sve_and_za_enabled;
constexpr detail::SmeCheckMask sme_checks =
	detail::sme_check_sme_and_za_enabled |
	detail::sme_check_sme_zt0_enabled | detail::sme_check_sme_enabled;
constexpr std::uint32_t largest_svl_bytes = 256;

constexpr bool has(detail::SmeFeatureMask mask, detail::SmeFeatureMask bits)
{
	return (mask & bits) == bits;
}

/* Whether a CPU with these features executes the row outside streaming
 * mode, as the non-streaming requirement says. */
constexpr bool non_streaming_implemented(const detail::SmeEncoding &row,
	detail::SmeFeatureMask features)
{
	return row.non_streaming_requirement != 0U &&
		has(features, row.non_streaming_requirement);
}

/*
 * What each access check demands of PSTATE on this profile, from the
 * shared pseudocode: without FEAT_SVE, CheckSVEEnabled is
 * CheckStreamingSVEEnabled, which needs PSTATE.SM; the ZA forms need
 * PSTATE.ZA; CheckSMEZT0Enabled needs PSTATE.ZA and names ZT0; and
 * CheckSMEEnabled needs neither. CheckNonStreamingSVEEnabled always traps
 * here, so no implemented row may call it.
 */
constexpr detail::SmeModeMask expected_mode(detail::SmeCheckMask checks)
{
	detail::SmeModeMask mode = 0;
	if ((checks & (detail::sme_check_sve_enabled |
		       detail::sme_check_streaming_sve_enabled |
		       detail::sme_check_streaming_sve_and_za_enabled)) != 0U)
		mode |= detail::sme_mode_streaming;
	if ((checks & (detail::sme_check_streaming_sve_and_za_enabled |
		       detail::sme_check_sme_and_za_enabled |
		       detail::sme_check_sme_zt0_enabled)) != 0U)
		mode |= detail::sme_mode_za;
	if ((checks & detail::sme_check_sme_zt0_enabled) != 0U)
		mode |= detail::sme_mode_zt0;
	return mode;
}

void check_index_shape()
{
	require(detail::sme_encoding_witnesses.size() ==
		detail::sme_encodings.size());
	require(detail::fp_simd_streaming_witnesses.size() ==
		detail::fp_simd_streaming_classes.size());
	require(detail::sme_encoding_buckets.size() ==
		(std::size_t{1} << (32U - detail::sme_encoding_bucket_shift)) + 1U);
	require(detail::sme_encoding_buckets.front() == 0U);
	require(detail::sme_encoding_buckets.back() ==
		detail::sme_encoding_bucket_rows.size());
	require(detail::fp_simd_streaming_buckets.size() ==
		detail::sme_encoding_buckets.size());
	require(detail::fp_simd_streaming_buckets.back() ==
		detail::fp_simd_streaming_bucket_rows.size());
	for (std::size_t index = 1; index < detail::sme_encoding_buckets.size();
	     ++index) {
		require(detail::sme_encoding_buckets[index - 1U] <=
			detail::sme_encoding_buckets[index]);
		require(detail::fp_simd_streaming_buckets[index - 1U] <=
			detail::fp_simd_streaming_buckets[index]);
	}
	for (const std::uint16_t row : detail::sme_encoding_bucket_rows)
		require(row < detail::sme_encodings.size());
	for (const std::uint16_t row : detail::fp_simd_streaming_bucket_rows)
		require(row < detail::fp_simd_streaming_classes.size());
}

/* Every bucket a row's words can reach lists the row. */
template<typename Rows, typename Buckets, typename Slots>
void check_buckets_cover(const Rows &rows, const Buckets &buckets,
	const Slots &slots)
{
	const unsigned shift = detail::sme_encoding_bucket_shift;
	const std::uint32_t width = 32U - shift;
	for (std::size_t index = 0; index < rows.size(); ++index) {
		const std::uint32_t fixed = rows[index].mask >> shift;
		const std::uint32_t base = rows[index].value >> shift;
		for (std::uint32_t key = 0; key < (1U << width); ++key) {
			if ((key & fixed) != base)
				continue;
			bool listed = false;
			for (std::uint32_t slot = buckets[key];
			     slot < buckets[key + 1U]; ++slot)
				listed = listed || slots[slot] == index;
			if (!listed) {
				std::fprintf(stderr, "row %zu missing from bucket %u\n",
					index, key);
				fail("bucket index incomplete");
			}
		}
	}
}

void check_operands(const detail::SmeEncoding &row, std::uint32_t witness)
{
	require(row.operand_begin + row.operand_count <=
		detail::sme_encoding_operands.size());
	const detail::SmeOperand *operands =
		detail::sme_encoding_operands_of(row);
	for (std::uint8_t index = 0; index < row.operand_count; ++index) {
		const detail::SmeOperand &operand = operands[index];
		require(operand.segment_count >= 1U &&
			operand.segment_count <= operand.segments.size());
		unsigned width = 0;
		bool field = false;
		for (std::uint8_t part = 0; part < operand.segment_count; ++part) {
			const detail::SmeSegment &segment = operand.segments[part];
			require(segment.width >= 1U);
			width += segment.width;
			field = field || segment.field;
			if (segment.field)
				require(segment.low + segment.width <= 32U);
			else
				require(segment.constant < (1U << segment.width));
		}
		require(field, "an operand names no field");
		/* Z, V and X registers number 0-31; P registers and ZA tiles
		 * (ZAQ0-ZAQ15 at most) 0-15. */
		unsigned limit = 5;
		if (operand.kind == detail::SmeOperandKind::p ||
		    operand.kind == detail::SmeOperandKind::za_tile)
			limit = 4;
		require(width <= limit, "operand number too wide");
		require(operand.count >= 1U && operand.count <= 4U);
		require(operand.stride == 1U || operand.stride == 4U ||
			operand.stride == 8U);
		require(detail::sme_operand_number(operand, witness) <
			(1U << limit));
	}
}

bool non_streaming_shape(detail::SmeMemoryShape shape)
{
	using Shape = detail::SmeMemoryShape;
	return shape == Shape::gather || shape == Shape::gather_first_fault ||
		shape == Shape::first_fault || shape == Shape::non_fault ||
		shape == Shape::gather_prefetch;
}

void check_memory(const detail::SmeEncoding &row, std::uint32_t witness)
{
	using Shape = detail::SmeMemoryShape;
	using Offset = detail::SmeAddressOffset;
	const detail::SmeMemory *memory = detail::sme_encoding_memory_of(row);
	if (memory == nullptr)
		return;
	require(row.memory <= detail::sme_encoding_memory.size());
	const detail::SmeOperand *operands =
		detail::sme_encoding_operands_of(row);
	require(memory->base_operand < row.operand_count);
	const bool gather = memory->shape == Shape::gather ||
		memory->shape == Shape::gather_first_fault ||
		memory->shape == Shape::gather_prefetch;
	const auto base = operands[memory->base_operand].kind;
	require(base == detail::SmeOperandKind::gpr_or_sp ||
		(gather && base == detail::SmeOperandKind::z),
		"base is not Xn|SP or a gather's Zn");
	const bool indexed = memory->offset == Offset::register_element ||
		memory->offset == Offset::register_byte ||
		memory->offset == Offset::vector_index;
	require(indexed == (memory->index_operand != detail::sme_no_operand));
	if (indexed) {
		require(memory->index_operand < row.operand_count);
		const auto kind = operands[memory->index_operand].kind;
		if (memory->offset == Offset::vector_index)
			require(kind == detail::SmeOperandKind::z && gather &&
				base == detail::SmeOperandKind::gpr_or_sp,
				"vector offsets need Zm and Xn|SP");
		else
			require(kind == detail::SmeOperandKind::gpr,
				"index is not Xm");
		if (memory->offset == Offset::register_byte)
			require(base == detail::SmeOperandKind::z,
				"an unscaled Xm offsets a vector base");
	}
	const bool immediate = memory->offset == Offset::immediate_footprint ||
		memory->offset == Offset::immediate_element ||
		memory->offset == Offset::immediate_16;
	require(immediate == (memory->immediate_segment_count != 0U));
	require((memory->shape == Shape::prefetch ||
		 memory->shape == Shape::gather_prefetch) ==
		(memory->direction == detail::SmeMemoryDirection::prefetch));
	if (memory->offset == Offset::vector_index) {
		require(memory->index_bits == 32U || memory->index_bits == 64U);
		require(memory->index_shift <= 3U);
		require(memory->index_sign.width == 0U ||
			(memory->index_bits == 32U && memory->index_sign.field &&
			 memory->index_sign.width == 1U));
	} else {
		require(memory->index_bits == 0U && memory->index_shift == 0U &&
			memory->index_sign.width == 0U);
	}
	const std::uint32_t esize = detail::sme_memory_esize(*memory, witness);
	require(esize >= 8U && esize <= 128U && (esize & (esize - 1U)) == 0U);
	require(memory->msize >= 8U && memory->msize <= 128U);
	require(memory->registers >= 1U && memory->registers <= 4U);
	const auto length = memory->shape == Shape::za_vector ?
		detail::SmeVectorLength::streaming :
		memory->shape == Shape::zt0 ? detail::SmeVectorLength::none :
					      detail::SmeVectorLength::current;
	require(memory->vector_length == length, "unexpected vector length");
	const bool za_state = memory->shape == Shape::za_slice ||
		memory->shape == Shape::za_vector || memory->shape == Shape::zt0;
	if (za_state)
		require((row.mode & detail::sme_mode_za) != 0U);
	else if (row.requirement != 0U)
		require((row.mode & detail::sme_mode_streaming) != 0U,
			"a vector access outside streaming mode");
	else
		require(has(row.non_streaming_requirement,
			detail::sme_feature_sve));
	if (non_streaming_shape(memory->shape))
		require(row.requirement == 0U &&
			row.non_streaming_requirement != 0U,
			"a non-streaming shape in streaming mode");
}

/*
 * The two requirements as the census classifies a row's checks. In
 * streaming mode the SVE features decide nothing; outside it,
 * CheckSVEEnabled and CheckNonStreamingSVEEnabled need FEAT_SVE, the
 * streaming checks trap, and the SME checks need what streaming mode needs.
 * A CheckSVEEnabled row keeps its extensions outside streaming mode, and
 * may need FEAT_SME there too where its decode does (SCLAMP: FEAT_SME or
 * FEAT_SVE2p1).
 */
void check_requirements(std::size_t index, const detail::SmeEncoding &row)
{
	const detail::SmeFeatureMask inside = row.requirement;
	const detail::SmeFeatureMask outside = row.non_streaming_requirement;
	bool ok = (inside & sve_features) == 0U &&
		(inside == 0U || has(inside, detail::sme_feature_sme));
	if ((row.checks & detail::sme_check_non_streaming_sve_enabled) != 0U) {
		ok = ok && inside == 0U && has(outside, detail::sme_feature_sve);
	} else if ((row.checks & streaming_checks) != 0U) {
		ok = ok && inside != 0U && outside == 0U;
	} else if ((row.checks & detail::sme_check_sve_enabled) != 0U) {
		const auto extensions = static_cast<detail::SmeFeatureMask>(
			inside & ~detail::sme_feature_sme);
		const auto allowed = static_cast<detail::SmeFeatureMask>(inside |
			detail::sme_feature_sve | detail::sme_feature_sve2);
		ok = ok && inside != 0U && has(outside, detail::sme_feature_sve) &&
			has(outside, extensions) && (outside & ~allowed) == 0U;
	} else {
		ok = ok && (row.checks & ~sme_checks) == 0U && inside != 0U &&
			outside == inside;
	}
	if (!ok) {
		std::fprintf(stderr, "row %zu checks %02x requirements %04x %04x\n",
			index, row.checks, inside, outside);
		fail("requirements disagree with the access checks");
	}
	/* Without the SVE profile, the non-streaming requirement decides what
	 * today's streaming requirement and mode decide outside streaming
	 * mode. */
	for (unsigned subset = 0; subset < 64U; ++subset) {
		detail::SmeFeatureMask features = 0;
		for (unsigned bit = 0; bit < 6U; ++bit)
			if (((subset >> bit) & 1U) != 0U)
				features = static_cast<detail::SmeFeatureMask>(
					features | (1U << bit));
		const bool today = detail::sme_encoding_implemented(row,
				features, largest_svl_bytes) &&
			(row.mode & detail::sme_mode_streaming) == 0U;
		if (non_streaming_implemented(row, features) != today) {
			std::fprintf(stderr, "row %zu features %04x\n", index,
				features);
			fail("the non-streaming requirement changes SME-only decode");
		}
	}
}

void check_rows()
{
	std::size_t streaming = 0;
	std::size_t non_streaming = 0;
	for (std::size_t index = 0; index < detail::sme_encodings.size();
	     ++index) {
		const detail::SmeEncoding &row = detail::sme_encodings[index];
		const std::uint32_t witness = detail::sme_encoding_witnesses[index];
		require((witness & row.mask) == row.value, "witness off its row");
		require(detail::sme_encoding_space(witness));
		if (detail::find_sme_encoding(witness) != &row) {
			std::fprintf(stderr, "row %zu witness %08x\n", index,
				witness);
			fail("a witness decodes to another row");
		}
		require((row.form & 0x7fffU) != 0U);
		require((row.checks & ~all_checks) == 0U);
		require((row.requirement & ~all_features) == 0U);
		require((row.non_streaming_requirement & ~all_features) == 0U);
		if (row.requirement == 0U && row.non_streaming_requirement == 0U) {
			require(row.checks == 0U && row.mode == 0U &&
				row.operand_count == 0U && row.memory == 0U &&
				row.min_svl_bytes == 0U, "an absent row has detail");
			require(detail::decode_sme(witness, all_features,
					largest_svl_bytes) == nullptr);
			continue;
		}
		require(row.checks != 0U, "an implemented row has no check");
		if (row.mode != expected_mode(row.checks)) {
			std::fprintf(stderr, "row %zu checks %02x mode %02x\n",
				index, row.checks, row.mode);
			fail("mode differs from the access checks");
		}
		check_requirements(index, row);
		if (row.non_streaming_requirement != 0U)
			++non_streaming;
		if (row.requirement == 0U) {
			/* Only the SVE profile, outside streaming mode, which
			 * no decode helper reads yet: VL is 16 bytes there. */
			require(row.min_svl_bytes == 0U);
			require(detail::decode_sme(witness, all_features,
					largest_svl_bytes) == nullptr);
			check_operands(row, witness);
			check_memory(row, witness);
			continue;
		}
		++streaming;
		if ((row.mode & detail::sme_mode_zt0) != 0U)
			require((row.requirement & detail::sme_feature_sme2) != 0U,
				"ZT0 without FEAT_SME2");
		require(row.min_svl_bytes == 0U || row.min_svl_bytes == 32U ||
			row.min_svl_bytes == 64U || row.min_svl_bytes == 128U ||
			row.min_svl_bytes == 256U);
		const std::uint32_t svl =
			row.min_svl_bytes == 0U ? 16U : row.min_svl_bytes;
		require(detail::decode_sme(witness, row.requirement, svl) == &row);
		require(detail::decode_sme(witness, all_features, svl) == &row);
		if (row.min_svl_bytes != 0U)
			require(detail::decode_sme(witness, all_features,
					row.min_svl_bytes / 2U) == nullptr);
		for (unsigned bit = 0; bit < 16U; ++bit) {
			const auto feature =
				static_cast<detail::SmeFeatureMask>(1U << bit);
			if ((row.requirement & feature) == 0U)
				continue;
			const auto others =
				static_cast<detail::SmeFeatureMask>(all_features & ~feature);
			require(detail::decode_sme(witness, others,
					largest_svl_bytes) == nullptr);
		}
		check_operands(row, witness);
		check_memory(row, witness);
	}
	std::printf("sme encoding table: %zu rows, %zu implemented in streaming "
		"mode, %zu outside it\n", detail::sme_encodings.size(), streaming,
		non_streaming);
}

void check_fp_simd_rows()
{
	std::size_t advsimd = 0;
	for (std::size_t index = 0;
	     index < detail::fp_simd_streaming_classes.size(); ++index) {
		const detail::FpSimdStreamingClass &row =
			detail::fp_simd_streaming_classes[index];
		const std::uint32_t witness =
			detail::fp_simd_streaming_witnesses[index];
		require((witness & row.mask) == row.value, "witness off its row");
		require(!detail::sme_encoding_space(witness));
		const std::uint32_t bucket =
			witness >> detail::sme_encoding_bucket_shift;
		std::size_t first = detail::fp_simd_streaming_classes.size();
		for (std::uint32_t slot = detail::fp_simd_streaming_buckets[bucket];
		     slot < detail::fp_simd_streaming_buckets[bucket + 1U]; ++slot) {
			const std::uint16_t candidate =
				detail::fp_simd_streaming_bucket_rows[slot];
			const detail::FpSimdStreamingClass &other =
				detail::fp_simd_streaming_classes[candidate];
			if ((witness & other.mask) == other.value) {
				first = candidate;
				break;
			}
		}
		if (first != index) {
			std::fprintf(stderr, "FP&SIMD row %zu witness %08x\n",
				index, witness);
			fail("an FP&SIMD witness decodes to another row");
		}
		require(detail::classify_fp_simd_streaming(witness) ==
			(row.advsimd ? detail::FpSimdStreaming::advsimd
				     : detail::FpSimdStreaming::fp));
		advsimd += row.advsimd ? 1U : 0U;
	}
	std::printf("FP&SIMD streaming classes: %zu rows, %zu AdvSIMD\n",
		detail::fp_simd_streaming_classes.size(), advsimd);
}

struct Expected final {
	const char *text;
	std::uint32_t word;
	detail::SmeFeatureMask requirement;
	detail::SmeFeatureMask non_streaming;
	detail::SmeModeMask mode;
};

namespace f {
constexpr detail::SmeFeatureMask sme = detail::sme_feature_sme;
constexpr detail::SmeFeatureMask sme2 = detail::sme_feature_sme2;
constexpr detail::SmeFeatureMask sve = detail::sme_feature_sve;
constexpr detail::SmeFeatureMask sve2 = detail::sme_feature_sve2;
} // namespace f

/* Words LLVM MC 22.1.8 assembles, with what the architecture says of them. */
constexpr Expected expected_words[] = {
	{"add z0.s, z1.s, z2.s", 0x04a2'0020U, f::sme, f::sve,
		detail::sme_mode_streaming},
	{"addvl x0, x0, #1", 0x0420'5020U, f::sme, f::sve,
		detail::sme_mode_streaming},
	{"addsvl x0, x0, #1", 0x0420'5820U, f::sme, f::sme, 0},
	{"rdsvl x0, #1", 0x04bf'5820U, f::sme, f::sme, 0},
	{"fmopa za0.s, p0/m, p1/m, z0.s, z1.s", 0x8081'2000U, f::sme, 0,
		detail::sme_mode_streaming | detail::sme_mode_za},
	{"fmopa za0.d, p0/m, p1/m, z0.d, z1.d", 0x80c1'2000U,
		f::sme | detail::sme_feature_sme_f64f64, 0,
		detail::sme_mode_streaming | detail::sme_mode_za},
	{"smopa za0.d, p0/m, p1/m, z0.h, z1.h", 0xa0c1'2000U,
		f::sme | detail::sme_feature_sme_i16i64, 0,
		detail::sme_mode_streaming | detail::sme_mode_za},
	{"ldr za[w12, 0], [x0]", 0xe100'0000U, f::sme, f::sme,
		detail::sme_mode_za},
	{"zero {za}", 0xc008'00ffU, f::sme, f::sme, detail::sme_mode_za},
	{"ldr zt0, [x0]", 0xe11f'8000U, f::sme | f::sme2, f::sme | f::sme2,
		detail::sme_mode_za | detail::sme_mode_zt0},
	{"ld1w {z0.s, z8.s}, pn8/z, [x0]", 0xa140'4000U, f::sme | f::sme2, 0,
		detail::sme_mode_streaming},
	/* SVE2 forms: FEAT_SVE2 or FEAT_SME. */
	{"pmullb z0.h, z1.b, z2.b", 0x4542'6820U, f::sme, f::sve | f::sve2,
		detail::sme_mode_streaming},
	{"usdot z0.s, z1.b, z2.b", 0x4482'7820U,
		f::sme | detail::sme_feature_i8mm,
		f::sve | detail::sme_feature_i8mm, detail::sme_mode_streaming},
	/* FEAT_SME or FEAT_SVE2p1 behind a bare CheckSVEEnabled: outside
	 * streaming mode a CPU with FEAT_SVE executes it through FEAT_SME. */
	{"sclamp z0.s, z1.s, z2.s", 0x4482'c020U, f::sme, f::sme | f::sve,
		detail::sme_mode_streaming},
	{"whilege {p0.s, p1.s}, x0, x1", 0x25a1'5010U, f::sme | f::sme2,
		f::sme | f::sme2 | f::sve, detail::sme_mode_streaming},
	/* Only outside streaming mode. */
	{"ld1d {z0.d}, p0/z, [x0, z1.d, lsl #3]", 0xc5e1'c000U, 0, f::sve, 0},
	{"compact z0.s, p0, z1.s", 0x05a1'8020U, 0, f::sve, 0},
	{"rdffr p0.b", 0x2519'f000U, 0, f::sve, 0},
	{"fadda d0, p0, d0, z1.d", 0x65d8'2020U, 0, f::sve, 0},
	{"histcnt z0.s, p0/z, z1.s, z2.s", 0x45a2'c020U, 0, f::sve | f::sve2, 0},
	{"pmullb z0.q, z1.d, z2.d", 0x4502'6820U, 0,
		f::sve | detail::sme_feature_sve_pmull128, 0},
	{"aese z0.b, z0.b, z1.b", 0x4522'e020U, 0,
		f::sve | detail::sme_feature_sve_aes, 0},
	{"rax1 z0.d, z1.d, z2.d", 0x4522'f420U, 0,
		f::sve | detail::sme_feature_sve_sha3, 0},
	{"bfmmla z0.s, z1.h, z2.h", 0x6462'e420U, 0,
		f::sve | detail::sme_feature_bf16, 0},
	/* Withheld: FEAT_SVE_BitPerm and FEAT_SVE_SM4. */
	{"bdep z0.s, z1.s, z2.s", 0x4582'b420U, 0, 0, 0},
	{"sm4e z0.s, z0.s, z1.s", 0x4523'e020U, 0, 0, 0},
};

void check_expected_words()
{
	for (const Expected &expected : expected_words) {
		const detail::SmeEncoding *row =
			detail::find_sme_encoding(expected.word);
		if (row == nullptr || row->requirement != expected.requirement ||
		    row->non_streaming_requirement != expected.non_streaming ||
		    row->mode != expected.mode) {
			std::fprintf(stderr, "%s\n", expected.text);
			fail("a known word decodes unexpectedly");
		}
	}

	/* An SVE gather: in the space and the table, never implemented in
	 * streaming mode. */
	require(detail::decode_sme(0xc5e1'c000U, all_features,
			largest_svl_bytes) == nullptr);
	/* SMSTART is a Base MSR (immediate) form, outside the space. */
	require(!detail::sme_encoding_space(0xd503'477fU));

	/* LD1B {z0.b}, p0/z, [x0, #1, mul vl]: contiguous, one vector of bytes. */
	const detail::SmeEncoding *ld1b = detail::find_sme_encoding(0xa401'a000U);
	require(ld1b != nullptr);
	const detail::SmeMemory *memory = detail::sme_encoding_memory_of(*ld1b);
	require(memory != nullptr &&
		memory->shape == detail::SmeMemoryShape::contiguous &&
		memory->direction == detail::SmeMemoryDirection::load &&
		memory->offset == detail::SmeAddressOffset::immediate_footprint &&
		detail::sme_memory_esize(*memory, 0xa401'a000U) == 8U &&
		memory->msize == 8U && memory->registers == 1U &&
		memory->immediate_signed && memory->immediate_segment_count == 1U &&
		detail::sme_segment_value(memory->immediate[0], 0xa401'a000U) == 1U);

	/* ST1W {z0.s}, p0, [x0, x1, lsl #2]: the element size is a field. */
	const detail::SmeEncoding *st1w = detail::find_sme_encoding(0xe541'4000U);
	require(st1w != nullptr);
	memory = detail::sme_encoding_memory_of(*st1w);
	require(memory != nullptr &&
		memory->direction == detail::SmeMemoryDirection::store &&
		memory->offset == detail::SmeAddressOffset::register_element &&
		memory->esize_shift.width == 1U &&
		detail::sme_memory_esize(*memory, 0xe541'4000U) == 32U &&
		detail::sme_memory_esize(*memory, 0xe561'4000U) == 64U &&
		memory->msize == 32U);
	const detail::SmeOperand *operands =
		detail::sme_encoding_operands_of(*st1w);
	require(detail::sme_operand_number(operands[memory->index_operand],
			0xe541'4000U) == 1U);

	/* LD1W {z0.s, z8.s}: a strided pair. */
	const detail::SmeEncoding *strided =
		detail::find_sme_encoding(0xa140'4000U);
	require(strided != nullptr);
	memory = detail::sme_encoding_memory_of(*strided);
	require(memory != nullptr &&
		memory->shape == detail::SmeMemoryShape::strided &&
		memory->registers == 2U && memory->stride == 8U);
	bool pair = false;
	operands = detail::sme_encoding_operands_of(*strided);
	for (std::uint8_t index = 0; index < strided->operand_count; ++index)
		pair = pair || (operands[index].kind == detail::SmeOperandKind::z &&
			operands[index].count == 2U && operands[index].stride == 8U);
	require(pair, "the strided pair has no two-register operand");

	/* LDR ZT0 and PRFB. */
	memory = detail::sme_encoding_memory_of(
		*detail::find_sme_encoding(0xe11f'8000U));
	require(memory != nullptr && memory->shape == detail::SmeMemoryShape::zt0);
	memory = detail::sme_encoding_memory_of(
		*detail::find_sme_encoding(0x85c0'0000U));
	require(memory != nullptr &&
		memory->shape == detail::SmeMemoryShape::prefetch);

	/* The vector length each shape follows. */
	require(detail::sme_encoding_memory_of(*ld1b)->vector_length ==
		detail::SmeVectorLength::current);
	require(detail::sme_encoding_memory_of(*detail::find_sme_encoding(
			0xe100'0000U))->vector_length ==
		detail::SmeVectorLength::streaming);
	require(detail::sme_encoding_memory_of(*detail::find_sme_encoding(
			0xe11f'8000U))->vector_length ==
		detail::SmeVectorLength::none);

	struct Shape final {
		const char *text;
		std::uint32_t word;
		detail::SmeMemoryShape shape;
		detail::SmeAddressOffset offset;
		detail::SmeOperandKind base;
		std::uint8_t msize;
		std::uint8_t index_bits;
		std::uint8_t index_shift;
		std::uint32_t index_sign;
	};
	using S = detail::SmeMemoryShape;
	using O = detail::SmeAddressOffset;
	using K = detail::SmeOperandKind;
	constexpr Shape shapes[] = {
		{"ld1d {z0.d}, p0/z, [x0, z1.d, lsl #3]", 0xc5e1'c000U,
			S::gather, O::vector_index, K::gpr_or_sp, 64, 64, 3, 0},
		{"ld1w {z0.s}, p0/z, [x0, z1.s, sxtw #2]", 0x8561'4000U,
			S::gather, O::vector_index, K::gpr_or_sp, 32, 32, 2, 1},
		{"ld1w {z0.d}, p0/z, [x0, z1.d, uxtw]", 0xc501'4000U,
			S::gather, O::vector_index, K::gpr_or_sp, 32, 32, 0, 0},
		{"st1d {z0.d}, p0, [x0, z1.d, lsl #3]", 0xe5a1'a000U,
			S::gather, O::vector_index, K::gpr_or_sp, 64, 64, 3, 0},
		{"ld1d {z0.d}, p0/z, [z1.d, #8]", 0xc5a1'c020U,
			S::gather, O::immediate_element, K::z, 64, 0, 0, 0},
		{"ldnt1d {z0.d}, p0/z, [z1.d, x2]", 0xc582'c020U,
			S::gather, O::register_byte, K::z, 64, 0, 0, 0},
		{"ldff1w {z0.s}, p0/z, [z1.s, #4]", 0x8521'e020U,
			S::gather_first_fault, O::immediate_element, K::z, 32, 0, 0, 0},
		{"ldff1d {z0.d}, p0/z, [x0, x1, lsl #3]", 0xa5e1'6000U,
			S::first_fault, O::register_element, K::gpr_or_sp, 64, 0, 0, 0},
		{"ldnf1d {z0.d}, p0/z, [x0, #1, mul vl]", 0xa5f1'a000U,
			S::non_fault, O::immediate_footprint, K::gpr_or_sp, 64, 0, 0, 0},
		{"prfh pldl1keep, p0, [x0, z1.d, lsl #1]", 0xc461'a000U,
			S::gather_prefetch, O::vector_index, K::gpr_or_sp, 16, 64, 1, 0},
		{"prfw pldl1keep, p0, [z1.s, #8]", 0x8502'e020U,
			S::gather_prefetch, O::immediate_element, K::z, 32, 0, 0, 0},
	};
	for (const Shape &item : shapes) {
		const detail::SmeEncoding *row = detail::find_sme_encoding(item.word);
		memory = row == nullptr ? nullptr :
					  detail::sme_encoding_memory_of(*row);
		if (memory == nullptr || memory->shape != item.shape ||
		    memory->offset != item.offset ||
		    detail::sme_encoding_operands_of(*row)[
			    memory->base_operand].kind != item.base ||
		    memory->msize != item.msize ||
		    memory->index_bits != item.index_bits ||
		    memory->index_shift != item.index_shift ||
		    detail::sme_segment_value(memory->index_sign, item.word) !=
			    item.index_sign ||
		    memory->vector_length != detail::SmeVectorLength::current) {
			std::fprintf(stderr, "%s\n", item.text);
			fail("a non-streaming memory form has the wrong shape");
		}
	}

	/* UZP { z0.q-z3.q }: allocated only with an SVL of 512 bits or more. */
	require(detail::decode_sme(0xc137'e082U, all_features, 64U) != nullptr);
	require(detail::decode_sme(0xc137'e082U, all_features, 32U) == nullptr);
	require(detail::sme_mode_permits(
		*detail::find_sme_encoding(0xc137'e082U), true, false));
	require(!detail::sme_mode_permits(
		*detail::find_sme_encoding(0xc137'e082U), false, true));
	require(!detail::sme_mode_permits(
		*detail::find_sme_encoding(0xe11f'8000U), true, false));

	struct Class final {
		const char *text;
		std::uint32_t word;
		detail::FpSimdStreaming expected;
	};
	constexpr Class classes[] = {
		{"fadd s0, s1, s2", 0x1e22'2820U, detail::FpSimdStreaming::fp},
		{"add v0.4s, v1.4s, v2.4s", 0x4ea2'8420U,
			detail::FpSimdStreaming::advsimd},
		{"ldr q0, [x0]", 0x3dc0'0000U, detail::FpSimdStreaming::fp},
		{"ld1 {v0.16b}, [x0]", 0x4c40'7000U,
			detail::FpSimdStreaming::advsimd},
		{"frecpe s0, s1", 0x5ea1'd820U, detail::FpSimdStreaming::fp},
		{"frecpe v0.4s, v1.4s", 0x4ea1'd820U,
			detail::FpSimdStreaming::advsimd},
		{"umov w0, v1.s[0]", 0x0e04'3c20U, detail::FpSimdStreaming::fp},
		{"umov w0, v1.s[1]", 0x0e0c'3c20U,
			detail::FpSimdStreaming::advsimd},
		{"fmov x0, d1", 0x9e66'0020U, detail::FpSimdStreaming::fp},
		{"fcvtzs s0, s1", 0x5ea1'b820U, detail::FpSimdStreaming::advsimd},
		{"fcvtzs w0, s1", 0x1e38'0020U, detail::FpSimdStreaming::fp},
	};
	for (const Class &item : classes) {
		if (detail::classify_fp_simd_streaming(item.word) != item.expected) {
			std::fprintf(stderr, "%s\n", item.text);
			fail("a known FP&SIMD word has the wrong class");
		}
	}
	/* A Base word has no FP&SIMD class. */
	require(detail::classify_fp_simd_streaming(0x8b02'0020U) ==
		detail::FpSimdStreaming::unclassified);
}

} // namespace

int main()
{
	check_index_shape();
	check_buckets_cover(detail::sme_encodings, detail::sme_encoding_buckets,
		detail::sme_encoding_bucket_rows);
	check_buckets_cover(detail::fp_simd_streaming_classes,
		detail::fp_simd_streaming_buckets,
		detail::fp_simd_streaming_bucket_rows);
	check_rows();
	check_fp_simd_rows();
	check_expected_words();
	return 0;
}
