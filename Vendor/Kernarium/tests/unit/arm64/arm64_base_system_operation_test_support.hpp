// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_base_feature_system_test_support.hpp"
#include "arm64_base_ic_test_support.hpp"

#include <array>
#include <cstdint>

namespace arm64_base_system_operation_test {

enum class Requirement {
	baseline,
	dpb,
	dpb2,
	mec,
	mec_mte2,
	mte,
	mte2,
	mtetc,
	occmo,
	occmo_mte,
	pops,
	pops_mte2,
	rme,
	rme_mte2,
	tlbios,
	tlbirange,
	tlbiw,
	xs,
};

struct DcOperation final {
	const char *name;
	unsigned op1;
	unsigned crm;
	unsigned op2;
	Requirement requirement;
	bool accessible_at_el0;
};

/*
 * Complete 2026-06 A-profile DC operation table. Combined requirements are
 * retained explicitly instead of being inferred from the mnemonic.
 */
inline constexpr std::array dc_operations{
	DcOperation{"ivac", 0b000, 0b0110, 0b001,
		Requirement::baseline, false},
	DcOperation{"isw", 0b000, 0b0110, 0b010,
		Requirement::baseline, false},
	DcOperation{"igvac", 0b000, 0b0110, 0b011,
		Requirement::mte2, false},
	DcOperation{"igsw", 0b000, 0b0110, 0b100,
		Requirement::mte2, false},
	DcOperation{"igdvac", 0b000, 0b0110, 0b101,
		Requirement::mte2, false},
	DcOperation{"igdsw", 0b000, 0b0110, 0b110,
		Requirement::mte2, false},
	DcOperation{"csw", 0b000, 0b1010, 0b010,
		Requirement::baseline, false},
	DcOperation{"cgsw", 0b000, 0b1010, 0b100,
		Requirement::mte2, false},
	DcOperation{"cgdsw", 0b000, 0b1010, 0b110,
		Requirement::mte2, false},
	DcOperation{"cisw", 0b000, 0b1110, 0b010,
		Requirement::baseline, false},
	DcOperation{"cigsw", 0b000, 0b1110, 0b100,
		Requirement::mte2, false},
	DcOperation{"cigdsw", 0b000, 0b1110, 0b110,
		Requirement::mte2, false},
	DcOperation{"civaps", 0b000, 0b1111, 0b001,
		Requirement::pops, false},
	DcOperation{"cigdvaps", 0b000, 0b1111, 0b101,
		Requirement::pops_mte2, false},
	DcOperation{"zva", 0b011, 0b0100, 0b001,
		Requirement::baseline, true},
	DcOperation{"gva", 0b011, 0b0100, 0b011,
		Requirement::mte, false},
	DcOperation{"gzva", 0b011, 0b0100, 0b100,
		Requirement::mte, false},
	DcOperation{"zgbva", 0b011, 0b0100, 0b101,
		Requirement::mtetc, false},
	DcOperation{"gbva", 0b011, 0b0100, 0b111,
		Requirement::mtetc, false},
	DcOperation{"cvac", 0b011, 0b1010, 0b001,
		Requirement::baseline, true},
	DcOperation{"cgvac", 0b011, 0b1010, 0b011,
		Requirement::mte, false},
	DcOperation{"cgdvac", 0b011, 0b1010, 0b101,
		Requirement::mte, false},
	DcOperation{"cvaoc", 0b011, 0b1011, 0b000,
		Requirement::occmo, false},
	DcOperation{"cvau", 0b011, 0b1011, 0b001,
		Requirement::baseline, true},
	DcOperation{"cgdvaoc", 0b011, 0b1011, 0b111,
		Requirement::occmo_mte, false},
	DcOperation{"cvap", 0b011, 0b1100, 0b001,
		Requirement::dpb, false},
	DcOperation{"cgvap", 0b011, 0b1100, 0b011,
		Requirement::mte, false},
	DcOperation{"cgdvap", 0b011, 0b1100, 0b101,
		Requirement::mte, false},
	DcOperation{"cvadp", 0b011, 0b1101, 0b001,
		Requirement::dpb2, false},
	DcOperation{"cgvadp", 0b011, 0b1101, 0b011,
		Requirement::mte, false},
	DcOperation{"cgdvadp", 0b011, 0b1101, 0b101,
		Requirement::mte, false},
	DcOperation{"civac", 0b011, 0b1110, 0b001,
		Requirement::baseline, true},
	DcOperation{"cigvac", 0b011, 0b1110, 0b011,
		Requirement::mte, false},
	DcOperation{"cigdvac", 0b011, 0b1110, 0b101,
		Requirement::mte, false},
	DcOperation{"civaoc", 0b011, 0b1111, 0b000,
		Requirement::occmo, false},
	DcOperation{"cigdvaoc", 0b011, 0b1111, 0b111,
		Requirement::occmo_mte, false},
	DcOperation{"cipae", 0b100, 0b1110, 0b000,
		Requirement::mec, false},
	DcOperation{"cigdpae", 0b100, 0b1110, 0b111,
		Requirement::mec_mte2, false},
	DcOperation{"cipapa", 0b110, 0b1110, 0b001,
		Requirement::rme, false},
	DcOperation{"cigdpapa", 0b110, 0b1110, 0b101,
		Requirement::rme_mte2, false},
};
static_assert(dc_operations.size() == 40);

[[nodiscard]] constexpr std::uint32_t system_register_pair(
	bool read, unsigned o0, unsigned op1, unsigned crn, unsigned crm,
	unsigned op2, unsigned rt) noexcept
{
	return 0xd550'0000U | (static_cast<unsigned>(read) << 21) |
		((o0 & 1U) << 19) | ((op1 & 7U) << 16) |
		((crn & 15U) << 12) | ((crm & 15U) << 8) |
		((op2 & 7U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t system_instruction_pair(
	unsigned op1, unsigned crn, unsigned crm, unsigned op2,
	unsigned rt) noexcept
{
	return 0xd548'0000U | ((op1 & 7U) << 16) |
		((crn & 15U) << 12) | ((crm & 15U) << 8) |
		((op2 & 7U) << 5) | (rt & 31U);
}

} // namespace arm64_base_system_operation_test
