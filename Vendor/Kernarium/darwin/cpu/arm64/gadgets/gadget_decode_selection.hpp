// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gadget_operand_encoding.hpp"

namespace uml::cpu::arm64::gadgets::selection {

enum class ProductionDecodeStart : std::uint8_t {
	branch_system,
	data_processing_immediate,
	data_processing_register,
	data_processing_pointer_auth_overlap,
	floating_simd,
	load_store,
	exception
};

/* Generate the architectural first-level decoder from A64's op0 field.
 * Fine-grained masks remain in their existing priority order inside each
 * family, so overlapping encodings retain exactly the audited behavior. */
[[nodiscard]] consteval std::array<ProductionDecodeStart, 256>
make_production_decode_starts()
{
	std::array<ProductionDecodeStart, 256> starts{};
	for (unsigned byte = 0; byte < starts.size(); ++byte) {
		const unsigned op0 = (byte >> 1) & 15U;
		switch (op0) {
		case 8:
		case 9:
			starts[byte] = ProductionDecodeStart::data_processing_immediate;
			break;
		case 5:
		case 13:
			starts[byte] = ProductionDecodeStart::data_processing_register;
			break;
		case 7:
		case 15:
			starts[byte] = ProductionDecodeStart::floating_simd;
			break;
		case 4:
		case 6:
		case 12:
		case 14:
			starts[byte] = ProductionDecodeStart::load_store;
			break;
		default:
			starts[byte] = ProductionDecodeStart::branch_system;
			break;
		}
		/* ADR/ADRP use immlo in high-byte bits 6:5, so all eight possible
		 * high bytes share this low-five-bit signature. */
		if ((byte & 0x1fU) == 0x10U)
			starts[byte] = ProductionDecodeStart::branch_system;
	}
	/* Exceptions share op0 with branches/system but sit at the tail of the
	 * detailed chain. Their distinct high byte can jump there directly. */
	starts[0xd4] = ProductionDecodeStart::exception;
	/* FEAT_PAuth_LR has two encodings inside data-processing space. Route
	 * those high-byte overlaps through the exact unavailability predicate,
	 * then resume ordinary data processing when it does not match. */
	starts[0xda] =
		ProductionDecodeStart::data_processing_pointer_auth_overlap;
	starts[0xf3] =
		ProductionDecodeStart::data_processing_pointer_auth_overlap;
	return starts;
}

inline constexpr auto production_decode_starts =
	make_production_decode_starts();

} // namespace uml::cpu::arm64::gadgets::selection
