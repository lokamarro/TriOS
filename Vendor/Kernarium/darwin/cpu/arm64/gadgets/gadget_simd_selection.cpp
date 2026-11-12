// SPDX-License-Identifier: GPL-2.0-or-later
#include "gadget_selection_api.hpp"
#include "gadget_operand_encoding.hpp"

namespace uml::cpu::arm64::gadgets::selection {

[[nodiscard]] Gadget simd_extract_gadget(bool q, unsigned offset) noexcept
{
	static constexpr std::array<Gadget, 16> q_gadgets{
		uml_arm64_gadget_simd_extract_q0, uml_arm64_gadget_simd_extract_q1,
		uml_arm64_gadget_simd_extract_q2, uml_arm64_gadget_simd_extract_q3,
		uml_arm64_gadget_simd_extract_q4, uml_arm64_gadget_simd_extract_q5,
		uml_arm64_gadget_simd_extract_q6, uml_arm64_gadget_simd_extract_q7,
		uml_arm64_gadget_simd_extract_q8, uml_arm64_gadget_simd_extract_q9,
		uml_arm64_gadget_simd_extract_q10, uml_arm64_gadget_simd_extract_q11,
		uml_arm64_gadget_simd_extract_q12, uml_arm64_gadget_simd_extract_q13,
		uml_arm64_gadget_simd_extract_q14, uml_arm64_gadget_simd_extract_q15,
	};
	static constexpr std::array<Gadget, 8> d_gadgets{
		uml_arm64_gadget_simd_extract_d0, uml_arm64_gadget_simd_extract_d1,
		uml_arm64_gadget_simd_extract_d2, uml_arm64_gadget_simd_extract_d3,
		uml_arm64_gadget_simd_extract_d4, uml_arm64_gadget_simd_extract_d5,
		uml_arm64_gadget_simd_extract_d6, uml_arm64_gadget_simd_extract_d7,
	};
	return q ? q_gadgets[offset] : d_gadgets[offset];
}

#define UML_SIMD_THREE_SAME_V6(name) SimdThreeSameGadgets{ \
	uml_arm64_gadget_simd_three_same_##name##_8b, \
	uml_arm64_gadget_simd_three_same_##name##_4h, \
	uml_arm64_gadget_simd_three_same_##name##_2s, \
	uml_arm64_gadget_simd_three_same_##name##_16b, \
	uml_arm64_gadget_simd_three_same_##name##_8h, \
	uml_arm64_gadget_simd_three_same_##name##_4s, \
	nullptr, nullptr, nullptr, nullptr, nullptr}
#define UML_SIMD_THREE_SAME_V7(name) SimdThreeSameGadgets{ \
	uml_arm64_gadget_simd_three_same_##name##_8b, \
	uml_arm64_gadget_simd_three_same_##name##_4h, \
	uml_arm64_gadget_simd_three_same_##name##_2s, \
	uml_arm64_gadget_simd_three_same_##name##_16b, \
	uml_arm64_gadget_simd_three_same_##name##_8h, \
	uml_arm64_gadget_simd_three_same_##name##_4s, \
	uml_arm64_gadget_simd_three_same_##name##_2d, \
	nullptr, nullptr, nullptr, nullptr}
#define UML_SIMD_THREE_SAME_V7_S4(name) SimdThreeSameGadgets{ \
	uml_arm64_gadget_simd_three_same_##name##_8b, \
	uml_arm64_gadget_simd_three_same_##name##_4h, \
	uml_arm64_gadget_simd_three_same_##name##_2s, \
	uml_arm64_gadget_simd_three_same_##name##_16b, \
	uml_arm64_gadget_simd_three_same_##name##_8h, \
	uml_arm64_gadget_simd_three_same_##name##_4s, \
	uml_arm64_gadget_simd_three_same_##name##_2d, \
	uml_arm64_gadget_simd_three_same_##name##_b, \
	uml_arm64_gadget_simd_three_same_##name##_h, \
	uml_arm64_gadget_simd_three_same_##name##_s, \
	uml_arm64_gadget_simd_three_same_##name##_d}
#define UML_SIMD_THREE_SAME_V7_SD(name) SimdThreeSameGadgets{ \
	uml_arm64_gadget_simd_three_same_##name##_8b, \
	uml_arm64_gadget_simd_three_same_##name##_4h, \
	uml_arm64_gadget_simd_three_same_##name##_2s, \
	uml_arm64_gadget_simd_three_same_##name##_16b, \
	uml_arm64_gadget_simd_three_same_##name##_8h, \
	uml_arm64_gadget_simd_three_same_##name##_4s, \
	uml_arm64_gadget_simd_three_same_##name##_2d, \
	nullptr, nullptr, nullptr, \
	uml_arm64_gadget_simd_three_same_##name##_d}
#define UML_SIMD_THREE_SAME_VB(name) SimdThreeSameGadgets{ \
	uml_arm64_gadget_simd_three_same_##name##_8b, nullptr, nullptr, \
	uml_arm64_gadget_simd_three_same_##name##_16b, nullptr, nullptr, nullptr, \
	nullptr, nullptr, nullptr, nullptr}
#define UML_SIMD_THREE_SAME_HS(name) SimdThreeSameGadgets{ \
	nullptr, uml_arm64_gadget_simd_three_same_##name##_4h, \
	uml_arm64_gadget_simd_three_same_##name##_2s, nullptr, \
	uml_arm64_gadget_simd_three_same_##name##_8h, \
	uml_arm64_gadget_simd_three_same_##name##_4s, nullptr, nullptr, \
	uml_arm64_gadget_simd_three_same_##name##_h, \
	uml_arm64_gadget_simd_three_same_##name##_s, nullptr}

[[nodiscard]] Gadget simd_three_same_gadget(unsigned opcode,
	bool unsigned_group, bool scalar, bool q, unsigned size) noexcept
{
	if ((!scalar && !q && size == 3U) || size > 3U)
		return nullptr;
	const unsigned shape = scalar ? 7U + size : q ? 3U + size : size;

	static constexpr auto shadd = UML_SIMD_THREE_SAME_V6(shadd);
	static constexpr auto uhadd = UML_SIMD_THREE_SAME_V6(uhadd);
	static constexpr auto sqadd = UML_SIMD_THREE_SAME_V7_S4(sqadd);
	static constexpr auto uqadd = UML_SIMD_THREE_SAME_V7_S4(uqadd);
	static constexpr auto srhadd = UML_SIMD_THREE_SAME_V6(srhadd);
	static constexpr auto urhadd = UML_SIMD_THREE_SAME_V6(urhadd);
	static constexpr auto shsub = UML_SIMD_THREE_SAME_V6(shsub);
	static constexpr auto uhsub = UML_SIMD_THREE_SAME_V6(uhsub);
	static constexpr auto sqsub = UML_SIMD_THREE_SAME_V7_S4(sqsub);
	static constexpr auto uqsub = UML_SIMD_THREE_SAME_V7_S4(uqsub);
	static constexpr auto cmgt = UML_SIMD_THREE_SAME_V7_SD(cmgt);
	static constexpr auto cmhi = UML_SIMD_THREE_SAME_V7_SD(cmhi);
	static constexpr auto cmge = UML_SIMD_THREE_SAME_V7_SD(cmge);
	static constexpr auto cmhs = UML_SIMD_THREE_SAME_V7_SD(cmhs);
	static constexpr auto sshl = UML_SIMD_THREE_SAME_V7_SD(sshl);
	static constexpr auto ushl = UML_SIMD_THREE_SAME_V7_SD(ushl);
	static constexpr auto sqshl = UML_SIMD_THREE_SAME_V7_S4(sqshl);
	static constexpr auto uqshl = UML_SIMD_THREE_SAME_V7_S4(uqshl);
	static constexpr auto srshl = UML_SIMD_THREE_SAME_V7_SD(srshl);
	static constexpr auto urshl = UML_SIMD_THREE_SAME_V7_SD(urshl);
	static constexpr auto sqrshl = UML_SIMD_THREE_SAME_V7_S4(sqrshl);
	static constexpr auto uqrshl = UML_SIMD_THREE_SAME_V7_S4(uqrshl);
	static constexpr auto smax = UML_SIMD_THREE_SAME_V6(smax);
	static constexpr auto umax = UML_SIMD_THREE_SAME_V6(umax);
	static constexpr auto smin = UML_SIMD_THREE_SAME_V6(smin);
	static constexpr auto umin = UML_SIMD_THREE_SAME_V6(umin);
	static constexpr auto sabd = UML_SIMD_THREE_SAME_V6(sabd);
	static constexpr auto uabd = UML_SIMD_THREE_SAME_V6(uabd);
	static constexpr auto saba = UML_SIMD_THREE_SAME_V6(saba);
	static constexpr auto uaba = UML_SIMD_THREE_SAME_V6(uaba);
	static constexpr auto add = UML_SIMD_THREE_SAME_V7_SD(add);
	static constexpr auto sub = UML_SIMD_THREE_SAME_V7_SD(sub);
	static constexpr auto cmtst = UML_SIMD_THREE_SAME_V7_SD(cmtst);
	static constexpr auto cmeq = UML_SIMD_THREE_SAME_V7_SD(cmeq);
	static constexpr auto mla = UML_SIMD_THREE_SAME_V6(mla);
	static constexpr auto mls = UML_SIMD_THREE_SAME_V6(mls);
	static constexpr auto mul = UML_SIMD_THREE_SAME_V6(mul);
	static constexpr auto pmul = UML_SIMD_THREE_SAME_VB(pmul);
	static constexpr auto smaxp = UML_SIMD_THREE_SAME_V6(smaxp);
	static constexpr auto umaxp = UML_SIMD_THREE_SAME_V6(umaxp);
	static constexpr auto sminp = UML_SIMD_THREE_SAME_V6(sminp);
	static constexpr auto uminp = UML_SIMD_THREE_SAME_V6(uminp);
	static constexpr auto sqdmulh = UML_SIMD_THREE_SAME_HS(sqdmulh);
	static constexpr auto sqrdmulh = UML_SIMD_THREE_SAME_HS(sqrdmulh);
	static constexpr auto addp = UML_SIMD_THREE_SAME_V7(addp);

	switch (opcode) {
	case 0: return (unsigned_group ? uhadd : shadd)[shape];
	case 1: return (unsigned_group ? uqadd : sqadd)[shape];
	case 2: return (unsigned_group ? urhadd : srhadd)[shape];
	case 4: return (unsigned_group ? uhsub : shsub)[shape];
	case 5: return (unsigned_group ? uqsub : sqsub)[shape];
	case 6: return (unsigned_group ? cmhi : cmgt)[shape];
	case 7: return (unsigned_group ? cmhs : cmge)[shape];
	case 8: return (unsigned_group ? ushl : sshl)[shape];
	case 9: return (unsigned_group ? uqshl : sqshl)[shape];
	case 10: return (unsigned_group ? urshl : srshl)[shape];
	case 11: return (unsigned_group ? uqrshl : sqrshl)[shape];
	case 12: return (unsigned_group ? umax : smax)[shape];
	case 13: return (unsigned_group ? umin : smin)[shape];
	case 14: return (unsigned_group ? uabd : sabd)[shape];
	case 15: return (unsigned_group ? uaba : saba)[shape];
	case 16: return (unsigned_group ? sub : add)[shape];
	case 17: return (unsigned_group ? cmeq : cmtst)[shape];
	case 18: return (unsigned_group ? mls : mla)[shape];
	case 19: return (unsigned_group ? pmul : mul)[shape];
	case 20: return (unsigned_group ? umaxp : smaxp)[shape];
	case 21: return (unsigned_group ? uminp : sminp)[shape];
	case 22: return (unsigned_group ? sqrdmulh : sqdmulh)[shape];
	case 23: return unsigned_group ? nullptr : addp[shape];
	default: return nullptr;
	}
}

#undef UML_SIMD_THREE_SAME_HS
#undef UML_SIMD_THREE_SAME_VB
#undef UML_SIMD_THREE_SAME_V7_SD
#undef UML_SIMD_THREE_SAME_V7_S4
#undef UML_SIMD_THREE_SAME_V7
#undef UML_SIMD_THREE_SAME_V6

[[nodiscard]] Gadget simd_destination_resident_mla_gadget(
	bool q, unsigned size) noexcept
{
	if (size > 2U)
		return nullptr;
	static constexpr std::array<Gadget, 6> gadgets{
		uml_arm64_gadget_simd_destination_resident_mla_8b,
		uml_arm64_gadget_simd_destination_resident_mla_4h,
		uml_arm64_gadget_simd_destination_resident_mla_2s,
		uml_arm64_gadget_simd_destination_resident_mla_16b,
		uml_arm64_gadget_simd_destination_resident_mla_8h,
		uml_arm64_gadget_simd_destination_resident_mla_4s,
	};
	return gadgets[(q ? 3U : 0U) + size];
}

[[nodiscard]] Gadget simd_rdm_three_same_gadget(bool subtract, bool scalar,
	bool q, unsigned size) noexcept
{
	if (size != 1U && size != 2U)
		return nullptr;
#define UML_SIMD_RDM_GADGETS(name) std::array<Gadget, 6>{ \
	uml_arm64_gadget_simd_three_same_##name##_4h, \
	uml_arm64_gadget_simd_three_same_##name##_2s, \
	uml_arm64_gadget_simd_three_same_##name##_8h, \
	uml_arm64_gadget_simd_three_same_##name##_4s, \
	uml_arm64_gadget_simd_three_same_##name##_h, \
	uml_arm64_gadget_simd_three_same_##name##_s}
	static constexpr auto add = UML_SIMD_RDM_GADGETS(sqrdmlah);
	static constexpr auto sub = UML_SIMD_RDM_GADGETS(sqrdmlsh);
#undef UML_SIMD_RDM_GADGETS
	const unsigned shape = scalar ? size + 3U :
		(q ? size + 1U : size - 1U);
	return (subtract ? sub : add)[shape];
}

[[nodiscard]] Gadget simd_dot_vector_gadget(
	SimdDotSignedness signedness, bool q, bool force_software) noexcept
{
	static constexpr std::array<Gadget, 2> signed_signed{
		uml_arm64_gadget_simd_dot_vector_sdot_2s,
		uml_arm64_gadget_simd_dot_vector_sdot_4s};
	static constexpr std::array<Gadget, 2> signed_signed_software{
		uml_arm64_gadget_simd_dot_vector_sdot_2s_soft,
		uml_arm64_gadget_simd_dot_vector_sdot_4s_soft};
	static constexpr std::array<Gadget, 2> unsigned_unsigned{
		uml_arm64_gadget_simd_dot_vector_udot_2s,
		uml_arm64_gadget_simd_dot_vector_udot_4s};
	static constexpr std::array<Gadget, 2> unsigned_unsigned_software{
		uml_arm64_gadget_simd_dot_vector_udot_2s_soft,
		uml_arm64_gadget_simd_dot_vector_udot_4s_soft};
	static constexpr std::array<Gadget, 2> unsigned_signed_native{
		uml_arm64_gadget_simd_dot_vector_usdot_2s,
		uml_arm64_gadget_simd_dot_vector_usdot_4s};
	static constexpr std::array<Gadget, 2> unsigned_signed_software{
		uml_arm64_gadget_simd_dot_vector_usdot_2s_soft,
		uml_arm64_gadget_simd_dot_vector_usdot_4s_soft};
	switch (signedness) {
	case SimdDotSignedness::signed_signed:
		return (!force_software &&
			host_capabilities().has(ArmFeature::dotprod) ?
			signed_signed : signed_signed_software)[q];
	case SimdDotSignedness::unsigned_unsigned:
		return (!force_software &&
			host_capabilities().has(ArmFeature::dotprod) ?
			unsigned_unsigned : unsigned_unsigned_software)[q];
	case SimdDotSignedness::unsigned_signed:
		return (!force_software &&
			host_capabilities().has(ArmFeature::i8mm) ?
			unsigned_signed_native :
			unsigned_signed_software)[q];
	case SimdDotSignedness::signed_unsigned: return nullptr;
	}
	return nullptr;
}

#define UML_SIMD_DOT_INDEXED_ARRAY(name) std::array<Gadget, 8>{ \
	uml_arm64_gadget_simd_dot_indexed_##name##_2s_0, \
	uml_arm64_gadget_simd_dot_indexed_##name##_2s_1, \
	uml_arm64_gadget_simd_dot_indexed_##name##_2s_2, \
	uml_arm64_gadget_simd_dot_indexed_##name##_2s_3, \
	uml_arm64_gadget_simd_dot_indexed_##name##_4s_0, \
	uml_arm64_gadget_simd_dot_indexed_##name##_4s_1, \
	uml_arm64_gadget_simd_dot_indexed_##name##_4s_2, \
	uml_arm64_gadget_simd_dot_indexed_##name##_4s_3}
#define UML_SIMD_DOT_INDEXED_SOFT_ARRAY(name) std::array<Gadget, 8>{ \
	uml_arm64_gadget_simd_dot_indexed_##name##_2s_0_soft, \
	uml_arm64_gadget_simd_dot_indexed_##name##_2s_1_soft, \
	uml_arm64_gadget_simd_dot_indexed_##name##_2s_2_soft, \
	uml_arm64_gadget_simd_dot_indexed_##name##_2s_3_soft, \
	uml_arm64_gadget_simd_dot_indexed_##name##_4s_0_soft, \
	uml_arm64_gadget_simd_dot_indexed_##name##_4s_1_soft, \
	uml_arm64_gadget_simd_dot_indexed_##name##_4s_2_soft, \
	uml_arm64_gadget_simd_dot_indexed_##name##_4s_3_soft}

[[nodiscard]] Gadget simd_dot_indexed_gadget(
	SimdDotSignedness signedness, bool q, unsigned index,
	bool force_software) noexcept
{
	static constexpr auto signed_signed = UML_SIMD_DOT_INDEXED_ARRAY(sdot);
	static constexpr auto unsigned_unsigned = UML_SIMD_DOT_INDEXED_ARRAY(udot);
	static constexpr auto signed_signed_software =
		UML_SIMD_DOT_INDEXED_SOFT_ARRAY(sdot);
	static constexpr auto unsigned_unsigned_software =
		UML_SIMD_DOT_INDEXED_SOFT_ARRAY(udot);
	static constexpr auto signed_unsigned_native =
		UML_SIMD_DOT_INDEXED_ARRAY(sudot);
	static constexpr auto unsigned_signed_native =
		UML_SIMD_DOT_INDEXED_ARRAY(usdot);
	static constexpr auto signed_unsigned_software =
		UML_SIMD_DOT_INDEXED_SOFT_ARRAY(sudot);
	static constexpr auto unsigned_signed_software =
		UML_SIMD_DOT_INDEXED_SOFT_ARRAY(usdot);
	const unsigned shape = (q ? 4U : 0U) + index;
	switch (signedness) {
	case SimdDotSignedness::signed_signed:
		return (!force_software &&
			host_capabilities().has(ArmFeature::dotprod) ?
			signed_signed : signed_signed_software)[shape];
	case SimdDotSignedness::unsigned_unsigned:
		return (!force_software &&
			host_capabilities().has(ArmFeature::dotprod) ?
			unsigned_unsigned : unsigned_unsigned_software)[shape];
	case SimdDotSignedness::signed_unsigned:
		return (!force_software &&
			host_capabilities().has(ArmFeature::i8mm) ?
			signed_unsigned_native :
			signed_unsigned_software)[shape];
	case SimdDotSignedness::unsigned_signed:
		return (!force_software &&
			host_capabilities().has(ArmFeature::i8mm) ?
			unsigned_signed_native :
			unsigned_signed_software)[shape];
	}
	return nullptr;
}

#undef UML_SIMD_DOT_INDEXED_SOFT_ARRAY
#undef UML_SIMD_DOT_INDEXED_ARRAY

[[nodiscard]] Gadget simd_matrix_gadget(
	SimdDotSignedness signedness, bool force_software) noexcept
{
	static constexpr std::array<Gadget, 3> native{
		uml_arm64_gadget_simd_matrix_smmla,
		uml_arm64_gadget_simd_matrix_ummla,
		uml_arm64_gadget_simd_matrix_usmmla};
	static constexpr std::array<Gadget, 3> software{
		uml_arm64_gadget_simd_matrix_smmla_soft,
		uml_arm64_gadget_simd_matrix_ummla_soft,
		uml_arm64_gadget_simd_matrix_usmmla_soft};
	unsigned index;
	switch (signedness) {
	case SimdDotSignedness::signed_signed: index = 0U; break;
	case SimdDotSignedness::unsigned_unsigned: index = 1U; break;
	case SimdDotSignedness::unsigned_signed: index = 2U; break;
	case SimdDotSignedness::signed_unsigned: return nullptr;
	}
	return (!force_software && host_capabilities().has(ArmFeature::i8mm) ?
		native : software)[index];
}

#define UML_SIMD_WIDENING_V6(name) SimdWideningGadgets{ \
	uml_arm64_gadget_simd_widening_##name##_8b, \
	uml_arm64_gadget_simd_widening_##name##_4h, \
	uml_arm64_gadget_simd_widening_##name##_2s, \
	uml_arm64_gadget_simd_widening_##name##_16b, \
	uml_arm64_gadget_simd_widening_##name##_8h, \
	uml_arm64_gadget_simd_widening_##name##_4s}
#define UML_SIMD_WIDENING_SAT(name) SimdWideningGadgets{ \
	nullptr, uml_arm64_gadget_simd_widening_##name##_4h, \
	uml_arm64_gadget_simd_widening_##name##_2s, nullptr, \
	uml_arm64_gadget_simd_widening_##name##_8h, \
	uml_arm64_gadget_simd_widening_##name##_4s}
#define UML_SIMD_WIDENING_SCALAR(name) std::array<Gadget, 3>{ \
	nullptr, uml_arm64_gadget_simd_widening_##name##_h, \
	uml_arm64_gadget_simd_widening_##name##_s}

[[nodiscard]] Gadget simd_widening_gadget(unsigned operation,
	bool unsigned_values, bool scalar, bool upper, unsigned size) noexcept
{
	if (size > 2U)
		return nullptr;
	static constexpr auto saddl = UML_SIMD_WIDENING_V6(saddl);
	static constexpr auto uaddl = UML_SIMD_WIDENING_V6(uaddl);
	static constexpr auto saddw = UML_SIMD_WIDENING_V6(saddw);
	static constexpr auto uaddw = UML_SIMD_WIDENING_V6(uaddw);
	static constexpr auto ssubl = UML_SIMD_WIDENING_V6(ssubl);
	static constexpr auto usubl = UML_SIMD_WIDENING_V6(usubl);
	static constexpr auto ssubw = UML_SIMD_WIDENING_V6(ssubw);
	static constexpr auto usubw = UML_SIMD_WIDENING_V6(usubw);
	static constexpr auto sabal = UML_SIMD_WIDENING_V6(sabal);
	static constexpr auto uabal = UML_SIMD_WIDENING_V6(uabal);
	static constexpr auto sabdl = UML_SIMD_WIDENING_V6(sabdl);
	static constexpr auto uabdl = UML_SIMD_WIDENING_V6(uabdl);
	static constexpr auto smlal = UML_SIMD_WIDENING_V6(smlal);
	static constexpr auto umlal = UML_SIMD_WIDENING_V6(umlal);
	static constexpr auto smlsl = UML_SIMD_WIDENING_V6(smlsl);
	static constexpr auto umlsl = UML_SIMD_WIDENING_V6(umlsl);
	static constexpr auto smull = UML_SIMD_WIDENING_V6(smull);
	static constexpr auto umull = UML_SIMD_WIDENING_V6(umull);
	static constexpr auto sqdmlal = UML_SIMD_WIDENING_SAT(sqdmlal);
	static constexpr auto sqdmlsl = UML_SIMD_WIDENING_SAT(sqdmlsl);
	static constexpr auto sqdmull = UML_SIMD_WIDENING_SAT(sqdmull);
	static constexpr auto sqdmlal_scalar = UML_SIMD_WIDENING_SCALAR(sqdmlal);
	static constexpr auto sqdmlsl_scalar = UML_SIMD_WIDENING_SCALAR(sqdmlsl);
	static constexpr auto sqdmull_scalar = UML_SIMD_WIDENING_SCALAR(sqdmull);

	if (scalar) {
		if (unsigned_values)
			return nullptr;
		switch (operation) {
		case 9: return sqdmlal_scalar[size];
		case 11: return sqdmlsl_scalar[size];
		case 13: return sqdmull_scalar[size];
		default: return nullptr;
		}
	}
	const unsigned shape = size + (upper ? 3U : 0U);
	switch (operation) {
	case 0: return (unsigned_values ? uaddl : saddl)[shape];
	case 1: return (unsigned_values ? uaddw : saddw)[shape];
	case 2: return (unsigned_values ? usubl : ssubl)[shape];
	case 3: return (unsigned_values ? usubw : ssubw)[shape];
	case 5: return (unsigned_values ? uabal : sabal)[shape];
	case 7: return (unsigned_values ? uabdl : sabdl)[shape];
	case 8: return (unsigned_values ? umlal : smlal)[shape];
	case 9: return unsigned_values ? nullptr : sqdmlal[shape];
	case 10: return (unsigned_values ? umlsl : smlsl)[shape];
	case 11: return unsigned_values ? nullptr : sqdmlsl[shape];
	case 12: return (unsigned_values ? umull : smull)[shape];
	case 13: return unsigned_values ? nullptr : sqdmull[shape];
	default: return nullptr;
	}
}

#undef UML_SIMD_WIDENING_SCALAR
#undef UML_SIMD_WIDENING_SAT
#undef UML_SIMD_WIDENING_V6

#define UML_SIMD_HIGH_NARROW(name) SimdWideningGadgets{ \
	uml_arm64_gadget_simd_widening_##name##_8h, \
	uml_arm64_gadget_simd_widening_##name##_4s, \
	uml_arm64_gadget_simd_widening_##name##_2d, \
	uml_arm64_gadget_simd_widening_##name##_8h2, \
	uml_arm64_gadget_simd_widening_##name##_4s2, \
	uml_arm64_gadget_simd_widening_##name##_2d2}

[[nodiscard]] Gadget simd_high_narrow_gadget(bool subtract, bool rounding,
	bool upper, unsigned size) noexcept
{
	if (size > 2U)
		return nullptr;
	static constexpr auto addhn = UML_SIMD_HIGH_NARROW(addhn);
	static constexpr auto raddhn = UML_SIMD_HIGH_NARROW(raddhn);
	static constexpr auto subhn = UML_SIMD_HIGH_NARROW(subhn);
	static constexpr auto rsubhn = UML_SIMD_HIGH_NARROW(rsubhn);
	const unsigned shape = size + (upper ? 3U : 0U);
	if (subtract)
		return (rounding ? rsubhn : subhn)[shape];
	return (rounding ? raddhn : addhn)[shape];
}

#undef UML_SIMD_HIGH_NARROW

[[nodiscard]] std::uintptr_t simd_unary_operand(
	unsigned destination, unsigned source) noexcept
{
	return packed_u16_operands(vector_operand_offset(destination),
		vector_operand_offset(source));
}

[[nodiscard]] std::uintptr_t simd_ternary_operand(
	unsigned destination, unsigned first, unsigned second,
	unsigned descriptor) noexcept
{
	return packed_u16_operands(vector_operand_offset(destination),
		vector_operand_offset(first), vector_operand_offset(second),
		descriptor);
}

[[nodiscard]] std::uintptr_t simd_quaternary_operand(
	unsigned destination, unsigned first, unsigned second,
	unsigned third) noexcept
{
	return packed_u16_operands(vector_operand_offset(destination),
		vector_operand_offset(first), vector_operand_offset(second),
		vector_operand_offset(third));
}

#define UML_SIMD_UNARY_V6(name) SimdUnaryGadgets{ \
	uml_arm64_gadget_simd_unary_##name##_8b, \
	uml_arm64_gadget_simd_unary_##name##_4h, \
	uml_arm64_gadget_simd_unary_##name##_2s, \
	uml_arm64_gadget_simd_unary_##name##_16b, \
	uml_arm64_gadget_simd_unary_##name##_8h, \
	uml_arm64_gadget_simd_unary_##name##_4s, \
	nullptr, nullptr, nullptr, nullptr, nullptr}
#define UML_SIMD_UNARY_V7(name) SimdUnaryGadgets{ \
	uml_arm64_gadget_simd_unary_##name##_8b, \
	uml_arm64_gadget_simd_unary_##name##_4h, \
	uml_arm64_gadget_simd_unary_##name##_2s, \
	uml_arm64_gadget_simd_unary_##name##_16b, \
	uml_arm64_gadget_simd_unary_##name##_8h, \
	uml_arm64_gadget_simd_unary_##name##_4s, \
	uml_arm64_gadget_simd_unary_##name##_2d, \
	nullptr, nullptr, nullptr, nullptr}
#define UML_SIMD_UNARY_V7_S4(name) SimdUnaryGadgets{ \
	uml_arm64_gadget_simd_unary_##name##_8b, \
	uml_arm64_gadget_simd_unary_##name##_4h, \
	uml_arm64_gadget_simd_unary_##name##_2s, \
	uml_arm64_gadget_simd_unary_##name##_16b, \
	uml_arm64_gadget_simd_unary_##name##_8h, \
	uml_arm64_gadget_simd_unary_##name##_4s, \
	uml_arm64_gadget_simd_unary_##name##_2d, \
	uml_arm64_gadget_simd_unary_##name##_b, \
	uml_arm64_gadget_simd_unary_##name##_h, \
	uml_arm64_gadget_simd_unary_##name##_s, \
	uml_arm64_gadget_simd_unary_##name##_d}
#define UML_SIMD_UNARY_V7_SD(name) SimdUnaryGadgets{ \
	uml_arm64_gadget_simd_unary_##name##_8b, \
	uml_arm64_gadget_simd_unary_##name##_4h, \
	uml_arm64_gadget_simd_unary_##name##_2s, \
	uml_arm64_gadget_simd_unary_##name##_16b, \
	uml_arm64_gadget_simd_unary_##name##_8h, \
	uml_arm64_gadget_simd_unary_##name##_4s, \
	uml_arm64_gadget_simd_unary_##name##_2d, \
	nullptr, nullptr, nullptr, \
	uml_arm64_gadget_simd_unary_##name##_d}

[[nodiscard]] Gadget simd_integer_unary_gadget(unsigned opcode,
	bool unsigned_group, bool scalar, bool q, unsigned size) noexcept
{
	if ((!scalar && !q && size == 3U) || size > 3U)
		return nullptr;
	const unsigned shape = scalar ? 7U + size : q ? 3U + size : size;
	static constexpr auto saddlp = UML_SIMD_UNARY_V6(saddlp);
	static constexpr auto uaddlp = UML_SIMD_UNARY_V6(uaddlp);
	static constexpr auto sadalp = UML_SIMD_UNARY_V6(sadalp);
	static constexpr auto uadalp = UML_SIMD_UNARY_V6(uadalp);
	static constexpr auto suqadd = UML_SIMD_UNARY_V7_S4(suqadd);
	static constexpr auto usqadd = UML_SIMD_UNARY_V7_S4(usqadd);
	static constexpr auto cls = UML_SIMD_UNARY_V6(cls);
	static constexpr auto clz = UML_SIMD_UNARY_V6(clz);
	static constexpr auto sqabs = UML_SIMD_UNARY_V7_S4(sqabs);
	static constexpr auto sqneg = UML_SIMD_UNARY_V7_S4(sqneg);
	static constexpr auto abs = UML_SIMD_UNARY_V7_SD(abs);
	static constexpr auto neg = UML_SIMD_UNARY_V7_SD(neg);
	static constexpr SimdWideningGadgets xtn{
		uml_arm64_gadget_simd_unary_xtn_8h,
		uml_arm64_gadget_simd_unary_xtn_4s,
		uml_arm64_gadget_simd_unary_xtn_2d,
		uml_arm64_gadget_simd_unary_xtn_8h2,
		uml_arm64_gadget_simd_unary_xtn_4s2,
		uml_arm64_gadget_simd_unary_xtn_2d2};
#define UML_SIMD_SAT_NARROW(name) SimdWideningGadgets{ \
	uml_arm64_gadget_simd_unary_##name##_8h, \
	uml_arm64_gadget_simd_unary_##name##_4s, \
	uml_arm64_gadget_simd_unary_##name##_2d, \
	uml_arm64_gadget_simd_unary_##name##_8h2, \
	uml_arm64_gadget_simd_unary_##name##_4s2, \
	uml_arm64_gadget_simd_unary_##name##_2d2}
	static constexpr auto sqxtn = UML_SIMD_SAT_NARROW(sqxtn);
	static constexpr auto uqxtn = UML_SIMD_SAT_NARROW(uqxtn);
	static constexpr auto sqxtun = UML_SIMD_SAT_NARROW(sqxtun);
	static constexpr std::array<Gadget, 3> sqxtn_scalar{
		uml_arm64_gadget_simd_unary_sqxtn_b,
		uml_arm64_gadget_simd_unary_sqxtn_h,
		uml_arm64_gadget_simd_unary_sqxtn_s};
	static constexpr std::array<Gadget, 3> uqxtn_scalar{
		uml_arm64_gadget_simd_unary_uqxtn_b,
		uml_arm64_gadget_simd_unary_uqxtn_h,
		uml_arm64_gadget_simd_unary_uqxtn_s};
	static constexpr std::array<Gadget, 3> sqxtun_scalar{
		uml_arm64_gadget_simd_unary_sqxtun_b,
		uml_arm64_gadget_simd_unary_sqxtun_h,
		uml_arm64_gadget_simd_unary_sqxtun_s};
	static constexpr auto shll = UML_SIMD_UNARY_V6(shll);
#undef UML_SIMD_SAT_NARROW

	switch (opcode) {
	case 2: return scalar ? nullptr : (unsigned_group ? uaddlp : saddlp)[shape];
	case 3: return (unsigned_group ? usqadd : suqadd)[shape];
	case 4: return scalar ? nullptr : (unsigned_group ? clz : cls)[shape];
	case 6: return scalar ? nullptr : (unsigned_group ? uadalp : sadalp)[shape];
	case 7: return (unsigned_group ? sqneg : sqabs)[shape];
	case 11: return (unsigned_group ? neg : abs)[shape];
	case 18:
		if (size > 2U)
			return nullptr;
		if (!unsigned_group)
			return scalar ? nullptr : xtn[size + (q ? 3U : 0U)];
		return scalar ? sqxtun_scalar[size] :
			sqxtun[size + (q ? 3U : 0U)];
	case 19:
		return !scalar && unsigned_group ? shll[shape] : nullptr;
	case 20:
		if (size > 2U)
			return nullptr;
		if (scalar)
			return (unsigned_group ? uqxtn_scalar : sqxtn_scalar)[size];
		return (unsigned_group ? uqxtn : sqxtn)[size + (q ? 3U : 0U)];
	default: return nullptr;
	}
}

[[nodiscard]] Gadget simd_compare_zero_native_gadget(unsigned opcode,
	bool alternate, bool scalar, bool q, unsigned size) noexcept
{
	if ((!scalar && !q && size == 3U) || (scalar && size != 3U))
		return nullptr;
	const unsigned shape = scalar ? 10U : q ? 3U + size : size;
	static constexpr auto cmgt = UML_SIMD_UNARY_V7_SD(cmgt0);
	static constexpr auto cmge = UML_SIMD_UNARY_V7_SD(cmge0);
	static constexpr auto cmeq = UML_SIMD_UNARY_V7_SD(cmeq0);
	static constexpr auto cmle = UML_SIMD_UNARY_V7_SD(cmle0);
	static constexpr auto cmlt = UML_SIMD_UNARY_V7_SD(cmlt0);
	if (opcode == 8U)
		return (alternate ? cmge : cmgt)[shape];
	if (opcode == 9U)
		return (alternate ? cmle : cmeq)[shape];
	return opcode == 10U && !alternate ? cmlt[shape] : nullptr;
}

/* The eight shapes of an FP vector gadget family: scalar H, S and D, then
 * 4H, 8H, 2S, 4S and 2D. */
#define UML_SIMD_FLOATING_SHAPES(name) \
	std::array<Gadget, 8>{ \
		uml_arm64_gadget_simd_##name##_h, \
		uml_arm64_gadget_simd_##name##_s, \
		uml_arm64_gadget_simd_##name##_d, \
		uml_arm64_gadget_simd_##name##_4h, \
		uml_arm64_gadget_simd_##name##_8h, \
		uml_arm64_gadget_simd_##name##_2s, \
		uml_arm64_gadget_simd_##name##_4s, \
		uml_arm64_gadget_simd_##name##_2d}

[[nodiscard]] Gadget simd_floating_compare_gadget(
	const detail::SimdFloatingCompareDecode &decoded) noexcept
{
	static constexpr auto equal = UML_SIMD_FLOATING_SHAPES(fcmp_eq);
	static constexpr auto greater_equal = UML_SIMD_FLOATING_SHAPES(fcmp_ge);
	static constexpr auto greater = UML_SIMD_FLOATING_SHAPES(fcmp_gt);
	static constexpr auto absolute_greater_equal =
		UML_SIMD_FLOATING_SHAPES(fcmp_facge);
	static constexpr auto absolute_greater =
		UML_SIMD_FLOATING_SHAPES(fcmp_facgt);
	static constexpr auto equal_zero = UML_SIMD_FLOATING_SHAPES(fcmp_eq0);
	static constexpr auto greater_equal_zero =
		UML_SIMD_FLOATING_SHAPES(fcmp_ge0);
	static constexpr auto greater_zero = UML_SIMD_FLOATING_SHAPES(fcmp_gt0);
	static constexpr auto less_equal_zero = UML_SIMD_FLOATING_SHAPES(fcmp_le0);
	static constexpr auto less_zero = UML_SIMD_FLOATING_SHAPES(fcmp_lt0);
	using detail::SimdFloatingCompareOperation;
	if (decoded.operation == SimdFloatingCompareOperation::equal)
		return (decoded.zero ? equal_zero : equal)[decoded.shape];
	if (decoded.operation == SimdFloatingCompareOperation::greater_equal)
		return (decoded.zero ? greater_equal_zero : greater_equal)[decoded.shape];
	if (decoded.operation == SimdFloatingCompareOperation::greater)
		return (decoded.zero ? greater_zero : greater)[decoded.shape];
	if (decoded.operation == SimdFloatingCompareOperation::absolute_greater_equal)
		return absolute_greater_equal[decoded.shape];
	if (decoded.operation == SimdFloatingCompareOperation::absolute_greater)
		return absolute_greater[decoded.shape];
	if (decoded.operation == SimdFloatingCompareOperation::less_equal)
		return less_equal_zero[decoded.shape];
	return less_zero[decoded.shape];
}

[[nodiscard]] Gadget simd_floating_precision_vector_gadget(
	const detail::SimdFloatingPrecisionVectorDecode &decoded) noexcept
{
	if (decoded.round_odd)
		return decoded.scalar ? uml_arm64_gadget_simd_fcvtxn_scalar :
			decoded.upper ? uml_arm64_gadget_simd_fcvtxn2_vector :
				uml_arm64_gadget_simd_fcvtxn_vector;
	static constexpr std::array lower{
		uml_arm64_gadget_simd_fcvtl_hs,
		uml_arm64_gadget_simd_fcvtl_sd,
		uml_arm64_gadget_simd_fcvtn_sh,
		uml_arm64_gadget_simd_fcvtn_ds,
	};
	static constexpr std::array upper{
		uml_arm64_gadget_simd_fcvtl2_hs,
		uml_arm64_gadget_simd_fcvtl2_sd,
		uml_arm64_gadget_simd_fcvtn2_sh,
		uml_arm64_gadget_simd_fcvtn2_ds,
	};
	return (decoded.upper ? upper : lower)[decoded.shape];
}

#define UML_FIXED_TABLE(name) \
	std::array<Gadget, 2>{uml_arm64_gadget_##name, \
		uml_arm64_gadget_##name##_end}
#define UML_SIMD_FIXED_TABLES(operation) \
	std::array<std::array<Gadget, 2>, 8>{ \
		UML_FIXED_TABLE(simd_fixed_##operation##_h), \
		UML_FIXED_TABLE(simd_fixed_##operation##_s), \
		UML_FIXED_TABLE(simd_fixed_##operation##_d), \
		UML_FIXED_TABLE(simd_fixed_##operation##_4h), \
		UML_FIXED_TABLE(simd_fixed_##operation##_8h), \
		UML_FIXED_TABLE(simd_fixed_##operation##_2s), \
		UML_FIXED_TABLE(simd_fixed_##operation##_4s), \
		UML_FIXED_TABLE(simd_fixed_##operation##_2d)}

[[nodiscard]] Gadget floating_to_integer_gadget(
	const detail::FloatingToIntegerDecode &decoded, bool discard) noexcept
{
	if (decoded.source_width == 16U && !host_capabilities().fp16)
		return uml_arm64_gadget_floating_to_integer_software_fallback;
	const auto fixed_table_entry = [](Gadget begin, Gadget end,
		unsigned count, unsigned index) noexcept {
		const auto first = reinterpret_cast<std::uintptr_t>(begin);
		const auto limit = reinterpret_cast<std::uintptr_t>(end);
		return reinterpret_cast<Gadget>(
			first + (limit - first) / count * index);
	};
#define UML_FLOATING_TO_GPR_FIXED(operation, suffix) \
	std::array<std::array<Gadget, 2>, 6>{ \
		UML_FIXED_TABLE(floating_to_gpr_fixed_##operation##_w_h##suffix), \
		UML_FIXED_TABLE(floating_to_gpr_fixed_##operation##_w_s##suffix), \
		UML_FIXED_TABLE(floating_to_gpr_fixed_##operation##_w_d##suffix), \
		UML_FIXED_TABLE(floating_to_gpr_fixed_##operation##_x_h##suffix), \
		UML_FIXED_TABLE(floating_to_gpr_fixed_##operation##_x_s##suffix), \
		UML_FIXED_TABLE(floating_to_gpr_fixed_##operation##_x_d##suffix)}
	static constexpr auto signed_fixed =
		UML_FLOATING_TO_GPR_FIXED(fcvtzs, );
	static constexpr auto unsigned_fixed =
		UML_FLOATING_TO_GPR_FIXED(fcvtzu, );
	static constexpr auto signed_fixed_discard =
		UML_FLOATING_TO_GPR_FIXED(fcvtzs, _discard);
	static constexpr auto unsigned_fixed_discard =
		UML_FLOATING_TO_GPR_FIXED(fcvtzu, _discard);
	static constexpr auto signed_simd_fixed =
		UML_SIMD_FIXED_TABLES(fcvtzs);
	static constexpr auto unsigned_simd_fixed =
		UML_SIMD_FIXED_TABLES(fcvtzu);
#undef UML_FLOATING_TO_GPR_FIXED
	if (decoded.fixed_point) {
		const unsigned offset = decoded.fractional_bits - 1U;
		if (!decoded.destination_gpr) {
			const auto &table =
				(decoded.unsigned_result ? unsigned_simd_fixed :
					signed_simd_fixed)[decoded.shape];
			return fixed_table_entry(table[0], table[1],
				decoded.integer_width, offset);
		}
		const unsigned index = (decoded.integer_width == 64U ? 3U : 0U) +
			decoded.shape;
		const auto &tables = discard ?
			(decoded.unsigned_result ? unsigned_fixed_discard :
				signed_fixed_discard) :
			(decoded.unsigned_result ? unsigned_fixed : signed_fixed);
		const auto &table = tables[index];
		return fixed_table_entry(table[0], table[1],
			decoded.integer_width, offset);
	}
#define UML_FLOATING_TO_GPR(operation, suffix) \
	std::array<Gadget, 6>{ \
		uml_arm64_gadget_floating_to_gpr_##operation##_w_h##suffix, \
		uml_arm64_gadget_floating_to_gpr_##operation##_w_s##suffix, \
		uml_arm64_gadget_floating_to_gpr_##operation##_w_d##suffix, \
		uml_arm64_gadget_floating_to_gpr_##operation##_x_h##suffix, \
		uml_arm64_gadget_floating_to_gpr_##operation##_x_s##suffix, \
		uml_arm64_gadget_floating_to_gpr_##operation##_x_d##suffix}
	static constexpr std::array signed_gpr{
		UML_FLOATING_TO_GPR(fcvtns, ), UML_FLOATING_TO_GPR(fcvtps, ),
		UML_FLOATING_TO_GPR(fcvtms, ), UML_FLOATING_TO_GPR(fcvtzs, ),
		UML_FLOATING_TO_GPR(fcvtas, )};
	static constexpr std::array unsigned_gpr{
		UML_FLOATING_TO_GPR(fcvtnu, ), UML_FLOATING_TO_GPR(fcvtpu, ),
		UML_FLOATING_TO_GPR(fcvtmu, ), UML_FLOATING_TO_GPR(fcvtzu, ),
		UML_FLOATING_TO_GPR(fcvtau, )};
	static constexpr std::array signed_discard{
		UML_FLOATING_TO_GPR(fcvtns, _discard),
		UML_FLOATING_TO_GPR(fcvtps, _discard),
		UML_FLOATING_TO_GPR(fcvtms, _discard),
		UML_FLOATING_TO_GPR(fcvtzs, _discard),
		UML_FLOATING_TO_GPR(fcvtas, _discard)};
	static constexpr std::array unsigned_discard{
		UML_FLOATING_TO_GPR(fcvtnu, _discard),
		UML_FLOATING_TO_GPR(fcvtpu, _discard),
		UML_FLOATING_TO_GPR(fcvtmu, _discard),
		UML_FLOATING_TO_GPR(fcvtzu, _discard),
		UML_FLOATING_TO_GPR(fcvtau, _discard)};
#undef UML_FLOATING_TO_GPR
	static constexpr std::array signed_simd{
		UML_SIMD_FLOATING_SHAPES(fcvtns),
		UML_SIMD_FLOATING_SHAPES(fcvtps),
		UML_SIMD_FLOATING_SHAPES(fcvtms),
		UML_SIMD_FLOATING_SHAPES(fcvtzs),
		UML_SIMD_FLOATING_SHAPES(fcvtas)};
	static constexpr std::array unsigned_simd{
		UML_SIMD_FLOATING_SHAPES(fcvtnu),
		UML_SIMD_FLOATING_SHAPES(fcvtpu),
		UML_SIMD_FLOATING_SHAPES(fcvtmu),
		UML_SIMD_FLOATING_SHAPES(fcvtzu),
		UML_SIMD_FLOATING_SHAPES(fcvtau)};
	const unsigned rounding = static_cast<unsigned>(decoded.rounding);
	if (!decoded.destination_gpr)
		return (decoded.unsigned_result ? unsigned_simd : signed_simd)
			[rounding][decoded.shape];
	const unsigned index = (decoded.integer_width == 64U ? 3U : 0U) +
		decoded.shape;
	if (discard)
		return (decoded.unsigned_result ? unsigned_discard :
			signed_discard)[rounding][index];
	return (decoded.unsigned_result ? unsigned_gpr : signed_gpr)
		[rounding][index];
}

[[nodiscard]] Gadget integer_to_floating_gadget(
	const detail::IntegerToFloatingDecode &decoded, bool zero_source) noexcept
{
	if (decoded.destination_width == 16U && !host_capabilities().fp16)
		return uml_arm64_gadget_integer_to_floating_software_fallback;
	const auto fixed_table_entry = [](Gadget begin, Gadget end,
		unsigned count, unsigned index) noexcept {
		const auto first = reinterpret_cast<std::uintptr_t>(begin);
		const auto limit = reinterpret_cast<std::uintptr_t>(end);
		return reinterpret_cast<Gadget>(
			first + (limit - first) / count * index);
	};
#define UML_GPR_TO_FLOATING_FIXED(operation, zero) \
	std::array<std::array<Gadget, 2>, 6>{ \
		UML_FIXED_TABLE( \
			gpr_to_floating_fixed_##operation##_w_h##zero), \
		UML_FIXED_TABLE( \
			gpr_to_floating_fixed_##operation##_w_s##zero), \
		UML_FIXED_TABLE( \
			gpr_to_floating_fixed_##operation##_w_d##zero), \
		UML_FIXED_TABLE( \
			gpr_to_floating_fixed_##operation##_x_h##zero), \
		UML_FIXED_TABLE( \
			gpr_to_floating_fixed_##operation##_x_s##zero), \
		UML_FIXED_TABLE( \
			gpr_to_floating_fixed_##operation##_x_d##zero)}
	static constexpr auto signed_gpr_fixed =
		UML_GPR_TO_FLOATING_FIXED(scvtf, 0);
	static constexpr auto unsigned_gpr_fixed =
		UML_GPR_TO_FLOATING_FIXED(ucvtf, 0);
	static constexpr auto signed_zero_fixed =
		UML_GPR_TO_FLOATING_FIXED(scvtf, 1);
	static constexpr auto unsigned_zero_fixed =
		UML_GPR_TO_FLOATING_FIXED(ucvtf, 1);
	static constexpr auto signed_simd_fixed =
		UML_SIMD_FIXED_TABLES(scvtf);
	static constexpr auto unsigned_simd_fixed =
		UML_SIMD_FIXED_TABLES(ucvtf);
#undef UML_GPR_TO_FLOATING_FIXED
	if (decoded.fixed_point) {
		const unsigned offset = decoded.fractional_bits - 1U;
		if (!decoded.source_gpr) {
			const auto &table =
				(decoded.unsigned_source ? unsigned_simd_fixed :
					signed_simd_fixed)[decoded.shape];
			return fixed_table_entry(table[0], table[1],
				decoded.integer_width, offset);
		}
		const unsigned index = (decoded.integer_width == 64U ? 3U : 0U) +
			decoded.shape;
		const auto &tables = zero_source ?
			(decoded.unsigned_source ? unsigned_zero_fixed :
				signed_zero_fixed) :
			(decoded.unsigned_source ? unsigned_gpr_fixed :
				signed_gpr_fixed);
		const auto &table = tables[index];
		return fixed_table_entry(table[0], table[1],
			decoded.integer_width, offset);
	}
#define UML_GPR_TO_FLOATING(operation, zero) \
	std::array<Gadget, 6>{ \
		uml_arm64_gadget_gpr_to_floating_##operation##_w_h##zero, \
		uml_arm64_gadget_gpr_to_floating_##operation##_w_s##zero, \
		uml_arm64_gadget_gpr_to_floating_##operation##_w_d##zero, \
		uml_arm64_gadget_gpr_to_floating_##operation##_x_h##zero, \
		uml_arm64_gadget_gpr_to_floating_##operation##_x_s##zero, \
		uml_arm64_gadget_gpr_to_floating_##operation##_x_d##zero}
	static constexpr auto signed_gpr = UML_GPR_TO_FLOATING(scvtf, 0);
	static constexpr auto unsigned_gpr = UML_GPR_TO_FLOATING(ucvtf, 0);
	static constexpr auto signed_zero = UML_GPR_TO_FLOATING(scvtf, 1);
	static constexpr auto unsigned_zero = UML_GPR_TO_FLOATING(ucvtf, 1);
#undef UML_GPR_TO_FLOATING
	static constexpr auto signed_simd = UML_SIMD_FLOATING_SHAPES(scvtf);
	static constexpr auto unsigned_simd = UML_SIMD_FLOATING_SHAPES(ucvtf);
	if (decoded.source_gpr) {
		const unsigned index = (decoded.integer_width == 64U ? 3U : 0U) +
			decoded.shape;
		if (zero_source)
			return (decoded.unsigned_source ? unsigned_zero :
				signed_zero)[index];
		return (decoded.unsigned_source ? unsigned_gpr : signed_gpr)[index];
	}
	return (decoded.unsigned_source ? unsigned_simd : signed_simd)[
		decoded.shape];
}

#undef UML_SIMD_FLOATING_SHAPES

#undef UML_SIMD_FIXED_TABLES
#undef UML_FIXED_TABLE

[[nodiscard]] Gadget simd_reverse_native_gadget(unsigned block_bits,
	bool q, unsigned size) noexcept
{
	static constexpr SimdWideningGadgets reverse64{
		uml_arm64_gadget_simd_unary_rev64_8b,
		uml_arm64_gadget_simd_unary_rev64_4h,
		uml_arm64_gadget_simd_unary_rev64_2s,
		uml_arm64_gadget_simd_unary_rev64_16b,
		uml_arm64_gadget_simd_unary_rev64_8h,
		uml_arm64_gadget_simd_unary_rev64_4s};
	static constexpr SimdWideningGadgets reverse32{
		uml_arm64_gadget_simd_unary_rev32_8b,
		uml_arm64_gadget_simd_unary_rev32_4h, nullptr,
		uml_arm64_gadget_simd_unary_rev32_16b,
		uml_arm64_gadget_simd_unary_rev32_8h, nullptr};
	static constexpr SimdWideningGadgets reverse16{
		uml_arm64_gadget_simd_unary_rev16_8b, nullptr, nullptr,
		uml_arm64_gadget_simd_unary_rev16_16b, nullptr, nullptr};
	if (size > 2U)
		return nullptr;
	const auto &table = block_bits == 64U ? reverse64 :
		block_bits == 32U ? reverse32 : reverse16;
	return table[size + (q ? 3U : 0U)];
}

[[nodiscard]] Gadget simd_byte_unary_gadget(unsigned operation, bool q) noexcept
{
	static constexpr std::array<Gadget, 2> count{
		uml_arm64_gadget_simd_unary_cnt_8b,
		uml_arm64_gadget_simd_unary_cnt_16b};
	static constexpr std::array<Gadget, 2> invert{
		uml_arm64_gadget_simd_unary_not_8b,
		uml_arm64_gadget_simd_unary_not_16b};
	static constexpr std::array<Gadget, 2> reverse{
		uml_arm64_gadget_simd_unary_rbit_8b,
		uml_arm64_gadget_simd_unary_rbit_16b};
	return operation == 0U ? count[q] : operation == 1U ? invert[q] : reverse[q];
}

[[nodiscard]] Gadget simd_integer_reduce_gadget(unsigned opcode,
	bool unsigned_group, bool q, unsigned size) noexcept
{
	if (size > 2U || (!q && size == 2U))
		return nullptr;
	const unsigned shape = q ? size + 2U : size;
#define UML_SIMD_REDUCE_5(name) std::array<Gadget, 5>{ \
	uml_arm64_gadget_simd_reduce_##name##_8b, \
	uml_arm64_gadget_simd_reduce_##name##_4h, \
	uml_arm64_gadget_simd_reduce_##name##_16b, \
	uml_arm64_gadget_simd_reduce_##name##_8h, \
	uml_arm64_gadget_simd_reduce_##name##_4s}
	static constexpr auto saddlv = UML_SIMD_REDUCE_5(saddlv);
	static constexpr auto uaddlv = UML_SIMD_REDUCE_5(uaddlv);
	static constexpr auto smaxv = UML_SIMD_REDUCE_5(smaxv);
	static constexpr auto umaxv = UML_SIMD_REDUCE_5(umaxv);
	static constexpr auto sminv = UML_SIMD_REDUCE_5(sminv);
	static constexpr auto uminv = UML_SIMD_REDUCE_5(uminv);
	static constexpr auto addv = UML_SIMD_REDUCE_5(addv);
#undef UML_SIMD_REDUCE_5
	if (opcode == 3U)
		return (unsigned_group ? uaddlv : saddlv)[shape];
	if (opcode == 10U)
		return (unsigned_group ? umaxv : smaxv)[shape];
	if (opcode == 26U)
		return (unsigned_group ? uminv : sminv)[shape];
	return opcode == 27U && !unsigned_group ? addv[shape] : nullptr;
}

[[nodiscard]] Gadget simd_unsigned_estimate_gadget(
	bool reciprocal_sqrt, bool q, bool invalid_size) noexcept
{
	if (invalid_size)
		return nullptr;
	if (reciprocal_sqrt)
		return q ? uml_arm64_gadget_simd_unary_ursqrte_4s :
			uml_arm64_gadget_simd_unary_ursqrte_2s;
	return q ? uml_arm64_gadget_simd_unary_urecpe_4s :
		uml_arm64_gadget_simd_unary_urecpe_2s;
}

#undef UML_SIMD_UNARY_V7_SD
#undef UML_SIMD_UNARY_V7_S4
#undef UML_SIMD_UNARY_V7
#undef UML_SIMD_UNARY_V6

[[nodiscard]] Gadget floating_arithmetic_gadget(
	unsigned type, bool one_source, unsigned operation) noexcept
{
	if (one_source) {
		switch (operation) {
		case 0:
			if (type == 3U)
				return uml_arm64_gadget_floating_move_half;
			return type == 1U ? uml_arm64_gadget_floating_move_double :
				uml_arm64_gadget_floating_move_single;
		case 1:
			if (type == 3U)
				return uml_arm64_gadget_floating_absolute_half;
			return type == 1U ? uml_arm64_gadget_floating_absolute_double :
				uml_arm64_gadget_floating_absolute_single;
		case 2:
			if (type == 3U)
				return uml_arm64_gadget_floating_negate_half;
			return type == 1U ? uml_arm64_gadget_floating_negate_double :
				uml_arm64_gadget_floating_negate_single;
		default: return type == 1U ? uml_arm64_gadget_floating_sqrt_double :
			uml_arm64_gadget_floating_sqrt_single;
		}
	}
	switch (operation) {
	case 0: return type == 1U ? uml_arm64_gadget_floating_multiply_double :
		uml_arm64_gadget_floating_multiply_single;
	case 1: return type == 1U ? uml_arm64_gadget_floating_divide_double :
		uml_arm64_gadget_floating_divide_single;
	case 2: return type == 1U ? uml_arm64_gadget_floating_add_double :
		uml_arm64_gadget_floating_add_single;
	case 3: return type == 1U ? uml_arm64_gadget_floating_subtract_double :
		uml_arm64_gadget_floating_subtract_single;
	default: return type == 1U ?
		uml_arm64_gadget_floating_negative_multiply_double :
		uml_arm64_gadget_floating_negative_multiply_single;
	}
}

/* The column of a floating-point vector gadget table, whose columns are 4H,
 * 8H, 2S, 4S and 2D, for a decode's lane `width` and `vector_bytes`. */
#define UML_FLOATING_VECTOR_SHAPE(decoded) \
	((decoded).width == 16U ? \
		((decoded).vector_bytes == 16U ? 1U : 0U) : \
		(decoded).width == 32U ? \
		((decoded).vector_bytes == 16U ? 3U : 2U) : 4U)
/* The column of a scalar floating-point gadget table: H, S and D. */
#define UML_FLOATING_SCALAR_SHAPE(decoded) \
	((decoded).width == 16U ? 0U : (decoded).width == 32U ? 1U : 2U)

[[nodiscard]] Gadget basic_floating_arithmetic_gadget(
	const detail::BasicFloatingArithmetic &decoded) noexcept
{
	using Operation = detail::BasicFloatingOperation;
	if (decoded.scalar) {
		switch (decoded.operation) {
		case Operation::multiply:
			return decoded.width == 16U ?
				uml_arm64_gadget_floating_multiply_half :
				decoded.width == 32U ?
				uml_arm64_gadget_floating_multiply_single :
				uml_arm64_gadget_floating_multiply_double;
		case Operation::divide:
			return decoded.width == 16U ?
				uml_arm64_gadget_floating_divide_half :
				decoded.width == 32U ?
				uml_arm64_gadget_floating_divide_single :
				uml_arm64_gadget_floating_divide_double;
		case Operation::add:
			return decoded.width == 16U ?
				uml_arm64_gadget_floating_add_half :
				decoded.width == 32U ?
				uml_arm64_gadget_floating_add_single :
				uml_arm64_gadget_floating_add_double;
		case Operation::subtract:
			return decoded.width == 16U ?
				uml_arm64_gadget_floating_subtract_half :
				decoded.width == 32U ?
				uml_arm64_gadget_floating_subtract_single :
				uml_arm64_gadget_floating_subtract_double;
		case Operation::negative_multiply:
			return decoded.width == 16U ?
				uml_arm64_gadget_floating_negative_multiply_half :
				decoded.width == 32U ?
				uml_arm64_gadget_floating_negative_multiply_single :
				uml_arm64_gadget_floating_negative_multiply_double;
		case Operation::square_root:
			return decoded.width == 16U ?
				uml_arm64_gadget_floating_sqrt_half :
				decoded.width == 32U ?
				uml_arm64_gadget_floating_sqrt_single :
				uml_arm64_gadget_floating_sqrt_double;
		}
	}
	const unsigned shape = UML_FLOATING_VECTOR_SHAPE(decoded);
#define UML_BASIC_FLOATING_VECTOR(operation) std::array<Gadget, 5>{ \
	uml_arm64_gadget_simd_basic_##operation##_4h, \
	uml_arm64_gadget_simd_basic_##operation##_8h, \
	uml_arm64_gadget_simd_basic_##operation##_2s, \
	uml_arm64_gadget_simd_basic_##operation##_4s, \
	uml_arm64_gadget_simd_basic_##operation##_2d}
	static constexpr auto multiply = UML_BASIC_FLOATING_VECTOR(fmul);
	static constexpr auto divide = UML_BASIC_FLOATING_VECTOR(fdiv);
	static constexpr auto add = UML_BASIC_FLOATING_VECTOR(fadd);
	static constexpr auto subtract = UML_BASIC_FLOATING_VECTOR(fsub);
	static constexpr auto square_root = UML_BASIC_FLOATING_VECTOR(fsqrt);
#undef UML_BASIC_FLOATING_VECTOR
	switch (decoded.operation) {
	case Operation::multiply: return multiply[shape];
	case Operation::divide: return divide[shape];
	case Operation::add: return add[shape];
	case Operation::subtract: return subtract[shape];
	case Operation::square_root: return square_root[shape];
	default: return nullptr;
	}
}

[[nodiscard]] unsigned floating_round_operation_index(
	detail::FloatingRoundOperation operation) noexcept
{
	using Operation = detail::FloatingRoundOperation;
	switch (operation) {
	case Operation::nearest: return 0;
	case Operation::plus: return 1;
	case Operation::minus: return 2;
	case Operation::zero: return 3;
	case Operation::ties_away: return 4;
	case Operation::exact_current: return 5;
	case Operation::current: return 6;
	case Operation::int32_exact: return 0;
	case Operation::int32_zero: return 1;
	case Operation::int64_exact: return 2;
	case Operation::int64_zero: return 3;
	}
	return 0;
}

[[nodiscard]] Gadget floating_round_gadget(
	const detail::FloatingRound &decoded) noexcept
{
	const unsigned operation =
		floating_round_operation_index(decoded.operation);
	if (!detail::is_sized_round(decoded.operation)) {
#define UML_SCALAR_ROUND(name) std::array<Gadget, 3>{ \
	uml_arm64_gadget_floating_round_##name##_h, \
	uml_arm64_gadget_floating_round_##name##_s, \
	uml_arm64_gadget_floating_round_##name##_d}
		static constexpr std::array scalar{
			UML_SCALAR_ROUND(n), UML_SCALAR_ROUND(p),
			UML_SCALAR_ROUND(m), UML_SCALAR_ROUND(z),
			UML_SCALAR_ROUND(a), UML_SCALAR_ROUND(x),
			UML_SCALAR_ROUND(i)};
#undef UML_SCALAR_ROUND
#define UML_VECTOR_ROUND(name) std::array<Gadget, 5>{ \
	uml_arm64_gadget_simd_round_##name##_4h, \
	uml_arm64_gadget_simd_round_##name##_8h, \
	uml_arm64_gadget_simd_round_##name##_2s, \
	uml_arm64_gadget_simd_round_##name##_4s, \
	uml_arm64_gadget_simd_round_##name##_2d}
		static constexpr std::array vector{
			UML_VECTOR_ROUND(n), UML_VECTOR_ROUND(p),
			UML_VECTOR_ROUND(m), UML_VECTOR_ROUND(z),
			UML_VECTOR_ROUND(a), UML_VECTOR_ROUND(x),
			UML_VECTOR_ROUND(i)};
#undef UML_VECTOR_ROUND
		if (decoded.scalar) {
			const unsigned shape = UML_FLOATING_SCALAR_SHAPE(decoded);
			return scalar[operation][shape];
		}
		const unsigned shape = UML_FLOATING_VECTOR_SHAPE(decoded);
		return vector[operation][shape];
	}
	if (!host_capabilities().has(ArmFeature::frintts))
		return uml_arm64_gadget_floating_round_sized_software;
	static constexpr std::array scalar{
		std::array<Gadget, 2>{uml_arm64_gadget_floating_round_32x_s,
			uml_arm64_gadget_floating_round_32x_d},
		std::array<Gadget, 2>{uml_arm64_gadget_floating_round_32z_s,
			uml_arm64_gadget_floating_round_32z_d},
		std::array<Gadget, 2>{uml_arm64_gadget_floating_round_64x_s,
			uml_arm64_gadget_floating_round_64x_d},
		std::array<Gadget, 2>{uml_arm64_gadget_floating_round_64z_s,
			uml_arm64_gadget_floating_round_64z_d},
	};
	static constexpr std::array vector{
		std::array<Gadget, 3>{uml_arm64_gadget_simd_round_32x_2s,
			uml_arm64_gadget_simd_round_32x_4s,
			uml_arm64_gadget_simd_round_32x_2d},
		std::array<Gadget, 3>{uml_arm64_gadget_simd_round_32z_2s,
			uml_arm64_gadget_simd_round_32z_4s,
			uml_arm64_gadget_simd_round_32z_2d},
		std::array<Gadget, 3>{uml_arm64_gadget_simd_round_64x_2s,
			uml_arm64_gadget_simd_round_64x_4s,
			uml_arm64_gadget_simd_round_64x_2d},
		std::array<Gadget, 3>{uml_arm64_gadget_simd_round_64z_2s,
			uml_arm64_gadget_simd_round_64z_4s,
			uml_arm64_gadget_simd_round_64z_2d},
	};
	if (decoded.scalar)
		return scalar[operation][decoded.width == 64U ? 1U : 0U];
	const unsigned shape = decoded.width == 64U ? 2U :
		decoded.vector_bytes == 16U ? 1U : 0U;
	return vector[operation][shape];
}

[[nodiscard]] Gadget floating_horizontal_gadget(
	const detail::FloatingHorizontal &decoded) noexcept
{
	const unsigned operation = static_cast<unsigned>(decoded.operation);
	const unsigned scalar_shape = UML_FLOATING_SCALAR_SHAPE(decoded);
	const unsigned vector_shape = UML_FLOATING_VECTOR_SHAPE(decoded);
#define UML_HORIZONTAL_SCALAR(name) std::array<Gadget, 3>{ \
	uml_arm64_gadget_floating_horizontal_##name##_h, \
	uml_arm64_gadget_floating_horizontal_##name##_s, \
	uml_arm64_gadget_floating_horizontal_##name##_d}
#define UML_HORIZONTAL_VECTOR(name) std::array<Gadget, 5>{ \
	uml_arm64_gadget_floating_horizontal_##name##_4h, \
	uml_arm64_gadget_floating_horizontal_##name##_8h, \
	uml_arm64_gadget_floating_horizontal_##name##_2s, \
	uml_arm64_gadget_floating_horizontal_##name##_4s, \
	uml_arm64_gadget_floating_horizontal_##name##_2d}
	static constexpr std::array scalar_binary{
		UML_HORIZONTAL_SCALAR(fabd_scalar),
		std::array<Gadget, 3>{},
		UML_HORIZONTAL_SCALAR(fmax_scalar),
		UML_HORIZONTAL_SCALAR(fmaxnm_scalar),
		UML_HORIZONTAL_SCALAR(fmin_scalar),
		UML_HORIZONTAL_SCALAR(fminnm_scalar)};
	static constexpr std::array vector_binary{
		UML_HORIZONTAL_VECTOR(fabd_vector),
		std::array<Gadget, 5>{},
		UML_HORIZONTAL_VECTOR(fmax_vector),
		UML_HORIZONTAL_VECTOR(fmaxnm_vector),
		UML_HORIZONTAL_VECTOR(fmin_vector),
		UML_HORIZONTAL_VECTOR(fminnm_vector)};
	static constexpr std::array scalar_pairwise{
		std::array<Gadget, 3>{},
		UML_HORIZONTAL_SCALAR(faddp_scalar),
		UML_HORIZONTAL_SCALAR(fmaxp_scalar),
		UML_HORIZONTAL_SCALAR(fmaxnmp_scalar),
		UML_HORIZONTAL_SCALAR(fminp_scalar),
		UML_HORIZONTAL_SCALAR(fminnmp_scalar)};
	static constexpr std::array vector_pairwise{
		std::array<Gadget, 5>{},
		UML_HORIZONTAL_VECTOR(faddp_vector),
		UML_HORIZONTAL_VECTOR(fmaxp_vector),
		UML_HORIZONTAL_VECTOR(fmaxnmp_vector),
		UML_HORIZONTAL_VECTOR(fminp_vector),
		UML_HORIZONTAL_VECTOR(fminnmp_vector)};
#undef UML_HORIZONTAL_VECTOR
#undef UML_HORIZONTAL_SCALAR

	using Form = detail::FloatingHorizontalForm;
	if (decoded.form == Form::scalar_binary)
		return scalar_binary[operation][scalar_shape];
	if (decoded.form == Form::vector_binary)
		return vector_binary[operation][vector_shape];
	if (decoded.form == Form::scalar_pairwise)
		return scalar_pairwise[operation][scalar_shape];
	if (decoded.form == Form::vector_pairwise)
		return vector_pairwise[operation][vector_shape];

#define UML_HORIZONTAL_REDUCE(name) std::array<Gadget, 3>{ \
	uml_arm64_gadget_floating_horizontal_##name##_4h, \
	uml_arm64_gadget_floating_horizontal_##name##_8h, \
	uml_arm64_gadget_floating_horizontal_##name##_4s}
	static constexpr std::array reduction{
		std::array<Gadget, 3>{}, std::array<Gadget, 3>{},
		UML_HORIZONTAL_REDUCE(fmaxv),
		UML_HORIZONTAL_REDUCE(fmaxnmv),
		UML_HORIZONTAL_REDUCE(fminv),
		UML_HORIZONTAL_REDUCE(fminnmv)};
#undef UML_HORIZONTAL_REDUCE
	const unsigned shape = decoded.width == 32U ? 2U :
		decoded.vector_bytes == 16U ? 1U : 0U;
	return reduction[operation][shape];
}

[[nodiscard]] Gadget bfloat_conversion_gadget(
	const detail::BFloatConversion &decoded) noexcept
{
	if (!host_capabilities().has(ArmFeature::bf16))
		return uml_arm64_gadget_bfloat_convert_software;
	if (decoded.scalar)
		return uml_arm64_gadget_bfloat_convert_scalar_native;
	return decoded.upper ? uml_arm64_gadget_bfloat_convert_upper_native :
		uml_arm64_gadget_bfloat_convert_lower_native;
}

[[nodiscard]] Gadget bfloat_multiply_long_gadget(
	const detail::BFloatMultiplyLong &decoded) noexcept
{
	if (!host_capabilities().has(ArmFeature::bf16))
		return uml_arm64_gadget_bfloat_multiply_long_software;
	static constexpr std::array vector{
		uml_arm64_gadget_bfloat_multiply_long_native_vector_b,
		uml_arm64_gadget_bfloat_multiply_long_native_vector_t};
	static constexpr std::array indexed{
		std::array<Gadget, 8>{
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_0,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_1,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_2,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_3,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_4,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_5,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_6,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_7},
		std::array<Gadget, 8>{
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_0,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_1,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_2,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_3,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_4,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_5,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_6,
			uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_7}};
	return decoded.by_element ? indexed[decoded.top][decoded.index] :
		vector[decoded.top];
}

[[nodiscard]] Gadget bfloat_dot_gadget(const detail::BFloatDot &decoded) noexcept
{
	if (!host_capabilities().has(ArmFeature::bf16))
		return uml_arm64_gadget_bfloat_dot_software;
	static constexpr std::array vector{
		uml_arm64_gadget_bfloat_dot_native_vector_2s,
		uml_arm64_gadget_bfloat_dot_native_vector_4s};
	static constexpr std::array indexed{
		std::array<Gadget, 4>{
			uml_arm64_gadget_bfloat_dot_native_indexed_2s_0,
			uml_arm64_gadget_bfloat_dot_native_indexed_2s_1,
			uml_arm64_gadget_bfloat_dot_native_indexed_2s_2,
			uml_arm64_gadget_bfloat_dot_native_indexed_2s_3},
		std::array<Gadget, 4>{
			uml_arm64_gadget_bfloat_dot_native_indexed_4s_0,
			uml_arm64_gadget_bfloat_dot_native_indexed_4s_1,
			uml_arm64_gadget_bfloat_dot_native_indexed_4s_2,
			uml_arm64_gadget_bfloat_dot_native_indexed_4s_3}};
	return decoded.by_element ? indexed[decoded.wide][decoded.index] :
		vector[decoded.wide];
}

[[nodiscard]] Gadget bfloat_matrix_gadget() noexcept
{
	return host_capabilities().has(ArmFeature::bf16) ?
		uml_arm64_gadget_bfloat_matrix_native :
		uml_arm64_gadget_bfloat_matrix_software;
}

[[nodiscard]] Gadget half_fused_long_gadget(
	const detail::HalfFusedLong &decoded) noexcept
{
	if (!host_capabilities().has(ArmFeature::fhm))
		return uml_arm64_gadget_half_fused_long_software;
#define UML_HALF_FUSED_LONG_NATIVE(name) \
	uml_arm64_gadget_half_fused_long_native_##name
	static constexpr std::array vector{
		UML_HALF_FUSED_LONG_NATIVE(vector_add_lower_2s),
		UML_HALF_FUSED_LONG_NATIVE(vector_add_lower_4s),
		UML_HALF_FUSED_LONG_NATIVE(vector_add_upper_2s),
		UML_HALF_FUSED_LONG_NATIVE(vector_add_upper_4s),
		UML_HALF_FUSED_LONG_NATIVE(vector_sub_lower_2s),
		UML_HALF_FUSED_LONG_NATIVE(vector_sub_lower_4s),
		UML_HALF_FUSED_LONG_NATIVE(vector_sub_upper_2s),
		UML_HALF_FUSED_LONG_NATIVE(vector_sub_upper_4s)};
#define UML_HALF_FUSED_LONG_INDEXED(operation, half, shape) \
	UML_HALF_FUSED_LONG_NATIVE(indexed_##operation##_##half##_##shape##_0), \
	UML_HALF_FUSED_LONG_NATIVE(indexed_##operation##_##half##_##shape##_1), \
	UML_HALF_FUSED_LONG_NATIVE(indexed_##operation##_##half##_##shape##_2), \
	UML_HALF_FUSED_LONG_NATIVE(indexed_##operation##_##half##_##shape##_3), \
	UML_HALF_FUSED_LONG_NATIVE(indexed_##operation##_##half##_##shape##_4), \
	UML_HALF_FUSED_LONG_NATIVE(indexed_##operation##_##half##_##shape##_5), \
	UML_HALF_FUSED_LONG_NATIVE(indexed_##operation##_##half##_##shape##_6), \
	UML_HALF_FUSED_LONG_NATIVE(indexed_##operation##_##half##_##shape##_7)
	static constexpr std::array indexed{
		UML_HALF_FUSED_LONG_INDEXED(add, lower, 2s),
		UML_HALF_FUSED_LONG_INDEXED(add, lower, 4s),
		UML_HALF_FUSED_LONG_INDEXED(add, upper, 2s),
		UML_HALF_FUSED_LONG_INDEXED(add, upper, 4s),
		UML_HALF_FUSED_LONG_INDEXED(sub, lower, 2s),
		UML_HALF_FUSED_LONG_INDEXED(sub, lower, 4s),
		UML_HALF_FUSED_LONG_INDEXED(sub, upper, 2s),
		UML_HALF_FUSED_LONG_INDEXED(sub, upper, 4s)};
#undef UML_HALF_FUSED_LONG_INDEXED
#undef UML_HALF_FUSED_LONG_NATIVE
	const unsigned operation = static_cast<unsigned>(decoded.subtract) * 4U +
		static_cast<unsigned>(decoded.upper) * 2U + decoded.wide;
	return decoded.by_element ?
		indexed[operation * 8U + decoded.index] :
		vector[operation];
}

[[nodiscard]] Gadget floating_reciprocal_gadget(
	const detail::FloatingReciprocal &decoded) noexcept
{
#define UML_RECIPROCAL_SCALAR(host, name) std::array<Gadget, 3>{ \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_h, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_s, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_d}
#define UML_RECIPROCAL_VECTOR(host, name) std::array<Gadget, 5>{ \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_4h, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_8h, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_2s, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_4s, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_2d}
#define UML_RECIPROCAL_SCALAR_PAIR(name) UML_RECIPROCAL_SCALAR(native, name)
#define UML_RECIPROCAL_VECTOR_PAIR(name) UML_RECIPROCAL_VECTOR(native, name)
	static constexpr std::array scalar{
		UML_RECIPROCAL_SCALAR_PAIR(frecpe),
		UML_RECIPROCAL_SCALAR_PAIR(frecps),
		UML_RECIPROCAL_SCALAR_PAIR(frecpx),
		UML_RECIPROCAL_SCALAR_PAIR(frsqrte),
		UML_RECIPROCAL_SCALAR_PAIR(frsqrts)};
	static constexpr std::array vector{
		UML_RECIPROCAL_VECTOR_PAIR(frecpe),
		UML_RECIPROCAL_VECTOR_PAIR(frecps),
		std::array<Gadget, 5>{},
		UML_RECIPROCAL_VECTOR_PAIR(frsqrte),
		UML_RECIPROCAL_VECTOR_PAIR(frsqrts)};
#undef UML_RECIPROCAL_VECTOR_PAIR
#undef UML_RECIPROCAL_SCALAR_PAIR
#undef UML_RECIPROCAL_VECTOR
#undef UML_RECIPROCAL_SCALAR
	const unsigned operation = static_cast<unsigned>(decoded.operation);
	if (decoded.scalar) {
		const unsigned shape = UML_FLOATING_SCALAR_SHAPE(decoded);
		return scalar[operation][shape];
	}
	const unsigned shape = UML_FLOATING_VECTOR_SHAPE(decoded);
	return vector[operation][shape];
}

[[nodiscard]] Gadget floating_complex_gadget(
	const detail::FloatingComplex &decoded) noexcept
{
	const unsigned shape = UML_FLOATING_VECTOR_SHAPE(decoded);
#define UML_COMPLEX_ADD(shape) std::array<Gadget, 2>{ \
	uml_arm64_gadget_floating_complex_fcadd_##shape##_90, \
	uml_arm64_gadget_floating_complex_fcadd_##shape##_270}
	static constexpr std::array add{
		UML_COMPLEX_ADD(4h), UML_COMPLEX_ADD(8h),
		UML_COMPLEX_ADD(2s), UML_COMPLEX_ADD(4s),
		UML_COMPLEX_ADD(2d)};
#undef UML_COMPLEX_ADD
#define UML_COMPLEX_ROTATIONS(name) std::array<Gadget, 4>{ \
	uml_arm64_gadget_floating_complex_##name##_0, \
	uml_arm64_gadget_floating_complex_##name##_90, \
	uml_arm64_gadget_floating_complex_##name##_180, \
	uml_arm64_gadget_floating_complex_##name##_270}
	static constexpr std::array multiply{
		UML_COMPLEX_ROTATIONS(fcmla_4h),
		UML_COMPLEX_ROTATIONS(fcmla_8h),
		UML_COMPLEX_ROTATIONS(fcmla_2s),
		UML_COMPLEX_ROTATIONS(fcmla_4s),
		UML_COMPLEX_ROTATIONS(fcmla_2d)};
	static constexpr std::array half64{
		UML_COMPLEX_ROTATIONS(fcmla_element_4h_0),
		UML_COMPLEX_ROTATIONS(fcmla_element_4h_1)};
	static constexpr std::array half128{
		UML_COMPLEX_ROTATIONS(fcmla_element_8h_0),
		UML_COMPLEX_ROTATIONS(fcmla_element_8h_1),
		UML_COMPLEX_ROTATIONS(fcmla_element_8h_2),
		UML_COMPLEX_ROTATIONS(fcmla_element_8h_3)};
	static constexpr std::array single128{
		UML_COMPLEX_ROTATIONS(fcmla_element_4s_0),
		UML_COMPLEX_ROTATIONS(fcmla_element_4s_1)};
#undef UML_COMPLEX_ROTATIONS
	if (decoded.operation == detail::FloatingComplexOperation::add)
		return add[shape][decoded.rotation == 3U ? 1U : 0U];
	if (!decoded.by_element)
		return multiply[shape][decoded.rotation];
	if (decoded.width == 32U)
		return single128[decoded.index][decoded.rotation];
	return (decoded.vector_bytes == 16U ? half128[decoded.index] :
		half64[decoded.index])[decoded.rotation];
}

[[nodiscard]] std::uintptr_t floating_complex_operand(
	std::uint32_t instruction,
	const detail::FloatingComplex &decoded) noexcept
{
	return vector_operand_offset(instruction & 31U) |
		(static_cast<std::uintptr_t>(vector_operand_offset(
			(instruction >> 5) & 31U)) << 16U) |
		(static_cast<std::uintptr_t>(vector_operand_offset(
			decoded.second_register)) << 32U);
}

[[nodiscard]] std::uint64_t expand_vfp_immediate(
	std::uint8_t immediate, unsigned bits) noexcept
{
	const unsigned exponent_bits = bits == 16U ? 5U : bits == 32U ? 8U : 11U;
	const unsigned fraction_bits = bits - exponent_bits - 1U;
	const auto sign = static_cast<std::uint64_t>(immediate >> 7) << (bits - 1U);
	const auto control = static_cast<std::uint64_t>((immediate >> 6) & 1U);
	const auto exponent_top = (control ^ 1U) << (bits - 2U);
	const auto exponent_repeat = control == 0U ? 0U :
		((std::uint64_t{1} << (exponent_bits - 3U)) - 1U) <<
			(fraction_bits + 2U);
	const auto payload = static_cast<std::uint64_t>(immediate & 0x3fU) <<
		(fraction_bits - 4U);
	return sign | exponent_top | exponent_repeat | payload;
}

[[nodiscard]] Gadget simd_floating_sign_gadget(bool negate, bool half,
	bool double_precision, bool q) noexcept
{
	if (half) {
		if (negate)
			return q ? uml_arm64_gadget_simd_floating_fneg_8h :
				uml_arm64_gadget_simd_floating_fneg_4h;
		return q ? uml_arm64_gadget_simd_floating_fabs_8h :
			uml_arm64_gadget_simd_floating_fabs_4h;
	}
	if (double_precision)
		return q ? (negate ? uml_arm64_gadget_simd_floating_fneg_2d :
			uml_arm64_gadget_simd_floating_fabs_2d) : nullptr;
	if (negate)
		return q ? uml_arm64_gadget_simd_floating_fneg_4s :
			uml_arm64_gadget_simd_floating_fneg_2s;
	return q ? uml_arm64_gadget_simd_floating_fabs_4s :
		uml_arm64_gadget_simd_floating_fabs_2s;
}

[[nodiscard]] Gadget floating_multiply_add_gadget(
	bool is_double, bool negate, bool subtract) noexcept
{
	if (!negate && !subtract)
		return is_double ? uml_arm64_gadget_floating_madd_double :
			uml_arm64_gadget_floating_madd_single;
	if (!negate)
		return is_double ? uml_arm64_gadget_floating_msub_double :
			uml_arm64_gadget_floating_msub_single;
	if (!subtract)
		return is_double ? uml_arm64_gadget_floating_nmadd_double :
			uml_arm64_gadget_floating_nmadd_single;
	return is_double ? uml_arm64_gadget_floating_nmsub_double :
		uml_arm64_gadget_floating_nmsub_single;
}

/* fmla/fmls Vd, Vn, Vm: rows are 4h 8h 2s 4s 2d per operation. */
[[nodiscard]] Gadget simd_fused_inplace_gadget(bool subtract,
	unsigned int arrangement, unsigned int vector_index) noexcept
{
	const auto row = static_cast<unsigned>(subtract) * 5U + arrangement;
	return uml_arm64_gadget_simd_fused_inplace_table[row * 32U + vector_index];
}

/* A vector FMLA/FMLS accumulates into Vd, so its destination is also its
 * third source and every one takes the entry that bakes Vd. */
[[nodiscard]] Gadget fused_floating_arithmetic_gadget(
	const detail::FusedFloatingArithmetic &decoded,
	unsigned int destination) noexcept
{
	if (decoded.scalar) {
#define UML_SCALAR_FUSED_WIDTH(name) \
		(decoded.width == 16U ? uml_arm64_gadget_floating_##name##_half : \
		 decoded.width == 32U ? uml_arm64_gadget_floating_##name##_single : \
		 uml_arm64_gadget_floating_##name##_double)
		if (!decoded.negate && !decoded.subtract)
			return UML_SCALAR_FUSED_WIDTH(madd);
		if (!decoded.negate)
			return UML_SCALAR_FUSED_WIDTH(msub);
		if (!decoded.subtract)
			return UML_SCALAR_FUSED_WIDTH(nmadd);
		return UML_SCALAR_FUSED_WIDTH(nmsub);
#undef UML_SCALAR_FUSED_WIDTH
	}
	const unsigned shape = UML_FLOATING_VECTOR_SHAPE(decoded);
	return simd_fused_inplace_gadget(decoded.subtract, shape, destination);
}

#undef UML_FLOATING_SCALAR_SHAPE
#undef UML_FLOATING_VECTOR_SHAPE

[[nodiscard]] Gadget simd_bitwise_gadget(unsigned operation, bool q) noexcept
{
	const unsigned shape = q ? 1U : 0U;
#define UML_SIMD_BITWISE(name) std::array<Gadget, 2>{ \
	uml_arm64_gadget_##name##_8b, uml_arm64_gadget_##name##_16b}
	static constexpr auto bit_and = UML_SIMD_BITWISE(simd_and);
	static constexpr auto bit_clear = UML_SIMD_BITWISE(simd_bic);
	static constexpr auto bit_or = UML_SIMD_BITWISE(simd_orr);
	static constexpr auto or_not = UML_SIMD_BITWISE(simd_orn);
	static constexpr auto bit_xor = UML_SIMD_BITWISE(simd_eor);
	static constexpr auto select = UML_SIMD_BITWISE(simd_bsl);
	static constexpr auto insert_true = UML_SIMD_BITWISE(simd_bit);
	static constexpr auto insert_false = UML_SIMD_BITWISE(simd_bif);
#undef UML_SIMD_BITWISE
	switch (operation) {
	case 0: return bit_and[shape];
	case 1: return bit_clear[shape];
	case 2: return bit_or[shape];
	case 3: return or_not[shape];
	case 4: return bit_xor[shape];
	case 5: return select[shape];
	case 6: return insert_true[shape];
	default: return insert_false[shape];
	}
}

[[nodiscard]] Gadget simd_move_gadget(bool q) noexcept
{
	return q ? uml_arm64_gadget_simd_mov_16b :
		uml_arm64_gadget_simd_mov_8b;
}

[[nodiscard]] Gadget simd_modified_immediate_gadget(
	unsigned action, bool q) noexcept
{
	const unsigned shape = q ? 1U : 0U;
	static constexpr std::array replace{
		uml_arm64_gadget_simd_immediate_replace_8b,
		uml_arm64_gadget_simd_immediate_replace_16b};
	static constexpr std::array bit_or{
		uml_arm64_gadget_simd_immediate_orr_8b,
		uml_arm64_gadget_simd_immediate_orr_16b};
	static constexpr std::array bit_clear{
		uml_arm64_gadget_simd_immediate_bic_8b,
		uml_arm64_gadget_simd_immediate_bic_16b};
	switch (action) {
	case 0: return replace[shape];
	case 1: return bit_or[shape];
	default: return bit_clear[shape];
	}
}

[[nodiscard]] Gadget simd_duplicate_element_gadget(
	bool scalar, bool q, unsigned element_shift) noexcept
{
	static constexpr std::array<Gadget, 4> scalar_gadgets{
		uml_arm64_gadget_simd_duplicate_element_b,
		uml_arm64_gadget_simd_duplicate_element_h,
		uml_arm64_gadget_simd_duplicate_element_s,
		uml_arm64_gadget_simd_duplicate_element_d};
	static constexpr std::array<Gadget, 4> narrow_gadgets{
		uml_arm64_gadget_simd_duplicate_element_8b,
		uml_arm64_gadget_simd_duplicate_element_4h,
		uml_arm64_gadget_simd_duplicate_element_2s,
		nullptr};
	static constexpr std::array<Gadget, 4> wide_gadgets{
		uml_arm64_gadget_simd_duplicate_element_16b,
		uml_arm64_gadget_simd_duplicate_element_8h,
		uml_arm64_gadget_simd_duplicate_element_4s,
		uml_arm64_gadget_simd_duplicate_element_2d};
	if (element_shift > 3U)
		return nullptr;
	return scalar ? scalar_gadgets[element_shift] :
		(q ? wide_gadgets[element_shift] : narrow_gadgets[element_shift]);
}

[[nodiscard]] Gadget simd_duplicate_general_gadget(
	bool q, unsigned element_shift, bool zero_source) noexcept
{
	if (element_shift > 3U || (!q && element_shift == 3U))
		return nullptr;
	if (zero_source)
		return q ? uml_arm64_gadget_simd_duplicate_general_zero_16b :
			uml_arm64_gadget_simd_duplicate_general_zero_8b;
	static constexpr std::array<Gadget, 4> narrow_gadgets{
		uml_arm64_gadget_simd_duplicate_general_8b,
		uml_arm64_gadget_simd_duplicate_general_4h,
		uml_arm64_gadget_simd_duplicate_general_2s,
		nullptr};
	static constexpr std::array<Gadget, 4> wide_gadgets{
		uml_arm64_gadget_simd_duplicate_general_16b,
		uml_arm64_gadget_simd_duplicate_general_8h,
		uml_arm64_gadget_simd_duplicate_general_4s,
		uml_arm64_gadget_simd_duplicate_general_2d};
	return (q ? wide_gadgets : narrow_gadgets)[element_shift];
}

[[nodiscard]] Gadget simd_insert_general_gadget(
	unsigned element_shift, bool zero_source) noexcept
{
	if (element_shift > 3U)
		return nullptr;
	static constexpr std::array<Gadget, 4> regular{
		uml_arm64_gadget_simd_insert_general_b,
		uml_arm64_gadget_simd_insert_general_h,
		uml_arm64_gadget_simd_insert_general_s,
		uml_arm64_gadget_simd_insert_general_d};
	static constexpr std::array<Gadget, 4> zero{
		uml_arm64_gadget_simd_insert_general_zero_b,
		uml_arm64_gadget_simd_insert_general_zero_h,
		uml_arm64_gadget_simd_insert_general_zero_s,
		uml_arm64_gadget_simd_insert_general_zero_d};
	return (zero_source ? zero : regular)[element_shift];
}

[[nodiscard]] Gadget simd_insert_element_gadget(
	unsigned element_shift) noexcept
{
	static constexpr std::array<Gadget, 4> gadgets{
		uml_arm64_gadget_simd_insert_element_b,
		uml_arm64_gadget_simd_insert_element_h,
		uml_arm64_gadget_simd_insert_element_s,
		uml_arm64_gadget_simd_insert_element_d};
	return element_shift < gadgets.size() ? gadgets[element_shift] : nullptr;
}

[[nodiscard]] Gadget simd_lane_to_gpr_gadget(bool signed_move,
	bool is_64_bit, unsigned element_shift, bool discard) noexcept
{
	Gadget gadget = nullptr;
	if (signed_move) {
		if (!is_64_bit && element_shift == 0U)
			gadget = uml_arm64_gadget_simd_lane_to_gpr_smov_w_b;
		else if (!is_64_bit && element_shift == 1U)
			gadget = uml_arm64_gadget_simd_lane_to_gpr_smov_w_h;
		else if (is_64_bit && element_shift == 0U)
			gadget = uml_arm64_gadget_simd_lane_to_gpr_smov_x_b;
		else if (is_64_bit && element_shift == 1U)
			gadget = uml_arm64_gadget_simd_lane_to_gpr_smov_x_h;
		else if (is_64_bit && element_shift == 2U)
			gadget = uml_arm64_gadget_simd_lane_to_gpr_smov_x_s;
	} else if (!is_64_bit && element_shift == 0U) {
		gadget = uml_arm64_gadget_simd_lane_to_gpr_umov_w_b;
	} else if (!is_64_bit && element_shift == 1U) {
		gadget = uml_arm64_gadget_simd_lane_to_gpr_umov_w_h;
	} else if (!is_64_bit && element_shift == 2U) {
		gadget = uml_arm64_gadget_simd_lane_to_gpr_umov_w_s;
	} else if (is_64_bit && element_shift == 3U) {
		gadget = uml_arm64_gadget_simd_lane_to_gpr_umov_x_d;
	}
	return gadget != nullptr && discard ?
		uml_arm64_gadget_simd_lane_to_gpr_discard : gadget;
}

[[nodiscard]] std::optional<SimdImmediateShift> decode_simd_immediate_shift(
	std::uint32_t instruction, bool left) noexcept
{
	const unsigned encoded = (instruction >> 16) & 0x7fU;
	if (encoded < 8U)
		return std::nullopt;
	const unsigned lane_bits = std::bit_floor(encoded);
	if (lane_bits > 64U)
		return std::nullopt;
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	const bool q = (instruction & 0x4000'0000U) != 0;
	if ((scalar && lane_bits != 64U) ||
	    (!scalar && !q && lane_bits == 64U))
		return std::nullopt;
	const unsigned shift = left ? encoded - lane_bits :
		2U * lane_bits - encoded;
	if ((left && shift >= lane_bits) ||
	    (!left && (shift == 0U || shift > lane_bits)))
		return std::nullopt;
	const unsigned size = std::countr_zero(lane_bits / 8U);
	return SimdImmediateShift{lane_bits, shift,
		scalar ? 7U : q ? size + 3U : size};
}

[[nodiscard]] Gadget simd_shift_right_immediate_gadget(
	unsigned opcode, bool alternate, unsigned shape) noexcept
{
#define UML_SIMD_SHIFT_ARRAY(name) std::array<Gadget, 8>{ \
	uml_arm64_gadget_simd_shift_right_##name##_8b, \
	uml_arm64_gadget_simd_shift_right_##name##_4h, \
	uml_arm64_gadget_simd_shift_right_##name##_2s, \
	uml_arm64_gadget_simd_shift_right_##name##_16b, \
	uml_arm64_gadget_simd_shift_right_##name##_8h, \
	uml_arm64_gadget_simd_shift_right_##name##_4s, \
	uml_arm64_gadget_simd_shift_right_##name##_2d, \
	uml_arm64_gadget_simd_shift_right_##name##_d}
	static constexpr auto sshr = UML_SIMD_SHIFT_ARRAY(sshr);
	static constexpr auto ushr = UML_SIMD_SHIFT_ARRAY(ushr);
	static constexpr auto ssra = UML_SIMD_SHIFT_ARRAY(ssra);
	static constexpr auto usra = UML_SIMD_SHIFT_ARRAY(usra);
	static constexpr auto srshr = UML_SIMD_SHIFT_ARRAY(srshr);
	static constexpr auto urshr = UML_SIMD_SHIFT_ARRAY(urshr);
	static constexpr auto srsra = UML_SIMD_SHIFT_ARRAY(srsra);
	static constexpr auto ursra = UML_SIMD_SHIFT_ARRAY(ursra);
	static constexpr auto sri = UML_SIMD_SHIFT_ARRAY(sri);
#undef UML_SIMD_SHIFT_ARRAY
	if (shape >= 8U)
		return nullptr;
	switch (opcode) {
	case 0: return (alternate ? ushr : sshr)[shape];
	case 2: return (alternate ? usra : ssra)[shape];
	case 4: return (alternate ? urshr : srshr)[shape];
	case 6: return (alternate ? ursra : srsra)[shape];
	case 8: return alternate ? sri[shape] : nullptr;
	default: return nullptr;
	}
}

[[nodiscard]] Gadget simd_shift_left_immediate_gadget(
	bool insert, unsigned shape) noexcept
{
#define UML_SIMD_SHIFT_ARRAY(name) std::array<Gadget, 8>{ \
	uml_arm64_gadget_simd_shift_left_##name##_8b, \
	uml_arm64_gadget_simd_shift_left_##name##_4h, \
	uml_arm64_gadget_simd_shift_left_##name##_2s, \
	uml_arm64_gadget_simd_shift_left_##name##_16b, \
	uml_arm64_gadget_simd_shift_left_##name##_8h, \
	uml_arm64_gadget_simd_shift_left_##name##_4s, \
	uml_arm64_gadget_simd_shift_left_##name##_2d, \
	uml_arm64_gadget_simd_shift_left_##name##_d}
	static constexpr auto shl = UML_SIMD_SHIFT_ARRAY(shl);
	static constexpr auto sli = UML_SIMD_SHIFT_ARRAY(sli);
#undef UML_SIMD_SHIFT_ARRAY
	return shape < 8U ? (insert ? sli : shl)[shape] : nullptr;
}

[[nodiscard]] std::optional<SimdImmediateShift>
decode_simd_saturating_shift_left(std::uint32_t instruction) noexcept
{
	const unsigned encoded = (instruction >> 16) & 0x7fU;
	if (encoded < 8U)
		return std::nullopt;
	const unsigned lane_bits = std::bit_floor(encoded);
	if (lane_bits > 64U)
		return std::nullopt;
	const bool scalar = (instruction & 0x1000'0000U) != 0;
	const bool q = (instruction & 0x4000'0000U) != 0;
	if (!scalar && !q && lane_bits == 64U)
		return std::nullopt;
	const unsigned shift = encoded - lane_bits;
	const unsigned size = std::countr_zero(lane_bits / 8U);
	return SimdImmediateShift{lane_bits, shift,
		scalar ? size + 7U : q ? size + 3U : size};
}

[[nodiscard]] Gadget simd_saturating_shift_left_gadget(
	SimdSaturatingShiftLeft operation, unsigned shape, unsigned shift) noexcept
{
#define UML_SIMD_SAT_SHIFT_ARRAY(name) std::array<Gadget, 11>{ \
	uml_arm64_gadget_simd_shift_left_##name##_8b, \
	uml_arm64_gadget_simd_shift_left_##name##_4h, \
	uml_arm64_gadget_simd_shift_left_##name##_2s, \
	uml_arm64_gadget_simd_shift_left_##name##_16b, \
	uml_arm64_gadget_simd_shift_left_##name##_8h, \
	uml_arm64_gadget_simd_shift_left_##name##_4s, \
	uml_arm64_gadget_simd_shift_left_##name##_2d, \
	uml_arm64_gadget_simd_shift_left_##name##_b, \
	uml_arm64_gadget_simd_shift_left_##name##_h, \
	uml_arm64_gadget_simd_shift_left_##name##_s, \
	uml_arm64_gadget_simd_shift_left_##name##_d}
	static constexpr auto signed_shift = UML_SIMD_SAT_SHIFT_ARRAY(sqshl);
	static constexpr auto unsigned_shift = UML_SIMD_SAT_SHIFT_ARRAY(uqshl);
#undef UML_SIMD_SAT_SHIFT_ARRAY
	if (shape >= 11U)
		return nullptr;
	switch (operation) {
	case SimdSaturatingShiftLeft::signed_to_signed:
		return signed_shift[shape];
	case SimdSaturatingShiftLeft::unsigned_to_unsigned:
		return unsigned_shift[shape];
	case SimdSaturatingShiftLeft::signed_to_unsigned:
		break;
	}
#define UML_SIMD_SQSHLU_ITEM(shape_name, amount) \
	uml_arm64_gadget_simd_shift_left_sqshlu_##shape_name##_##amount,
#define UML_SIMD_SQSHLU_ARRAY(bits, shape_name) \
	static constexpr std::array<Gadget, bits> sqshlu_##shape_name{ \
		UML_SIMD_FOR_EACH_SHIFT_##bits(UML_SIMD_SQSHLU_ITEM, shape_name)}
	UML_SIMD_SQSHLU_ARRAY(8, 8b);
	UML_SIMD_SQSHLU_ARRAY(16, 4h);
	UML_SIMD_SQSHLU_ARRAY(32, 2s);
	UML_SIMD_SQSHLU_ARRAY(8, 16b);
	UML_SIMD_SQSHLU_ARRAY(16, 8h);
	UML_SIMD_SQSHLU_ARRAY(32, 4s);
	UML_SIMD_SQSHLU_ARRAY(64, 2d);
	UML_SIMD_SQSHLU_ARRAY(8, b);
	UML_SIMD_SQSHLU_ARRAY(16, h);
	UML_SIMD_SQSHLU_ARRAY(32, s);
	UML_SIMD_SQSHLU_ARRAY(64, d);
#undef UML_SIMD_SQSHLU_ARRAY
#undef UML_SIMD_SQSHLU_ITEM
	switch (shape) {
	case 0: return sqshlu_8b[shift];
	case 1: return sqshlu_4h[shift];
	case 2: return sqshlu_2s[shift];
	case 3: return sqshlu_16b[shift];
	case 4: return sqshlu_8h[shift];
	case 5: return sqshlu_4s[shift];
	case 6: return sqshlu_2d[shift];
	case 7: return sqshlu_b[shift];
	case 8: return sqshlu_h[shift];
	case 9: return sqshlu_s[shift];
	case 10: return sqshlu_d[shift];
	default: return nullptr;
	}
}

#undef UML_SIMD_FOR_EACH_SHIFT_64
#undef UML_SIMD_FOR_EACH_SHIFT_32
#undef UML_SIMD_FOR_EACH_SHIFT_16
#undef UML_SIMD_FOR_EACH_SHIFT_8

[[nodiscard]] Gadget simd_shift_narrow_gadget(
	unsigned source_bits, bool high, bool rounding) noexcept
{
	static constexpr std::array<Gadget, 12> gadgets{
		uml_arm64_gadget_simd_shrn_8h_to_8b,
		uml_arm64_gadget_simd_shrn_4s_to_4h,
		uml_arm64_gadget_simd_shrn_2d_to_2s,
		uml_arm64_gadget_simd_shrn2_8h_to_8b,
		uml_arm64_gadget_simd_shrn2_4s_to_4h,
		uml_arm64_gadget_simd_shrn2_2d_to_2s,
		uml_arm64_gadget_simd_rshrn_8h_to_8b,
		uml_arm64_gadget_simd_rshrn_4s_to_4h,
		uml_arm64_gadget_simd_rshrn_2d_to_2s,
		uml_arm64_gadget_simd_rshrn2_8h_to_8b,
		uml_arm64_gadget_simd_rshrn2_4s_to_4h,
		uml_arm64_gadget_simd_rshrn2_2d_to_2s};
	if (source_bits != 16U && source_bits != 32U && source_bits != 64U)
		return nullptr;
	const unsigned source = std::countr_zero(source_bits) - 4U;
	return gadgets[source + (high ? 3U : 0U) + (rounding ? 6U : 0U)];
}

[[nodiscard]] Gadget simd_saturating_narrow_gadget(unsigned opcode,
	bool alternate, unsigned source_bits, bool scalar, bool high) noexcept
{
	if (source_bits != 16U && source_bits != 32U && source_bits != 64U)
		return nullptr;
	if ((opcode == 16U || opcode == 17U) && !alternate)
		return nullptr;
	const unsigned shape = (std::countr_zero(source_bits) - 4U) * 3U +
		(scalar ? 2U : high ? 1U : 0U);
#define UML_SIMD_SAT_NARROW_ARRAY(name) std::array<Gadget, 9>{ \
	uml_arm64_gadget_simd_##name##_h_b_low, \
	uml_arm64_gadget_simd_##name##_h_b_high, \
	uml_arm64_gadget_simd_##name##_h_b_scalar, \
	uml_arm64_gadget_simd_##name##_s_h_low, \
	uml_arm64_gadget_simd_##name##_s_h_high, \
	uml_arm64_gadget_simd_##name##_s_h_scalar, \
	uml_arm64_gadget_simd_##name##_d_s_low, \
	uml_arm64_gadget_simd_##name##_d_s_high, \
	uml_arm64_gadget_simd_##name##_d_s_scalar}
	static constexpr auto sqshrn = UML_SIMD_SAT_NARROW_ARRAY(sqshrn);
	static constexpr auto sqrshrn = UML_SIMD_SAT_NARROW_ARRAY(sqrshrn);
	static constexpr auto sqshrun = UML_SIMD_SAT_NARROW_ARRAY(sqshrun);
	static constexpr auto sqrshrun = UML_SIMD_SAT_NARROW_ARRAY(sqrshrun);
	static constexpr auto uqshrn = UML_SIMD_SAT_NARROW_ARRAY(uqshrn);
	static constexpr auto uqrshrn = UML_SIMD_SAT_NARROW_ARRAY(uqrshrn);
#undef UML_SIMD_SAT_NARROW_ARRAY
	switch (opcode) {
	case 16: return sqshrun[shape];
	case 17: return sqrshrun[shape];
	case 18: return (alternate ? uqshrn : sqshrn)[shape];
	case 19: return (alternate ? uqrshrn : sqrshrn)[shape];
	default: return nullptr;
	}
}

[[nodiscard]] Gadget simd_shift_left_long_gadget(
	unsigned source_bits, bool high, bool unsigned_source) noexcept
{
	if (source_bits != 8U && source_bits != 16U && source_bits != 32U)
		return nullptr;
	const unsigned shape = std::countr_zero(source_bits) - 3U;
	static constexpr std::array<Gadget, 3> sshll{
		uml_arm64_gadget_simd_sshll_8h_8b,
		uml_arm64_gadget_simd_sshll_4s_4h,
		uml_arm64_gadget_simd_sshll_2d_2s};
	static constexpr std::array<Gadget, 3> sshll2{
		uml_arm64_gadget_simd_sshll2_8h_8b,
		uml_arm64_gadget_simd_sshll2_4s_4h,
		uml_arm64_gadget_simd_sshll2_2d_2s};
	static constexpr std::array<Gadget, 3> ushll{
		uml_arm64_gadget_simd_ushll_8h_8b,
		uml_arm64_gadget_simd_ushll_4s_4h,
		uml_arm64_gadget_simd_ushll_2d_2s};
	static constexpr std::array<Gadget, 3> ushll2{
		uml_arm64_gadget_simd_ushll2_8h_8b,
		uml_arm64_gadget_simd_ushll2_4s_4h,
		uml_arm64_gadget_simd_ushll2_2d_2s};
	return unsigned_source ? (high ? ushll2 : ushll)[shape] :
		(high ? sshll2 : sshll)[shape];
}

#define UML_SIMD_INDEXED_ITEMS_4(name, shape) \
	uml_arm64_gadget_simd_indexed_##name##_##shape##_0, \
	uml_arm64_gadget_simd_indexed_##name##_##shape##_1, \
	uml_arm64_gadget_simd_indexed_##name##_##shape##_2, \
	uml_arm64_gadget_simd_indexed_##name##_##shape##_3
#define UML_SIMD_INDEXED_ITEMS_8(name, shape) \
	UML_SIMD_INDEXED_ITEMS_4(name, shape), \
	uml_arm64_gadget_simd_indexed_##name##_##shape##_4, \
	uml_arm64_gadget_simd_indexed_##name##_##shape##_5, \
	uml_arm64_gadget_simd_indexed_##name##_##shape##_6, \
	uml_arm64_gadget_simd_indexed_##name##_##shape##_7
#define UML_SIMD_INDEXED_ARRAY_4(name, shape) \
	std::array<Gadget, 8>{UML_SIMD_INDEXED_ITEMS_4(name, shape), \
		nullptr, nullptr, nullptr, nullptr}
#define UML_SIMD_INDEXED_ARRAY_2(name, shape) \
	std::array<Gadget, 8>{ \
		uml_arm64_gadget_simd_indexed_##name##_##shape##_0, \
		uml_arm64_gadget_simd_indexed_##name##_##shape##_1, \
		nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}
#define UML_SIMD_INDEXED_ARRAY_8(name, shape) \
	std::array<Gadget, 8>{UML_SIMD_INDEXED_ITEMS_8(name, shape)}

[[nodiscard]] Gadget simd_indexed_same_gadget(unsigned operation,
	unsigned size, bool q, unsigned index) noexcept
{
#define UML_SIMD_INDEXED_SAME_FAMILY(name) \
	std::array<std::array<Gadget, 8>, 4>{ \
		UML_SIMD_INDEXED_ARRAY_8(name, 4h), \
		UML_SIMD_INDEXED_ARRAY_8(name, 8h), \
		UML_SIMD_INDEXED_ARRAY_4(name, 2s), \
		UML_SIMD_INDEXED_ARRAY_4(name, 4s)}
	static constexpr std::array families{
		UML_SIMD_INDEXED_SAME_FAMILY(mla),
		UML_SIMD_INDEXED_SAME_FAMILY(mls),
		UML_SIMD_INDEXED_SAME_FAMILY(mul)};
#undef UML_SIMD_INDEXED_SAME_FAMILY
	const unsigned family = operation / 4U;
	const unsigned shape = (size == 2U ? 2U : 0U) + (q ? 1U : 0U);
	return families[family][shape][index];
}

[[nodiscard]] Gadget simd_indexed_long_gadget(unsigned operation,
	bool unsigned_group, unsigned size, bool high, unsigned index) noexcept
{
#define UML_SIMD_INDEXED_LONG_FAMILY(name) \
	std::array<std::array<Gadget, 8>, 4>{ \
		UML_SIMD_INDEXED_ARRAY_8(name, h_low), \
		UML_SIMD_INDEXED_ARRAY_8(name, h_high), \
		UML_SIMD_INDEXED_ARRAY_4(name, s_low), \
		UML_SIMD_INDEXED_ARRAY_4(name, s_high)}
	static constexpr std::array families{
		UML_SIMD_INDEXED_LONG_FAMILY(smlal),
		UML_SIMD_INDEXED_LONG_FAMILY(smlsl),
		UML_SIMD_INDEXED_LONG_FAMILY(smull),
		UML_SIMD_INDEXED_LONG_FAMILY(umlal),
		UML_SIMD_INDEXED_LONG_FAMILY(umlsl),
		UML_SIMD_INDEXED_LONG_FAMILY(umull)};
#undef UML_SIMD_INDEXED_LONG_FAMILY
	const unsigned family = (unsigned_group ? 3U : 0U) +
		(operation - 2U) / 4U;
	const unsigned shape = (size == 2U ? 2U : 0U) + (high ? 1U : 0U);
	return families[family][shape][index];
}

#define UML_SIMD_INDEXED_SATURATING_FAMILY(name) \
	std::array<std::array<Gadget, 8>, 6>{ \
		UML_SIMD_INDEXED_ARRAY_8(name, h_low), \
		UML_SIMD_INDEXED_ARRAY_8(name, h_high), \
		UML_SIMD_INDEXED_ARRAY_8(name, h_scalar), \
		UML_SIMD_INDEXED_ARRAY_4(name, s_low), \
		UML_SIMD_INDEXED_ARRAY_4(name, s_high), \
		UML_SIMD_INDEXED_ARRAY_4(name, s_scalar)}

[[nodiscard]] Gadget simd_indexed_saturating_long_gadget(unsigned operation,
	unsigned size, bool scalar, bool high, unsigned index) noexcept
{
	static constexpr std::array families{
		UML_SIMD_INDEXED_SATURATING_FAMILY(sqdmlal),
		UML_SIMD_INDEXED_SATURATING_FAMILY(sqdmlsl),
		UML_SIMD_INDEXED_SATURATING_FAMILY(sqdmull)};
	const unsigned family = (operation - 3U) / 4U;
	const unsigned shape = (size == 2U ? 3U : 0U) +
		(scalar ? 2U : high ? 1U : 0U);
	return families[family][shape][index];
}

[[nodiscard]] Gadget simd_indexed_saturating_high_gadget(unsigned operation,
	unsigned size, bool scalar, bool wide, unsigned index) noexcept
{
	static constexpr std::array families{
		UML_SIMD_INDEXED_SATURATING_FAMILY(sqdmulh),
		UML_SIMD_INDEXED_SATURATING_FAMILY(sqrdmulh)};
	const unsigned shape = (size == 2U ? 3U : 0U) +
		(scalar ? 2U : wide ? 1U : 0U);
	return families[operation - 12U][shape][index];
}

[[nodiscard]] Gadget simd_indexed_saturating_accumulate_high_gadget(
	unsigned operation, unsigned size, bool scalar, bool wide,
	unsigned index) noexcept
{
	static constexpr std::array families{
		UML_SIMD_INDEXED_SATURATING_FAMILY(sqrdmlah),
		UML_SIMD_INDEXED_SATURATING_FAMILY(sqrdmlsh)};
	const unsigned shape = (size == 2U ? 3U : 0U) +
		(scalar ? 2U : wide ? 1U : 0U);
	return families[(operation - 13U) / 2U][shape][index];
}

#undef UML_SIMD_INDEXED_SATURATING_FAMILY

[[nodiscard]] Gadget simd_indexed_integer_gadget(unsigned operation,
	bool unsigned_group, bool scalar, bool q, unsigned size,
	unsigned index) noexcept
{
	if (size != 1U && size != 2U)
		return nullptr;
	switch (operation) {
	case 0: case 4:
		return unsigned_group && !scalar ? simd_indexed_same_gadget(
			operation, size, q, index) : nullptr;
	case 8:
		return !unsigned_group && !scalar ? simd_indexed_same_gadget(
			operation, size, q, index) : nullptr;
	case 2: case 6: case 10:
		return !scalar ? simd_indexed_long_gadget(operation, unsigned_group,
			size, q, index) : nullptr;
	case 3: case 7: case 11:
		return !unsigned_group ? simd_indexed_saturating_long_gadget(
			operation, size, scalar, q, index) : nullptr;
	case 12:
		return !unsigned_group ? simd_indexed_saturating_high_gadget(
			operation, size, scalar, q, index) : nullptr;
	case 13:
		return unsigned_group ?
			simd_indexed_saturating_accumulate_high_gadget(
				operation, size, scalar, q, index) :
			simd_indexed_saturating_high_gadget(
				operation, size, scalar, q, index);
	case 15:
		return unsigned_group ?
			simd_indexed_saturating_accumulate_high_gadget(
				operation, size, scalar, q, index) : nullptr;
	default:
		return nullptr;
	}
}

[[nodiscard]] Gadget simd_indexed_floating_gadget(unsigned operation,
	bool extended, bool scalar, bool q, unsigned size, unsigned index) noexcept
{
#define UML_SIMD_INDEXED_FLOATING_FAMILY(name) \
	std::array<std::array<Gadget, 8>, 8>{ \
		UML_SIMD_INDEXED_ARRAY_8(name, h), \
		UML_SIMD_INDEXED_ARRAY_8(name, 4h), \
		UML_SIMD_INDEXED_ARRAY_8(name, 8h), \
		UML_SIMD_INDEXED_ARRAY_4(name, s), \
		UML_SIMD_INDEXED_ARRAY_4(name, 2s), \
		UML_SIMD_INDEXED_ARRAY_4(name, 4s), \
		UML_SIMD_INDEXED_ARRAY_2(name, d), \
		UML_SIMD_INDEXED_ARRAY_2(name, 2d)}
	static constexpr std::array native_families{
		UML_SIMD_INDEXED_FLOATING_FAMILY(fmla),
		UML_SIMD_INDEXED_FLOATING_FAMILY(fmls),
		UML_SIMD_INDEXED_FLOATING_FAMILY(fmul),
		UML_SIMD_INDEXED_FLOATING_FAMILY(fmulx)};
#undef UML_SIMD_INDEXED_FLOATING_FAMILY
	unsigned family;
	if (operation == 1U && !extended)
		family = 0U;
	else if (operation == 5U && !extended)
		family = 1U;
	else if (operation == 9U)
		family = extended ? 3U : 2U;
	else
		return nullptr;
	unsigned shape;
	if (size == 0U)
		shape = scalar ? 0U : q ? 2U : 1U;
	else if (size == 2U)
		shape = scalar ? 3U : q ? 5U : 4U;
	else if (size == 3U && (scalar || q))
		shape = scalar ? 6U : 7U;
	else
		return nullptr;
	return index < 8U ? native_families[family][shape][index] : nullptr;
}

[[nodiscard]] Gadget simd_fmulx_gadget(bool scalar, bool q, bool half,
	bool double_precision) noexcept
{
	static constexpr std::array<Gadget, 8> gadgets{
		uml_arm64_gadget_simd_fmulx_h,
		uml_arm64_gadget_simd_fmulx_4h,
		uml_arm64_gadget_simd_fmulx_8h,
		uml_arm64_gadget_simd_fmulx_s,
		uml_arm64_gadget_simd_fmulx_2s,
		uml_arm64_gadget_simd_fmulx_4s,
		uml_arm64_gadget_simd_fmulx_d,
		uml_arm64_gadget_simd_fmulx_2d};
	if (half)
		return gadgets[scalar ? 0U : q ? 2U : 1U];
	if (double_precision && !scalar && !q)
		return nullptr;
	return gadgets[double_precision ? (scalar ? 6U : 7U) :
		(scalar ? 3U : q ? 5U : 4U)];
}

[[nodiscard]] std::optional<SimdIndexedElement>
decode_simd_indexed_element(std::uint32_t instruction) noexcept
{
	const unsigned size = (instruction >> 22) & 3U;
	unsigned register_index = (instruction >> 16) & 15U;
	if (size == 1U)
		return SimdIndexedElement{register_index,
			((instruction >> 9) & 4U) |
			((instruction >> 20) & 1U) |
			((instruction >> 20) & 2U)};
	if (size == 2U) {
		register_index |= ((instruction >> 20) & 1U) << 4;
		return SimdIndexedElement{register_index,
			((instruction >> 10) & 2U) |
			((instruction >> 21) & 1U)};
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<SimdIndexedElement>
decode_simd_indexed_floating_element(std::uint32_t instruction) noexcept
{
	const unsigned size = (instruction >> 22) & 3U;
	unsigned register_index = (instruction >> 16) & 15U;
	if (size == 0U)
		return SimdIndexedElement{register_index,
			((instruction >> 9) & 4U) |
			((instruction >> 20) & 1U) |
			((instruction >> 20) & 2U)};
	if (size != 2U && size != 3U)
		return std::nullopt;
	register_index |= ((instruction >> 20) & 1U) << 4;
	if (size == 3U && (instruction & 0x0020'0000U) != 0)
		return std::nullopt;
	return SimdIndexedElement{register_index,
		size == 2U ? ((instruction >> 10) & 2U) |
			((instruction >> 21) & 1U) : (instruction >> 11) & 1U};
}

[[nodiscard]] SimdIndexedElement decode_simd_dot_indexed_element(
	std::uint32_t instruction) noexcept
{
	return SimdIndexedElement{
		((instruction >> 16) & 15U) | (((instruction >> 20) & 1U) << 4),
		((instruction >> 10) & 2U) | ((instruction >> 21) & 1U)};
}

[[nodiscard]] bool is_simd_indexed_integer_group(
	unsigned operation, bool unsigned_group, bool scalar) noexcept
{
	if (scalar)
		return unsigned_group ? operation == 13U || operation == 15U :
			operation == 3U || operation == 7U || operation == 11U ||
			operation == 12U || operation == 13U;
	if (unsigned_group)
		return operation == 0U || operation == 2U || operation == 4U ||
			operation == 6U || operation == 10U || operation == 13U ||
			operation == 15U;
	return operation == 2U || operation == 3U || operation == 6U ||
		operation == 7U || operation == 8U || operation == 10U ||
		operation == 11U || operation == 12U || operation == 13U;
}

[[nodiscard]] bool is_simd_indexed_scalar_reserved(
	unsigned operation, bool unsigned_group, unsigned size) noexcept
{
	const bool allocated_operation = unsigned_group ?
		operation == 9U || operation == 13U || operation == 15U :
		operation == 1U || operation == 3U || operation == 5U ||
		operation == 7U || operation == 9U || operation == 11U ||
		operation == 12U || operation == 13U;
	if (!allocated_operation)
		return true;
	const bool floating = operation == 1U || operation == 5U ||
		operation == 9U;
	return floating && size == 1U;
}

#undef UML_SIMD_INDEXED_ARRAY_8
#undef UML_SIMD_INDEXED_ARRAY_4
#undef UML_SIMD_INDEXED_ARRAY_2
#undef UML_SIMD_INDEXED_ITEMS_8
#undef UML_SIMD_INDEXED_ITEMS_4

[[nodiscard]] std::uint64_t repeat_simd_element(
	std::uint64_t element, unsigned lane_bits) noexcept
{
	std::uint64_t result = 0;
	for (unsigned bit = 0; bit < 64U; bit += lane_bits)
		result |= element << bit;
	return result;
}

[[nodiscard]] std::uint64_t simd_shift_insert_mask(
	unsigned lane_bits, unsigned shift, bool left) noexcept
{
	const auto lane_mask = lane_bits == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << lane_bits) - 1U;
	const auto element = left ? (std::uint64_t{1} << shift) - 1U :
		shift == lane_bits ? lane_mask : lane_mask ^ (lane_mask >> shift);
	return repeat_simd_element(element, lane_bits);
}

[[nodiscard]] Gadget simd_permute_gadget(
	unsigned opcode, bool q, unsigned size) noexcept
{
	if (size > 3U || (!q && size == 3U))
		return nullptr;
	const unsigned shape = q ? size + 3U : size;
#define UML_SIMD_PERMUTE(name) std::array<Gadget, 7>{ \
	uml_arm64_gadget_simd_permute_##name##_8b, \
	uml_arm64_gadget_simd_permute_##name##_4h, \
	uml_arm64_gadget_simd_permute_##name##_2s, \
	uml_arm64_gadget_simd_permute_##name##_16b, \
	uml_arm64_gadget_simd_permute_##name##_8h, \
	uml_arm64_gadget_simd_permute_##name##_4s, \
	uml_arm64_gadget_simd_permute_##name##_2d}
	static constexpr auto uzp1 = UML_SIMD_PERMUTE(uzp1);
	static constexpr auto trn1 = UML_SIMD_PERMUTE(trn1);
	static constexpr auto zip1 = UML_SIMD_PERMUTE(zip1);
	static constexpr auto uzp2 = UML_SIMD_PERMUTE(uzp2);
	static constexpr auto trn2 = UML_SIMD_PERMUTE(trn2);
	static constexpr auto zip2 = UML_SIMD_PERMUTE(zip2);
#undef UML_SIMD_PERMUTE
	switch (opcode) {
	case 1: return uzp1[shape];
	case 2: return trn1[shape];
	case 3: return zip1[shape];
	case 5: return uzp2[shape];
	case 6: return trn2[shape];
	case 7: return zip2[shape];
	default: return nullptr;
	}
}

[[nodiscard]] Gadget simd_table_lookup_gadget(
	bool extend, unsigned count, bool q) noexcept
{
	const unsigned shape = (count - 1U) * 2U + (q ? 1U : 0U);
	static constexpr std::array<Gadget, 8> tbl{
		uml_arm64_gadget_simd_tbl_1_8b,
		uml_arm64_gadget_simd_tbl_1_16b,
		uml_arm64_gadget_simd_tbl_2_8b,
		uml_arm64_gadget_simd_tbl_2_16b,
		uml_arm64_gadget_simd_tbl_3_8b,
		uml_arm64_gadget_simd_tbl_3_16b,
		uml_arm64_gadget_simd_tbl_4_8b,
		uml_arm64_gadget_simd_tbl_4_16b};
	static constexpr std::array<Gadget, 8> tbx{
		uml_arm64_gadget_simd_tbx_1_8b,
		uml_arm64_gadget_simd_tbx_1_16b,
		uml_arm64_gadget_simd_tbx_2_8b,
		uml_arm64_gadget_simd_tbx_2_16b,
		uml_arm64_gadget_simd_tbx_3_8b,
		uml_arm64_gadget_simd_tbx_3_16b,
		uml_arm64_gadget_simd_tbx_4_8b,
		uml_arm64_gadget_simd_tbx_4_16b};
	return (extend ? tbx : tbl)[shape];
}

[[nodiscard]] Gadget simd_aes_gadget(unsigned operation,
                                     bool force_software) noexcept {
  const auto &host = host_capabilities();
  const bool native = !force_software &&
                      host.has(ArmFeature::advanced_simd) &&
                      host.has(ArmFeature::aes);
  switch (operation) {
  case 4:
    return native ? uml_arm64_gadget_simd_aese_native
                  : uml_arm64_gadget_simd_aese_soft;
  case 5:
    return native ? uml_arm64_gadget_simd_aesd_native
                  : uml_arm64_gadget_simd_aesd_soft;
  case 6:
    return native ? uml_arm64_gadget_simd_aesmc_native
                  : uml_arm64_gadget_simd_aesmc_soft;
  case 7:
    return native ? uml_arm64_gadget_simd_aesimc_native
                  : uml_arm64_gadget_simd_aesimc_soft;
  default:
    return nullptr;
  }
}

[[nodiscard]] Gadget simd_sha_three_gadget(unsigned operation,
                                           bool force_software) noexcept {
  static constexpr std::array<Gadget, 7> native{
      uml_arm64_gadget_simd_sha1c,    uml_arm64_gadget_simd_sha1p,
      uml_arm64_gadget_simd_sha1m,    uml_arm64_gadget_simd_sha1su0,
      uml_arm64_gadget_simd_sha256h,  uml_arm64_gadget_simd_sha256h2,
      uml_arm64_gadget_simd_sha256su1};
  static constexpr std::array<Gadget, 7> software{
      uml_arm64_gadget_simd_sha1c_soft,    uml_arm64_gadget_simd_sha1p_soft,
      uml_arm64_gadget_simd_sha1m_soft,    uml_arm64_gadget_simd_sha1su0_soft,
      uml_arm64_gadget_simd_sha256h_soft,  uml_arm64_gadget_simd_sha256h2_soft,
      uml_arm64_gadget_simd_sha256su1_soft};
  if (operation >= native.size())
    return nullptr;
  const bool supported = operation <= 3U
                             ? host_capabilities().has(ArmFeature::sha1)
                             : host_capabilities().has(ArmFeature::sha256);
  return (!force_software && supported ? native : software)[operation];
}

[[nodiscard]] Gadget simd_sha_two_gadget(unsigned operation,
                                         bool force_software) noexcept {
  static constexpr std::array<Gadget, 3> native{
      uml_arm64_gadget_simd_sha1h, uml_arm64_gadget_simd_sha1su1,
      uml_arm64_gadget_simd_sha256su0};
  static constexpr std::array<Gadget, 3> software{
      uml_arm64_gadget_simd_sha1h_soft, uml_arm64_gadget_simd_sha1su1_soft,
      uml_arm64_gadget_simd_sha256su0_soft};
  if (operation >= native.size())
    return nullptr;
  const bool supported = operation <= 1U
                             ? host_capabilities().has(ArmFeature::sha1)
                             : host_capabilities().has(ArmFeature::sha256);
  return (!force_software && supported ? native : software)[operation];
}

[[nodiscard]] Gadget simd_sha512_three_gadget(std::uint32_t instruction,
                                              bool force_software) noexcept {
  const bool native =
      !force_software && host_capabilities().has(ArmFeature::sha512);
  switch (instruction & 0xffe0'fc00U) {
  case 0xce60'8000U:
    return native ? uml_arm64_gadget_simd_sha512h
                  : uml_arm64_gadget_simd_sha512h_soft;
  case 0xce60'8400U:
    return native ? uml_arm64_gadget_simd_sha512h2
                  : uml_arm64_gadget_simd_sha512h2_soft;
  case 0xce60'8800U:
    return native ? uml_arm64_gadget_simd_sha512su1
                  : uml_arm64_gadget_simd_sha512su1_soft;
  default:
    return nullptr;
  }
}

[[nodiscard]] Gadget simd_xar_gadget(
	unsigned rotate, bool native_path) noexcept
{
#define UML_SIMD_XAR_GADGETS(implementation) std::array<Gadget, 64>{ \
	uml_arm64_gadget_simd_xar_##implementation##_0, \
	uml_arm64_gadget_simd_xar_##implementation##_1, \
	uml_arm64_gadget_simd_xar_##implementation##_2, \
	uml_arm64_gadget_simd_xar_##implementation##_3, \
	uml_arm64_gadget_simd_xar_##implementation##_4, \
	uml_arm64_gadget_simd_xar_##implementation##_5, \
	uml_arm64_gadget_simd_xar_##implementation##_6, \
	uml_arm64_gadget_simd_xar_##implementation##_7, \
	uml_arm64_gadget_simd_xar_##implementation##_8, \
	uml_arm64_gadget_simd_xar_##implementation##_9, \
	uml_arm64_gadget_simd_xar_##implementation##_10, \
	uml_arm64_gadget_simd_xar_##implementation##_11, \
	uml_arm64_gadget_simd_xar_##implementation##_12, \
	uml_arm64_gadget_simd_xar_##implementation##_13, \
	uml_arm64_gadget_simd_xar_##implementation##_14, \
	uml_arm64_gadget_simd_xar_##implementation##_15, \
	uml_arm64_gadget_simd_xar_##implementation##_16, \
	uml_arm64_gadget_simd_xar_##implementation##_17, \
	uml_arm64_gadget_simd_xar_##implementation##_18, \
	uml_arm64_gadget_simd_xar_##implementation##_19, \
	uml_arm64_gadget_simd_xar_##implementation##_20, \
	uml_arm64_gadget_simd_xar_##implementation##_21, \
	uml_arm64_gadget_simd_xar_##implementation##_22, \
	uml_arm64_gadget_simd_xar_##implementation##_23, \
	uml_arm64_gadget_simd_xar_##implementation##_24, \
	uml_arm64_gadget_simd_xar_##implementation##_25, \
	uml_arm64_gadget_simd_xar_##implementation##_26, \
	uml_arm64_gadget_simd_xar_##implementation##_27, \
	uml_arm64_gadget_simd_xar_##implementation##_28, \
	uml_arm64_gadget_simd_xar_##implementation##_29, \
	uml_arm64_gadget_simd_xar_##implementation##_30, \
	uml_arm64_gadget_simd_xar_##implementation##_31, \
	uml_arm64_gadget_simd_xar_##implementation##_32, \
	uml_arm64_gadget_simd_xar_##implementation##_33, \
	uml_arm64_gadget_simd_xar_##implementation##_34, \
	uml_arm64_gadget_simd_xar_##implementation##_35, \
	uml_arm64_gadget_simd_xar_##implementation##_36, \
	uml_arm64_gadget_simd_xar_##implementation##_37, \
	uml_arm64_gadget_simd_xar_##implementation##_38, \
	uml_arm64_gadget_simd_xar_##implementation##_39, \
	uml_arm64_gadget_simd_xar_##implementation##_40, \
	uml_arm64_gadget_simd_xar_##implementation##_41, \
	uml_arm64_gadget_simd_xar_##implementation##_42, \
	uml_arm64_gadget_simd_xar_##implementation##_43, \
	uml_arm64_gadget_simd_xar_##implementation##_44, \
	uml_arm64_gadget_simd_xar_##implementation##_45, \
	uml_arm64_gadget_simd_xar_##implementation##_46, \
	uml_arm64_gadget_simd_xar_##implementation##_47, \
	uml_arm64_gadget_simd_xar_##implementation##_48, \
	uml_arm64_gadget_simd_xar_##implementation##_49, \
	uml_arm64_gadget_simd_xar_##implementation##_50, \
	uml_arm64_gadget_simd_xar_##implementation##_51, \
	uml_arm64_gadget_simd_xar_##implementation##_52, \
	uml_arm64_gadget_simd_xar_##implementation##_53, \
	uml_arm64_gadget_simd_xar_##implementation##_54, \
	uml_arm64_gadget_simd_xar_##implementation##_55, \
	uml_arm64_gadget_simd_xar_##implementation##_56, \
	uml_arm64_gadget_simd_xar_##implementation##_57, \
	uml_arm64_gadget_simd_xar_##implementation##_58, \
	uml_arm64_gadget_simd_xar_##implementation##_59, \
	uml_arm64_gadget_simd_xar_##implementation##_60, \
	uml_arm64_gadget_simd_xar_##implementation##_61, \
	uml_arm64_gadget_simd_xar_##implementation##_62, \
	uml_arm64_gadget_simd_xar_##implementation##_63}
	static constexpr auto native_gadgets = UML_SIMD_XAR_GADGETS(native);
	static constexpr auto software_gadgets = UML_SIMD_XAR_GADGETS(soft);
#undef UML_SIMD_XAR_GADGETS
	return (native_path ? native_gadgets : software_gadgets)[rotate];
}

[[nodiscard]] Gadget simd_pmull_gadget(
	bool high, unsigned size, bool force_software) noexcept
{
	if (size == 0U)
		return high ? uml_arm64_gadget_simd_pmull_8b2_direct :
			uml_arm64_gadget_simd_pmull_8b_direct;
	if (size == 3U) {
		const bool native = !force_software &&
			host_capabilities().has(ArmFeature::pmull);
		if (native)
			return high ? uml_arm64_gadget_simd_pmull_1d2_native_direct :
				uml_arm64_gadget_simd_pmull_1d_native_direct;
		return high ? uml_arm64_gadget_simd_pmull_1d2_soft_direct :
			uml_arm64_gadget_simd_pmull_1d_soft_direct;
	}
	return nullptr;
}

[[nodiscard]] Gadget floating_select_gadget(
	unsigned condition, unsigned type) noexcept
{
#define UML_FLOATING_SELECT_WIDTH(name) \
	(type == 3U ? uml_arm64_gadget_floating_select_##name##_h : \
	 type == 1U ? uml_arm64_gadget_floating_select_##name##_d : \
	 uml_arm64_gadget_floating_select_##name##_s)
#define UML_FLOATING_SELECT_CASE(index, name) \
	case index: return UML_FLOATING_SELECT_WIDTH(name);
	switch (condition) {
	UML_ARM64_CONDITIONS(UML_FLOATING_SELECT_CASE)
	default: return UML_FLOATING_SELECT_WIDTH(always);
	}
#undef UML_FLOATING_SELECT_CASE
#undef UML_FLOATING_SELECT_WIDTH
}

[[nodiscard]] Gadget floating_compare_gadget(
	unsigned type, bool signaling, bool zero) noexcept
{
#define UML_FLOATING_COMPARE_WIDTH(kind, operand) \
	(type == 3U ? uml_arm64_gadget_floating_compare_##kind##_h_##operand : \
	 type == 1U ? uml_arm64_gadget_floating_compare_##kind##_d_##operand : \
	 uml_arm64_gadget_floating_compare_##kind##_s_##operand)
	if (signaling)
		return zero ? UML_FLOATING_COMPARE_WIDTH(signaling, zero) :
			UML_FLOATING_COMPARE_WIDTH(signaling, register);
	return zero ? UML_FLOATING_COMPARE_WIDTH(quiet, zero) :
		UML_FLOATING_COMPARE_WIDTH(quiet, register);
#undef UML_FLOATING_COMPARE_WIDTH
}

[[nodiscard]] Gadget floating_convert_precision_gadget(
	unsigned source_type, unsigned destination_type) noexcept
{
	if (source_type == 3U)
		return destination_type == 0U ?
			uml_arm64_gadget_floating_convert_h_to_s :
			uml_arm64_gadget_floating_convert_h_to_d;
	if (source_type == 0U)
		return destination_type == 3U ?
			uml_arm64_gadget_floating_convert_s_to_h :
			uml_arm64_gadget_floating_convert_s_to_d;
	return destination_type == 3U ?
		uml_arm64_gadget_floating_convert_d_to_h :
		uml_arm64_gadget_floating_convert_d_to_s;
}

[[nodiscard]] Gadget floating_conditional_compare_gadget(
	unsigned condition, unsigned type, bool signaling) noexcept
{
#define UML_FLOATING_CONDITIONAL_COMPARE_WIDTH(name, kind) \
	(type == 3U ? uml_arm64_gadget_floating_conditional_compare_##name##_##kind##_h : \
	 type == 1U ? uml_arm64_gadget_floating_conditional_compare_##name##_##kind##_d : \
	 uml_arm64_gadget_floating_conditional_compare_##name##_##kind##_s)
#define UML_FLOATING_CONDITIONAL_COMPARE_KIND(name) \
	(signaling ? UML_FLOATING_CONDITIONAL_COMPARE_WIDTH(name, signaling) : \
	 UML_FLOATING_CONDITIONAL_COMPARE_WIDTH(name, quiet))
#define UML_FLOATING_CONDITIONAL_COMPARE_CASE(index, name) \
	case index: return UML_FLOATING_CONDITIONAL_COMPARE_KIND(name);
	switch (condition) {
	UML_ARM64_CONDITIONS(UML_FLOATING_CONDITIONAL_COMPARE_CASE)
	default: return floating_compare_gadget(type, signaling, false);
	}
#undef UML_FLOATING_CONDITIONAL_COMPARE_CASE
#undef UML_FLOATING_CONDITIONAL_COMPARE_KIND
#undef UML_FLOATING_CONDITIONAL_COMPARE_WIDTH
}

} // namespace uml::cpu::arm64::gadgets::selection
