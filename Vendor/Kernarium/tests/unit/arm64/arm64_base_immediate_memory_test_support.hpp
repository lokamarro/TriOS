// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "require.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace uml::emulator::tests::base_immediate_memory {

namespace arm64 = uml::cpu::arm64;

inline constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr std::uint64_t data_base = engine_test_data_base;
inline constexpr std::uint64_t store_value = 0xf1e2'd3c4'b5a6'9788ULL;
inline constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

struct Variant final {
	unsigned size_code;
	unsigned operation;
	std::size_t size;
	bool load;
	std::string_view name;
};

/* These are the thirteen allocated scalar size/opc combinations shared by
 * unsigned-offset, unscaled, unprivileged, pre-index, and post-index forms.
 * The table itself is an independent allocation oracle: no production decode
 * helper is reused.
 */
inline constexpr std::array variants{
	Variant{0, 0, 1, false, "strb"},
	Variant{0, 1, 1, true, "ldrb"},
	Variant{0, 2, 1, true, "ldrsb-x"},
	Variant{0, 3, 1, true, "ldrsb-w"},
	Variant{1, 0, 2, false, "strh"},
	Variant{1, 1, 2, true, "ldrh"},
	Variant{1, 2, 2, true, "ldrsh-x"},
	Variant{1, 3, 2, true, "ldrsh-w"},
	Variant{2, 0, 4, false, "str-w"},
	Variant{2, 1, 4, true, "ldr-w"},
	Variant{2, 2, 4, true, "ldrsw-x"},
	Variant{3, 0, 8, false, "str-x"},
	Variant{3, 1, 8, true, "ldr-x"},
};

enum class Form : unsigned {
	unscaled = 0,
	post_index = 1,
	unprivileged = 2,
	pre_index = 3,
	unsigned_offset = 4,
};

inline constexpr std::array all_forms{
	Form::unscaled,
	Form::post_index,
	Form::unprivileged,
	Form::pre_index,
	Form::unsigned_offset,
};

[[nodiscard]] inline constexpr bool writes_back(Form form)
{
	return form == Form::post_index || form == Form::pre_index;
}

[[nodiscard]] inline constexpr std::uint32_t encode_unsigned(
	unsigned size_code, unsigned operation, unsigned imm12,
	unsigned rn, unsigned rt)
{
	return 0x3900'0000U | (size_code << 30) | (operation << 22) |
		((imm12 & 0xfffU) << 10) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline constexpr std::uint32_t encode_other(
	unsigned size_code, unsigned operation, unsigned imm9, Form form,
	unsigned rn, unsigned rt)
{
	return 0x3800'0000U | (size_code << 30) | (operation << 22) |
		((imm9 & 0x1ffU) << 12) |
		((static_cast<unsigned>(form) & 3U) << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline constexpr std::uint32_t encode(
	const Variant &variant, Form form, unsigned immediate,
	unsigned rn, unsigned rt)
{
	return form == Form::unsigned_offset ?
		encode_unsigned(variant.size_code, variant.operation,
			immediate, rn, rt) :
		encode_other(variant.size_code, variant.operation,
			immediate, form, rn, rt);
}

[[nodiscard]] inline constexpr std::int64_t signed_imm9(unsigned imm9)
{
	const auto value = static_cast<std::uint64_t>(imm9 & 0x1ffU);
	return value < 0x100U ? static_cast<std::int64_t>(value) :
		static_cast<std::int64_t>(value) - 0x200;
}

[[nodiscard]] inline constexpr unsigned encode_imm9(std::int64_t value)
{
	return static_cast<unsigned>(static_cast<std::uint64_t>(value) & 0x1ffU);
}

[[nodiscard]] inline constexpr std::uint64_t address_for(
	const Variant &variant, Form form, std::uint64_t base, unsigned immediate)
{
	if (form == Form::unsigned_offset)
		return base + static_cast<std::uint64_t>(immediate & 0xfffU) *
			variant.size;
	if (form == Form::post_index)
		return base;
	return base + static_cast<std::uint64_t>(
		signed_imm9(immediate));
}

[[nodiscard]] inline constexpr std::uint64_t writeback_for(
	Form form, std::uint64_t base, unsigned immediate)
{
	return writes_back(form) ?
		base + static_cast<std::uint64_t>(signed_imm9(immediate)) :
		base;
}

[[nodiscard]] inline constexpr std::uint64_t base_for_address(
	const Variant &variant, Form form, std::uint64_t address,
	unsigned immediate)
{
	if (form == Form::post_index)
		return address;
	if (form == Form::unsigned_offset)
		return address -
			static_cast<std::uint64_t>(immediate & 0xfffU) *
				variant.size;
	return address - static_cast<std::uint64_t>(
		signed_imm9(immediate));
}

[[nodiscard]] inline arm64::CpuState seeded_state(std::uint64_t seed)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			seed ^ (0x8000'0000'1000'0000ULL |
				(static_cast<std::uint64_t>(index) << 8) | index);
	cpu.sp = seed ^ 0x1234'5678'9abc'def0ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'009fU;
	cpu.tls = seed ^ 0xfedc'ba98'7654'3210ULL;
	for (unsigned index = 0; index < cpu.vectors.size(); ++index)
		for (unsigned byte = 0; byte < cpu.vectors[index].bytes.size();
		     ++byte)
			cpu.vectors[index].bytes[byte] =
				static_cast<std::byte>(seed + index * 23U + byte);
	return cpu;
}

inline void fill_memory(std::span<std::byte> image, unsigned salt)
{
	for (std::size_t offset = 0; offset < image.size(); ++offset)
		image[offset] = static_cast<std::byte>(
			0x80U | ((offset * 37U + salt * 29U) & 0x7fU));
}

[[nodiscard]] inline std::uint64_t decode_little_endian(
	std::span<const std::byte> image, std::size_t offset, std::size_t size)
{
	std::uint64_t value = 0;
	for (std::size_t index = 0; index < size; ++index)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(image[offset + index])) <<
			(index * 8U);
	return value;
}

inline void encode_little_endian(
	std::span<std::byte> image, std::size_t offset, std::size_t size,
	std::uint64_t value)
{
	for (std::size_t index = 0; index < size; ++index)
		image[offset + index] =
			static_cast<std::byte>(value >> (index * 8U));
}

[[nodiscard]] inline constexpr std::uint64_t low_mask(unsigned bits)
{
	return bits == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << bits) - 1U;
}

[[nodiscard]] inline constexpr std::uint64_t sign_extend_bits(
	std::uint64_t value, unsigned bits)
{
	const auto mask = low_mask(bits);
	const auto sign = std::uint64_t{1} << (bits - 1U);
	value &= mask;
	/* Unsigned modular subtraction defines the desired two's-complement bit
	 * pattern without relying on signed overflow or implementation-defined
	 * right shifts.
	 */
	return (value ^ sign) - sign;
}

[[nodiscard]] inline constexpr std::uint64_t expected_load(
	const Variant &variant, std::uint64_t raw)
{
	const auto bits = static_cast<unsigned>(variant.size * 8U);
	if (variant.operation < 2U)
		return raw & low_mask(bits);
	auto value = sign_extend_bits(raw, bits);
	return variant.operation == 3U ? value & 0xffff'ffffU : value;
}

using uml::testing::require;

inline void check_program(
	EngineHarness &harness, std::string_view name,
	std::span<const std::uint32_t> instructions,
	const arm64::CpuState &initial, arm64::CpuState expected,
	std::uint64_t region_address, std::span<const std::byte> initial_memory,
	std::span<const std::byte> expected_memory)
{
	std::vector<ProgramWord> code;
	code.reserve(instructions.size() + 1U);
	for (std::size_t index = 0; index < instructions.size(); ++index)
		code.push_back({
			.offset = index * sizeof(std::uint32_t),
			.instruction = instructions[index],
		});
	code.push_back({
		.offset = instructions.size() * sizeof(std::uint32_t),
		.instruction = terminal_system_call,
	});
	expected.pc = engine_test_code_base +
		code.size() * sizeof(std::uint32_t);
	const std::array regions{
		MemoryRegion{
			.address = region_address,
			.initial = initial_memory,
			.expected = expected_memory,
		},
	};
	harness.check_program({
		.name = name,
		.code = std::span<const ProgramWord>{code},
		.initial = initial,
		.expected = expected,
		.native = NativeRequirements::baseline(),
		.memory = std::span<const MemoryRegion>{regions},
	});
}

inline void apply_oracle(
	const Variant &variant, Form form, unsigned immediate,
	unsigned rn, unsigned rt, arm64::CpuState &state,
	std::span<std::byte> memory, std::uint64_t region_address)
{
	const auto base = state.read_register(rn, true);
	const auto address = address_for(variant, form, base, immediate);
	const auto offset = static_cast<std::size_t>(address - region_address);
	if (variant.load) {
		const auto value = expected_load(variant,
			decode_little_endian(memory, offset, variant.size));
		state.write_register(rt, value, false);
		/* The chosen constrained-unpredictable resolution is writeback
		 * suppression for overlapping loads.
		 */
		if (writes_back(form) && !(rn == rt && rn != 31U))
			state.write_register(rn,
				writeback_for(form, base, immediate), true);
	} else {
		const auto value = state.read_register(rt, false);
		encode_little_endian(memory, offset, variant.size, value);
		if (writes_back(form))
			state.write_register(rn,
				writeback_for(form, base, immediate), true);
	}
}

} // namespace uml::emulator::tests::base_immediate_memory
