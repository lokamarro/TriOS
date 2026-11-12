// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu_state.hpp"
#include "layout/assembly_layout.hpp"

namespace uml::cpu::x86_64 {
namespace {

[[nodiscard]] constexpr std::uint64_t mask_for_width(
	std::uint8_t width) noexcept
{
	return width == 8 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << (width * 8U)) - 1U;
}

[[nodiscard]] constexpr bool even_parity(std::uint8_t value) noexcept
{
	value ^= static_cast<std::uint8_t>(value >> 4U);
	value &= 0xfU;
	return ((0x9669U >> value) & 1U) != 0;
}

/* The flags one record defines on top of `flags`. An INC or DEC record with
 * a pending carry takes that one flag from the slot record, which is
 * evaluated the same way and never has a pending carry of its own. */
[[nodiscard]] std::uint64_t apply_record(const DeferredFlags &deferred,
	std::uint64_t flags, const DeferredFlags *slot) noexcept
{
	const bool shift = deferred.operation == DeferredFlagOperation::shift_left ||
		deferred.operation == DeferredFlagOperation::shift_right ||
		deferred.operation == DeferredFlagOperation::shift_arithmetic_right;
	if (shift) {
		/* Carry is the last bit shifted out; overflow is defined for a
		 * count of one and otherwise left as it was; the auxiliary flag
		 * is untouched. A count beyond the width shifts out zeros for the
		 * logical shifts and the sign for the arithmetic one. */
		const auto mask = mask_for_width(deferred.width);
		const unsigned bits = deferred.width * 8U;
		const auto sign = std::uint64_t{1} << (bits - 1U);
		const auto left = deferred.left & mask;
		const auto count = static_cast<unsigned>(deferred.right);
		const auto result = deferred.result & mask;
		flags &= ~(rflags_carry | rflags_parity | rflags_zero | rflags_sign);
		if (result == 0)
			flags |= rflags_zero;
		if ((result & sign) != 0)
			flags |= rflags_sign;
		if (even_parity(static_cast<std::uint8_t>(result)))
			flags |= rflags_parity;
		bool carry = false;
		switch (deferred.operation) {
		case DeferredFlagOperation::shift_left:
			carry = count <= bits && ((left >> (bits - count)) & 1U) != 0;
			break;
		case DeferredFlagOperation::shift_right:
			carry = count <= bits && ((left >> (count - 1U)) & 1U) != 0;
			break;
		default:
			carry = ((left >> (count <= bits ? count - 1U : bits - 1U)) &
				1U) != 0;
			break;
		}
		if (carry)
			flags |= rflags_carry;
		if (count == 1U) {
			flags &= ~rflags_overflow;
			if (deferred.operation == DeferredFlagOperation::shift_left) {
				if ((((result & sign) != 0) ? 1U : 0U) ^ (carry ? 1U : 0U))
					flags |= rflags_overflow;
			} else if (deferred.operation ==
				DeferredFlagOperation::shift_right && (left & sign) != 0) {
				flags |= rflags_overflow;
			}
		}
	} else if (deferred.operation != DeferredFlagOperation::none) {
		const auto mask = mask_for_width(deferred.width);
		const auto sign = std::uint64_t{1} << (deferred.width * 8U - 1U);
		const auto left = deferred.left & mask;
		const auto right = deferred.right & mask;
		const auto result = deferred.result & mask;
		constexpr auto arithmetic_flags = rflags_carry | rflags_parity |
			rflags_auxiliary | rflags_zero | rflags_sign | rflags_overflow;
		const bool preserve_carry = deferred.operation ==
			DeferredFlagOperation::increment || deferred.operation ==
			DeferredFlagOperation::decrement;
		flags &= ~(preserve_carry ?
			arithmetic_flags & ~rflags_carry : arithmetic_flags);

		if (result == 0)
			flags |= rflags_zero;
		if ((result & sign) != 0)
			flags |= rflags_sign;
		if (even_parity(static_cast<std::uint8_t>(result)))
			flags |= rflags_parity;

		switch (deferred.operation) {
		case DeferredFlagOperation::add:
		case DeferredFlagOperation::increment:
		case DeferredFlagOperation::add_carry: {
			const auto adjusted_right = (right + deferred.carry) & mask;
			if (!preserve_carry &&
				(adjusted_right < right || result < left))
				flags |= rflags_carry;
			if (((~(left ^ right) & (left ^ result)) & sign) != 0)
				flags |= rflags_overflow;
			if (((left ^ right ^ result) & 0x10U) != 0)
				flags |= rflags_auxiliary;
			break;
		}
		case DeferredFlagOperation::subtract:
		case DeferredFlagOperation::decrement:
		case DeferredFlagOperation::subtract_borrow: {
			const auto adjusted_right = (right + deferred.carry) & mask;
			if (!preserve_carry &&
				(adjusted_right < right || left < adjusted_right))
				flags |= rflags_carry;
			if ((((left ^ right) & (left ^ result)) & sign) != 0)
				flags |= rflags_overflow;
			if (((left ^ right ^ result) & 0x10U) != 0)
				flags |= rflags_auxiliary;
			break;
		}
		case DeferredFlagOperation::logical:
			break;
		case DeferredFlagOperation::none:
		case DeferredFlagOperation::shift_left:
		case DeferredFlagOperation::shift_right:
		case DeferredFlagOperation::shift_arithmetic_right:
			break;
		}
	}

	if (slot != nullptr && deferred.carry_pending != 0 &&
		(deferred.operation == DeferredFlagOperation::increment ||
		 deferred.operation == DeferredFlagOperation::decrement))
		flags = (flags & ~rflags_carry) |
			(apply_record(*slot, flags, nullptr) & rflags_carry);
	const auto override_mask = static_cast<std::uint64_t>(
		deferred.override_mask);
	flags = (flags & ~override_mask) |
		(static_cast<std::uint64_t>(deferred.override_value) & override_mask);
	return flags | rflags_reserved;
}

} // namespace

void materialize_flags(CpuState &cpu) noexcept
{
	const auto &deferred = cpu.deferred_flags;
	if (deferred.operation == DeferredFlagOperation::none &&
		deferred.override_mask == 0)
		return;
	cpu.rflags = apply_record(deferred, cpu.rflags, &cpu.carry_slot);
	cpu.deferred_flags.operation = DeferredFlagOperation::none;
	cpu.deferred_flags.carry_pending = 0;
	cpu.deferred_flags.override_mask = 0;
	cpu.deferred_flags.override_value = 0;
}

} // namespace uml::cpu::x86_64
