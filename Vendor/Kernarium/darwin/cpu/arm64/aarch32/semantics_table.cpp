// SPDX-License-Identifier: GPL-2.0-or-later
//
// The library's table of the decoder's forms (semantics_internal.hpp): the
// operation of each (form_operation.hpp) and what only the library needs to
// execute it.

#include "cpu/arm64/aarch32/semantics_internal.hpp"

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>

namespace uml::cpu::arm64::aarch32::detail {

namespace {

[[nodiscard]] const std::array<OpInfo, kFormCount> &table()
{
	static const auto built = [] {
		std::array<OpInfo, kFormCount> result{};
		std::unordered_map<std::string_view, VectorHandler> by_stem;
		for (const auto entries : {vector_move_entries(), neon_integer_entries(), neon_shift_entries(),
				 vector_float_entries(), armv8_float_entries(), armv8_integer_entries(),
				 crypto_entries(), bf16_entries()})
			for (const VectorEntry &entry : entries)
				by_stem.emplace(entry.stem, entry.handler);
		for (std::size_t index = 0; index < kFormCount; ++index) {
			const std::string_view name = kForms[index].mnemonic;
			const std::string_view form = kForms[index].name;
			OpInfo info{form_operation(static_cast<Form>(index))};
			/* A VFP or Advanced SIMD data-processing form: the handler of
			 * its stem, the name up to the encoding tag. */
			if (info.op == Op::none) {
				std::string_view stem = form;
				std::string_view tail;
				for (std::size_t at = 0; at + 2U < form.size(); ++at)
					if (form[at] == '_' && (form[at + 1U] == 'A' || form[at + 1U] == 'T')
						&& form[at + 2U] >= '0' && form[at + 2U] <= '9') {
						stem = form.substr(0, at);
						tail = form.substr(at + 3U);
						break;
					}
				/* A tag followed by a BF16 name part (VCVTB_A1_bfs) selects
				 * the handler registered under the stem and that part, and
				 * never the handler of the stem alone. */
				const auto found = tail.starts_with("_b")
					? by_stem.find(std::string(stem) + std::string(tail))
					: by_stem.find(stem);
				if (found != by_stem.end()) {
					info.op = Op::vector;
					info.vector = found->second;
				}
			}
			/* BLX (immediate) shares BL's encoding file and links the same way. */
			if (info.op == Op::blx && form.starts_with("BL_i"))
				info.op = Op::bl;
			if (info.op == Op::vld || info.op == Op::vst) {
				/* VLDn_m_ (multiple structures), VLDn_1_ (one lane) and
				 * VLDn_a_ (all lanes), after the mnemonic's digit. */
				info.structs = static_cast<std::uint8_t>(name.back() - '0');
				info.structure = form.find("_1_") != std::string_view::npos
					? StructureKind::single_lane
					: form.find("_a_") != std::string_view::npos
						? StructureKind::all_lanes
						: StructureKind::multiple;
			}
			if (info.op == Op::ldm || info.op == Op::stm) {
				info.stack = form.starts_with("PUSH") || form.starts_with("POP");
				if (form.starts_with("LDMDA") || form.starts_with("STMDA"))
					info.transfer = Mode::da;
				else if (form.starts_with("LDMDB") || form.starts_with("STMDB")
					|| form.starts_with("PUSH"))
					info.transfer = Mode::db;
				else if (form.starts_with("LDMIB") || form.starts_with("STMIB"))
					info.transfer = Mode::ib;
			}
			result[index] = info;
		}
		return result;
	}();
	return built;
}

} // namespace

const OpInfo &op_info(Form form) noexcept
{
	return table()[static_cast<std::size_t>(form)];
}

} // namespace uml::cpu::arm64::aarch32::detail
