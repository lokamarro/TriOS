// SPDX-License-Identifier: GPL-2.0-or-later
#include "program_builder_decode.hpp"
#include "gadget_selection.hpp"

namespace uml::cpu::arm64::gadgets::builder_detail {

using namespace selection;

DecodeResult decode_exception(DecodeContext &context)
{
	const auto address = context.address;
	const auto instruction = context.instruction;
	auto &compiled = context.compiled;
	auto &words = context.words;

	if ((instruction & 0xffe0'001fU) == 0xd420'0000U) {
		emit(words, uml_arm64_gadget_breakpoint);
		emit(words, address);
		++compiled;
		return DecodeResult::terminal;
	} else if ((instruction & 0xffe0'001fU) == 0xd400'0002U ||
		   (instruction & 0xffe0'001fU) == 0xd400'0003U ||
		   (instruction & 0xffe0'001fU) == 0xd440'0000U ||
		   (instruction & 0xffe0'001fU) == 0xd4a0'0001U ||
		   (instruction & 0xffe0'001fU) == 0xd4a0'0002U ||
		   (instruction & 0xffe0'001fU) == 0xd4a0'0003U ||
		   instruction == 0xd6bf'03e0U ||
		   instruction == 0xd69f'03e0U ||
		   instruction == 0xd69f'0bffU ||
		   instruction == 0xd69f'0fffU) {
		return illegal_instruction(context);
	} else if ((instruction & 0xffe0'001fU) == 0xd400'0001U) {
		emit(words, uml_arm64_gadget_svc);
		emit(words, address + sizeof(instruction));
		++compiled;
		return DecodeResult::terminal;
	} else {
		return implementation_gap(context);
	}
}

} // namespace uml::cpu::arm64::gadgets::builder_detail
