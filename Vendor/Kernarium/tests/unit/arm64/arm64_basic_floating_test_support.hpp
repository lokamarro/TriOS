// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "cpu/arm64/detail/floating_arithmetic.hpp"
#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "require.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace arm64_basic_floating_test {

namespace common = uml::testing;
namespace arm64 = uml::cpu::arm64;
namespace detail = uml::cpu::arm64::detail;
namespace engine_tests = uml::emulator::tests;

using Operation = detail::BasicFloatingOperation;

struct Form final {
	Operation operation;
	bool scalar;
	unsigned width;
	unsigned vector_bytes;
};

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
};

inline constexpr std::array widths{16U, 32U, 64U};
inline constexpr std::array vector_shapes{
	std::array{16U, 8U}, std::array{16U, 16U},
	std::array{32U, 8U}, std::array{32U, 16U},
	std::array{64U, 16U}};

[[nodiscard]] constexpr unsigned scalar_opcode(Operation operation) noexcept
{
	switch (operation) {
	case Operation::multiply: return 0;
	case Operation::divide: return 1;
	case Operation::add: return 2;
	case Operation::subtract: return 3;
	case Operation::negative_multiply: return 8;
	case Operation::square_root: return 0;
	}
	return 0;
}

[[nodiscard]] constexpr unsigned scalar_type(unsigned width) noexcept
{
	return width == 16U ? 3U : width == 32U ? 0U : 1U;
}

[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1, 2}) noexcept
{
	std::uint32_t value;
	if (form.scalar) {
		value = form.operation == Operation::square_root ? 0x1e21'c000U :
			0x1e20'0800U | (scalar_opcode(form.operation) << 12);
		value |= scalar_type(form.width) << 22;
	} else {
		const bool q = form.vector_bytes == 16U;
		const bool half = form.width == 16U;
		const bool is_double = form.width == 64U;
		if (half) {
			switch (form.operation) {
			case Operation::add: value = 0x0e40'1400U; break;
			case Operation::subtract: value = 0x0ec0'1400U; break;
			case Operation::multiply: value = 0x2e40'1c00U; break;
			case Operation::divide: value = 0x2e40'3c00U; break;
			case Operation::square_root: value = 0x2ef9'f800U; break;
			default: value = 0U; break;
			}
		} else {
			switch (form.operation) {
			case Operation::add: value = 0x0e20'd400U; break;
			case Operation::subtract: value = 0x0ea0'd400U; break;
			case Operation::multiply: value = 0x2e20'dc00U; break;
			case Operation::divide: value = 0x2e20'fc00U; break;
			case Operation::square_root: value = 0x2ea1'f800U; break;
			default: value = 0U; break;
			}
			value |= is_double ? 0x0040'0000U : 0U;
		}
		value |= q ? 0x4000'0000U : 0U;
	}
	return value |
		(form.operation == Operation::square_root ? 0U :
			(registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) | (registers.destination & 31U);
}

} // namespace arm64_basic_floating_test
