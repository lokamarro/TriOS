// SPDX-License-Identifier: GPL-2.0-or-later
#include "software_helpers.hpp"

#include "../detail/bfloat_conversion.hpp"
#include "../detail/bfloat_dot.hpp"
#include "../detail/bfloat_matrix.hpp"
#include "../detail/bfloat_multiply.hpp"
#include "../detail/floating_conversion.hpp"
#include "../detail/floating_integer_conversion.hpp"
#include "../detail/half_fused_long.hpp"
#include "../detail/instruction_primitives.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

using uml::cpu::arm64::gadgets::vector_at;

} // namespace

extern "C" [[gnu::noinline]] void uml_arm64_bfloat_conversion_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const auto destination_offset = operand & 0xffffU;
	const auto source_offset = (operand >> 16) & 0xffffU;
	const bool scalar = (operand & (std::uintptr_t{1} << 32)) != 0U;
	const bool upper = (operand & (std::uintptr_t{1} << 33)) != 0U;
	auto &destination = vector_at(cpu, destination_offset);
	const auto source = vector_at(cpu, source_offset).bytes;
	auto result = upper ? destination.bytes : std::array<std::byte, 16>{};
	const unsigned lanes = scalar ? 1U : 4U;
	const unsigned first_lane = upper ? 4U : 0U;
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto input = static_cast<std::uint32_t>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{source}.subspan(lane * 4U, 4U)));
		const auto converted =
			uml::cpu::arm64::detail::convert_to_bfloat(input, cpu->fpcr);
		uml::cpu::arm64::detail::encode_little_endian(converted.bits,
			std::span{result}.subspan((first_lane + lane) * 2U, 2U));
		cpu->fpsr |= converted.exceptions;
	}
	destination.bytes = result;
}

extern "C" [[gnu::noinline]] void uml_arm64_bfloat_multiply_long_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const auto vector = [cpu, operand](unsigned shift)
		-> uml::cpu::arm64::VectorRegister & {
		return vector_at(cpu, (operand >> shift) & 0xffffU);
	};
	auto &destination = vector(0U);
	const auto addends = destination.bytes;
	const auto first = vector(16U).bytes;
	const auto second = vector(32U).bytes;
	const bool top = (operand & (std::uintptr_t{1} << 48)) != 0U;
	const bool by_element = (operand & (std::uintptr_t{1} << 49)) != 0U;
	const unsigned index = (operand >> 50) & 7U;
	auto result = addends;
	for (unsigned lane = 0; lane < 4U; ++lane) {
		const unsigned first_lane = lane * 2U + top;
		const unsigned second_lane = by_element ? index : lane * 2U + top;
		const auto addend = static_cast<std::uint32_t>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{addends}.subspan(lane * 4U, 4U)));
		const auto left = static_cast<std::uint16_t>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{first}.subspan(first_lane * 2U, 2U)));
		const auto right = static_cast<std::uint16_t>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{second}.subspan(second_lane * 2U, 2U)));
		uml::cpu::arm64::detail::encode_little_endian(
			uml::cpu::arm64::detail::bfloat_multiply_add(
				addend, left, right),
			std::span{result}.subspan(lane * 4U, 4U));
	}
	destination.bytes = result;
}

extern "C" [[gnu::noinline]] void uml_arm64_bfloat_dot_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const auto vector = [cpu, operand](unsigned shift)
		-> uml::cpu::arm64::VectorRegister & {
		return vector_at(cpu, (operand >> shift) & 0xffffU);
	};
	auto &destination = vector(0U);
	const auto addends = destination.bytes;
	const auto first = vector(16U).bytes;
	const auto second = vector(32U).bytes;
	const bool by_element = (operand & (std::uintptr_t{1} << 48)) != 0U;
	const bool wide = (operand & (std::uintptr_t{1} << 49)) != 0U;
	const unsigned index = (operand >> 50) & 3U;
	std::array<std::byte, 16> result{};
	const unsigned lanes = wide ? 4U : 2U;
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const unsigned first_pair = lane * 2U;
		const unsigned second_pair = by_element ? index * 2U : first_pair;
		const auto read_bfloat = [](const auto &source, unsigned element) {
			return static_cast<std::uint16_t>(
				uml::cpu::arm64::detail::decode_little_endian(
					std::span{source}.subspan(element * 2U, 2U)));
		};
		const auto addend = static_cast<std::uint32_t>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{addends}.subspan(lane * 4U, 4U)));
		uml::cpu::arm64::detail::encode_little_endian(
			uml::cpu::arm64::detail::bfloat_dot_add(addend,
				read_bfloat(first, first_pair),
				read_bfloat(first, first_pair + 1U),
				read_bfloat(second, second_pair),
				read_bfloat(second, second_pair + 1U)),
			std::span{result}.subspan(lane * 4U, 4U));
	}
	destination.bytes = result;
}

extern "C" [[gnu::noinline]] void uml_arm64_bfloat_matrix_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const auto vector = [cpu, operand](unsigned shift)
		-> uml::cpu::arm64::VectorRegister & {
		return vector_at(cpu, (operand >> shift) & 0xffffU);
	};
	auto &destination = vector(0U);
	const auto addends = destination.bytes;
	const auto left = vector(16U).bytes;
	const auto right = vector(32U).bytes;
	destination.bytes =
		uml::cpu::arm64::detail::bfloat_matrix_multiply_add(
			addends, left, right);
}

extern "C" [[gnu::noinline]] void uml_arm64_half_fused_long_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const auto vector = [cpu, operand](unsigned shift)
		-> uml::cpu::arm64::VectorRegister & {
		return vector_at(cpu, (operand >> shift) & 0xffffU);
	};
	const unsigned descriptor = operand >> 48U;
	const bool by_element = (descriptor & 1U) != 0U;
	const bool subtract = (descriptor & 2U) != 0U;
	const bool upper = (descriptor & 4U) != 0U;
	const bool wide = (descriptor & 8U) != 0U;
	const unsigned index = (descriptor >> 4U) & 7U;
	auto &destination = vector(0U);
	const auto addends = destination.bytes;
	const auto first = vector(16U).bytes;
	const auto second = vector(32U).bytes;
	std::array<std::byte, 16> result{};
	const unsigned lanes = wide ? 4U : 2U;
	const unsigned first_lane = upper ? lanes : 0U;
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const unsigned second_lane = by_element ? index : first_lane + lane;
		const auto addend = static_cast<std::uint32_t>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{addends}.subspan(lane * 4U, 4U)));
		const auto left = static_cast<std::uint16_t>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{first}.subspan((first_lane + lane) * 2U, 2U)));
		const auto right = static_cast<std::uint16_t>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{second}.subspan(second_lane * 2U, 2U)));
		const auto fused = uml::cpu::arm64::detail::half_fused_multiply_add(
			addend, left, right, subtract, cpu->fpcr);
		uml::cpu::arm64::detail::encode_little_endian(fused.bits,
			std::span{result}.subspan(lane * 4U, 4U));
		cpu->fpsr |= fused.exceptions;
	}
	destination.bytes = result;
}

extern "C" [[gnu::noinline]]
uml::cpu::arm64::detail::FloatingPrecisionConversion
uml_arm64_convert_floating_precision(std::uint64_t input,
	unsigned source_width, unsigned destination_width, std::uint32_t fpcr) noexcept
{
	return uml::cpu::arm64::detail::convert_floating_precision(
		input, source_width, destination_width, fpcr);
}

extern "C" [[gnu::noinline]] void
uml_arm64_simd_floating_precision_vector_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const unsigned destination = operand & 31U;
	const unsigned source_register = (operand >> 8) & 31U;
	const unsigned shape = (operand >> 16) & 7U;
	const bool upper = ((operand >> 20) & 1U) != 0U;
	const bool scalar = ((operand >> 21) & 1U) != 0U;
	constexpr std::array source_widths{16U, 32U, 32U, 64U, 64U};
	constexpr std::array destination_widths{32U, 64U, 16U, 32U, 32U};
	const unsigned source_width = source_widths[shape];
	const unsigned destination_width = destination_widths[shape];
	const bool narrow = shape >= 2U;
	const bool round_odd = shape == 4U;
	const unsigned lanes = scalar ? 1U :
		(narrow ? 128U : 64U) / source_width;
	const unsigned source_bytes = source_width / 8U;
	const unsigned destination_bytes = destination_width / 8U;
	const auto source = cpu->vectors[source_register].bytes;
	auto result = narrow && upper ? cpu->vectors[destination].bytes :
		std::array<std::byte, 16>{};
	const unsigned source_base = !narrow && upper ? 8U : 0U;
	const unsigned destination_base = narrow && upper ? 8U : 0U;
	std::uint32_t exceptions{};
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto input =
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{source}.subspan(
					source_base + lane * source_bytes, source_bytes));
		const auto converted =
			uml::cpu::arm64::detail::convert_floating_precision(input,
				source_width, destination_width, cpu->fpcr,
				round_odd ? 4U : ~0U);
		uml::cpu::arm64::detail::encode_little_endian(converted.bits,
			std::span{result}.subspan(
				destination_base + lane * destination_bytes,
				destination_bytes));
		exceptions |= converted.exceptions;
	}
	cpu->vectors[destination].bytes = result;
	cpu->fpsr |= exceptions;
}

extern "C" [[gnu::noinline]] void uml_arm64_floating_to_integer_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const unsigned destination = operand & 31U;
	const unsigned source_register = (operand >> 8) & 31U;
	const unsigned shape = (operand >> 16) & 15U;
	const bool unsigned_result = ((operand >> 20) & 1U) != 0U;
	const bool destination_gpr = ((operand >> 21) & 1U) != 0U;
	const auto rounding = static_cast<
		uml::cpu::arm64::detail::FloatingIntegerRounding>(
			(operand >> 24) & 7U);
	constexpr std::array widths{16U, 32U, 64U, 16U, 16U, 32U, 32U, 64U};
	constexpr std::array vector_bytes{2U, 4U, 8U, 8U, 16U, 8U, 16U, 16U};
	const unsigned width = widths[shape];
	const unsigned fractional_bits = (operand >> 28) & 127U;
	const unsigned integer_width = destination_gpr ?
		(((operand >> 22) & 1U) != 0U ? 64U : 32U) : width;
	const bool scalar = ((operand >> 23) & 1U) != 0U;
	const unsigned bytes = destination_gpr ? width / 8U :
		scalar ? integer_width / 8U : vector_bytes[shape];
	const unsigned lane_bytes = width / 8U;
	const auto source = cpu->vectors[source_register].bytes;
	std::uint32_t exceptions{};
	if (destination_gpr) {
		const auto input =
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{source}.first(lane_bytes));
		const auto converted =
			uml::cpu::arm64::detail::convert_floating_to_integer_bits(
				input, width, integer_width, fractional_bits, unsigned_result,
				rounding, cpu->fpcr);
		cpu->write_register(destination,
			integer_width == 64U ? converted.value :
				static_cast<std::uint32_t>(converted.value), false);
		exceptions = converted.exceptions;
	} else {
		std::array<std::byte, 16> result{};
		const unsigned lanes = scalar ? 1U : bytes / lane_bytes;
		for (unsigned lane = 0; lane < lanes; ++lane) {
			const auto input =
				uml::cpu::arm64::detail::decode_little_endian(
					std::span{source}.subspan(lane * lane_bytes, lane_bytes));
			const auto converted =
				uml::cpu::arm64::detail::convert_floating_to_integer_bits(
					input, width, integer_width, fractional_bits,
					unsigned_result,
					rounding, cpu->fpcr);
			uml::cpu::arm64::detail::encode_little_endian(converted.value,
				std::span{result}.subspan(lane * integer_width / 8U,
					integer_width / 8U));
			exceptions |= converted.exceptions;
		}
		cpu->vectors[destination].bytes = result;
	}
	cpu->fpsr |= exceptions;
}

extern "C" [[gnu::noinline]] void
uml_arm64_floating_javascript_convert_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const unsigned destination = operand & 31U;
	const unsigned source_register = (operand >> 8) & 31U;
	const auto input = uml::cpu::arm64::detail::decode_little_endian(
		std::span{cpu->vectors[source_register].bytes}.first(8U));
	const auto converted =
		uml::cpu::arm64::detail::convert_floating_to_javascript_integer(
			input, cpu->fpcr);
	cpu->write_register(destination, converted.value, false);
	cpu->nzcv = converted.sets_z ? 0x4000'0000U : 0U;
	cpu->fpsr |= converted.exceptions;
}

extern "C" [[gnu::noinline]] void uml_arm64_integer_to_floating_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const unsigned destination = operand & 31U;
	const unsigned source_register = (operand >> 8) & 31U;
	const unsigned shape = (operand >> 16) & 7U;
	const bool unsigned_source = ((operand >> 20) & 1U) != 0U;
	const bool source_gpr = ((operand >> 21) & 1U) != 0U;
	const bool scalar = ((operand >> 23) & 1U) != 0U;
	const unsigned destination_code = (operand >> 24) & 3U;
	const unsigned destination_width =
		destination_code == 0U ? 16U :
		destination_code == 1U ? 32U : 64U;
	const unsigned integer_width = !source_gpr ?
		destination_width : ((operand >> 22) & 1U) != 0U ? 64U : 32U;
	const unsigned fractional_bits = (operand >> 26) & 127U;
	constexpr std::array vector_bytes{2U, 4U, 8U, 8U, 16U, 8U, 16U, 16U};
	const unsigned bytes = scalar ? destination_width / 8U :
		vector_bytes[shape];
	const unsigned lanes = scalar ? 1U : bytes / (destination_width / 8U);
	const unsigned integer_bytes = integer_width / 8U;
	const unsigned destination_bytes = destination_width / 8U;
	const auto source = source_gpr ? std::array<std::byte, 16>{} :
		cpu->vectors[source_register].bytes;
	const std::uint64_t gpr_source = source_gpr ?
		cpu->read_register(source_register, false) : 0U;
	std::array<std::byte, 16> result{};
	std::uint32_t exceptions{};
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto input = source_gpr ? gpr_source :
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{source}.subspan(
					lane * integer_bytes, integer_bytes));
		const auto converted =
			uml::cpu::arm64::detail::convert_integer_to_floating_bits(
				input, integer_width, destination_width, fractional_bits,
				unsigned_source, cpu->fpcr);
		uml::cpu::arm64::detail::encode_little_endian(converted.bits,
			std::span{result}.subspan(
				lane * destination_bytes, destination_bytes));
		exceptions |= converted.exceptions;
	}
	cpu->vectors[destination].bytes = result;
	cpu->fpsr |= exceptions;
}

