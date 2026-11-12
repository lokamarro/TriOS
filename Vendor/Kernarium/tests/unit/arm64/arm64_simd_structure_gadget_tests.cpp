// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "uml_darwin_guest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <span>
#include <vector>

namespace arm64_simd_structure_test {

namespace engine = uml::emulator::tests;

enum class AddressMode : unsigned {
	no_offset,
	post_immediate,
	post_register,
};

struct MultipleForm final {
	unsigned opcode;
	unsigned structure_elements;
};

/*
 * These are the seven allocated opcode partitions from the shared decode in
 * the architecture specification.  structure_elements==1 is the LD1/ST1
 * contiguous-register family; the remaining forms are de/interleaving
 * structures.
 */
inline constexpr std::array multiple_forms{
	MultipleForm{7, 1}, // LD1/ST1 one-register
	MultipleForm{10, 1}, // LD1/ST1 two-register
	MultipleForm{6, 1}, // LD1/ST1 three-register
	MultipleForm{2, 1}, // LD1/ST1 four-register
	MultipleForm{8, 2}, // LD2/ST2
	MultipleForm{4, 3}, // LD3/ST3
	MultipleForm{0, 4}, // LD4/ST4
};

struct SingleForm final {
	bool valid{};
	std::size_t element_size{};
	std::size_t lane{};
};

[[nodiscard]] inline constexpr bool multiple_arrangement_valid(
	const MultipleForm &form, bool full_vector, unsigned size)
{
	return form.structure_elements == 1U || full_vector || size != 3U;
}

[[nodiscard]] inline constexpr std::uint32_t multiple_instruction(
	bool full_vector, bool load, bool post_index, unsigned offset_register,
	unsigned opcode, unsigned size, unsigned rn, unsigned rt)
{
	return 0x0c00'0000U | (static_cast<unsigned>(full_vector) << 30) |
		(static_cast<unsigned>(post_index) << 23) |
		(static_cast<unsigned>(load) << 22) |
		((offset_register & 31U) << 16) | ((opcode & 15U) << 12) |
		((size & 3U) << 10) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline constexpr std::uint32_t single_instruction(
	bool full_vector, bool load, bool post_index, unsigned extra_register,
	unsigned offset_register, unsigned opcode, unsigned scale_bit,
	unsigned size, unsigned rn, unsigned rt)
{
	return 0x0d00'0000U | (static_cast<unsigned>(full_vector) << 30) |
		(static_cast<unsigned>(post_index) << 23) |
		(static_cast<unsigned>(load) << 22) |
		((extra_register & 1U) << 21) |
		((offset_register & 31U) << 16) | ((opcode & 7U) << 13) |
		((scale_bit & 1U) << 12) | ((size & 3U) << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

/*
 * Independent transcription of the shared single-structure decode:
 *   scale=00: byte lane, index Q:S:size
 *   scale=01: halfword lane, size<0> must be zero
 *   scale=10: word/doubleword lane, with the specified reserved cells
 *   scale=11: load-and-replicate only, S must be zero
 */
[[nodiscard]] inline constexpr SingleForm decode_single(
	bool full_vector, bool load, unsigned opcode, unsigned scale_bit,
	unsigned size)
{
	const unsigned scale = opcode >> 1;
	if (scale == 3U) {
		if (!load || scale_bit != 0U)
			return {};
		return {
			.valid = true,
			.element_size = std::size_t{1} << size,
		};
	}
	if (scale == 0U)
		return {
			.valid = true,
			.element_size = 1,
			.lane = (static_cast<std::size_t>(full_vector) << 3) |
				(scale_bit << 2) | size,
		};
	if (scale == 1U) {
		if ((size & 1U) != 0U)
			return {};
		return {
			.valid = true,
			.element_size = 2,
			.lane = (static_cast<std::size_t>(full_vector) << 2) |
				(scale_bit << 1) | (size >> 1),
		};
	}
	if ((size & 2U) != 0U ||
	    ((size & 1U) != 0U && scale_bit != 0U))
		return {};
	return {
		.valid = true,
		.element_size = (size & 1U) != 0U ? 8U : 4U,
		.lane = (size & 1U) != 0U ?
			static_cast<std::size_t>(full_vector) :
			(static_cast<std::size_t>(full_vector) << 1) | scale_bit,
	};
}

[[nodiscard]] inline constexpr bool post_index(AddressMode mode)
{
	return mode != AddressMode::no_offset;
}

[[nodiscard]] inline constexpr unsigned encoded_offset_register(
	AddressMode mode, unsigned register_offset)
{
	switch (mode) {
	case AddressMode::no_offset:
		return 0;
	case AddressMode::post_immediate:
		return 31;
	case AddressMode::post_register:
		return register_offset;
	}
	return 0;
}

} // namespace arm64_simd_structure_test

namespace structure = arm64_simd_structure_test;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace arm64 = uml::cpu::arm64;

namespace {

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	const std::array code{instruction, std::uint32_t{0xd400'0001U}};
	return gadgets::Builder{}.build(
		structure::engine::engine_test_code_base, code);
}

[[nodiscard]] constexpr std::uintptr_t base_offset(unsigned rn)
{
	return rn < 31U ? rn * sizeof(std::uint64_t) :
		offsetof(arm64::CpuState, sp);
}

[[nodiscard]] constexpr std::uintptr_t offset_register_offset(unsigned rm)
{
	return rm < 31U ? rm * sizeof(std::uint64_t) : 0U;
}

[[nodiscard]] constexpr std::uintptr_t packed_operand(
	unsigned rn, unsigned rt, unsigned rm, std::size_t lane_offset = 0)
{
	return rt | (base_offset(rn) << 16U) |
		(offset_register_offset(rm) << 32U) |
		(static_cast<std::uintptr_t>(lane_offset) << 48U);
}

[[nodiscard]] std::uintptr_t require_stream(
	const gadgets::BuildResult &built, std::uintptr_t operand)
{
	if (!built.complete)
		std::abort();
	const auto words = built.program.words();
	if (words.size() <= 1U)
		std::abort();
	if (words[1] != operand)
		std::abort();
	return words[0];
}

void multiple_translation_is_fully_specialized()
{
	std::set<std::uintptr_t> selected;
	for (const auto &form : structure::multiple_forms)
		for (const bool full_vector : {false, true})
			for (unsigned size = 0; size < 4U; ++size) {
				if (!structure::multiple_arrangement_valid(
					form, full_vector, size))
					continue;
				for (const bool load : {false, true})
					for (unsigned mode = 0; mode < 3U; ++mode)
						for (const bool sp_base : {false, true}) {
							const auto address_mode =
								static_cast<structure::AddressMode>(
									mode);
							const auto rn =
								sp_base ? 31U : 30U;
							const auto rm =
								structure::
									encoded_offset_register(
										address_mode, 5);
							const auto built = build(
								structure::
									multiple_instruction(
										full_vector, load,
										structure::post_index(
											address_mode),
										rm, form.opcode, size,
										rn, 29));
							selected.insert(require_stream(
								built,
								packed_operand(
									rn, 29, rm)));
						}
			}
	/*
	 * Each of the 48 contiguous and 126 interleaved transfer shapes has a
	 * distinct SP-specialized counterpart. The size field deliberately
	 * aliases the contiguous family because it has no runtime significance.
	 */
	if (selected.size() != 348U)
		std::abort();
}

void replicate_translation_is_fully_specialized()
{
	std::set<std::uintptr_t> selected;
	for (const bool full_vector : {false, true})
		for (unsigned extra = 0; extra < 2U; ++extra)
			for (unsigned opcode = 6; opcode < 8U; ++opcode)
				for (unsigned size = 0; size < 4U; ++size) {
					const auto form = structure::decode_single(
						full_vector, true, opcode, 0, size);
					if (!form.valid)
						std::abort();
					for (unsigned mode = 0; mode < 3U; ++mode)
						for (const bool sp_base : {false, true}) {
							const auto address_mode =
								static_cast<structure::AddressMode>(
									mode);
							const auto rn =
								sp_base ? 31U : 30U;
							const auto rm =
								structure::
									encoded_offset_register(
										address_mode, 5);
							const auto built = build(
								structure::
									single_instruction(
										full_vector, true,
										structure::post_index(
											address_mode),
										extra, rm, opcode, 0,
										size, rn, 29));
							selected.insert(require_stream(
								built,
								packed_operand(
									rn, 29, rm)));
						}
				}
	if (selected.size() != 192U)
		std::abort();
}

void lane_translation_is_fully_specialized()
{
	std::set<std::uintptr_t> selected;
	for (const bool full_vector : {false, true})
		for (const bool load : {false, true})
			for (unsigned extra = 0; extra < 2U; ++extra)
				for (unsigned opcode = 0; opcode < 6U; ++opcode)
					for (unsigned scale_bit = 0; scale_bit < 2U;
					     ++scale_bit)
						for (unsigned size = 0; size < 4U;
						     ++size) {
							const auto form =
								structure::decode_single(
									full_vector, load,
									opcode, scale_bit,
									size);
							if (!form.valid)
								continue;
							for (unsigned mode = 0; mode < 3U;
							     ++mode)
								for (const bool sp_base :
								     {false, true}) {
									const auto address_mode =
										static_cast<
											structure::
												AddressMode>(
											mode);
									const auto rn =
										sp_base ? 31U : 30U;
									const auto rm =
										structure::
											encoded_offset_register(
												address_mode,
												5);
									const auto built = build(
										structure::
											single_instruction(
												full_vector,
												load,
												structure::
													post_index(
														address_mode),
												extra, rm,
												opcode,
												scale_bit,
												size, rn,
												29));
									selected.insert(require_stream(
										built,
										packed_operand(
											rn, 29, rm,
											form.lane *
												form.element_size)));
								}
						}
	if (selected.size() != 192U)
		std::abort();
}

} // namespace

int main()
{
	multiple_translation_is_fully_specialized();
	replicate_translation_is_fully_specialized();
	lane_translation_is_fully_specialized();
	return 0;
}
