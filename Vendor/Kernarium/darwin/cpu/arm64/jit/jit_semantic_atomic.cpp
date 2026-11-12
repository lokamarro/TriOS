// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_semantic_instructions.hpp"
#include "jit_semantic_reservation.hpp"

#include "cpu/arm64/detail/instruction_primitives.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

namespace uml::cpu::arm64::jit::semantic {

using arm64::detail::decode_little_endian;
using arm64::detail::encode_little_endian;
using arm64::detail::sign_extend;

RunResult load_store_exclusive(CpuState &cpu,
					    GuestMemory &memory,
					    std::uint32_t instruction)
{
	const auto pc = cpu.pc;
	const bool load = (instruction & 0x0040'0000U) != 0;
	const bool pair = (instruction & 0x0020'0000U) != 0;
	const bool exclusive = (instruction & 0x0080'0000U) == 0;
	const unsigned status = (instruction >> 16) & 0x1fU;
	/* Compare-and-swap encodings are a separate class. */
	if (pair && !exclusive)
		return {StopReason::unsupported_instruction, pc, instruction, 0,
			false};
	const unsigned size_code = instruction >> 30;
	const unsigned transfer = instruction & 0x1fU;
	const unsigned second_transfer = (instruction >> 10) & 0x1fU;
	const unsigned base_register = (instruction >> 5) & 0x1fU;
	const auto tagged_address = cpu.read_register(base_register, true);
	const auto address = cpu.untag_data_address(tagged_address);
	const bool ordered = (instruction & 0x0000'8000U) != 0;
	if (pair) {
		/* Pair-exclusive encodings fix bit 31, use Rt2 as a real transfer
		 * register, and select UNDEFINED for duplicate load destinations in
		 * this architecture profile.
		 */
		if (size_code < 2U || (load && status != 0x1fU) ||
		    (load && transfer == second_transfer))
			return illegal(pc, instruction);
		const std::size_t element_size = std::size_t{1} << size_code;
		const auto total_size = element_size * 2;
		if ((base_register == 31U && (address & 15U) != 0) ||
		    single_copy_alignment_faults(address, total_size))
			return alignment_fault(address, instruction, !load);
		if (load) {
			std::scoped_lock lock{exclusive_memory_mutex};
			std::uint64_t first = 0;
			std::uint64_t second = 0;
			std::uintptr_t key = 0;
			if (!atomic_load_pair_value(memory, address, element_size,
					ordered ? std::memory_order_acquire :
						std::memory_order_relaxed,
					first, second, key))
				return memory_fault(address, instruction, false);
			cpu.write_register(transfer, first, false);
			cpu.write_register(second_transfer, second, false);
			cpu.exclusive_address = address;
			cpu.exclusive_key = key;
			/* Store the monitored bytes in address order rather than in the
			 * instruction's element shape. A 32-bit LDXP and a 64-bit LDXR
			 * cover the same eight-byte exclusive region and may pair with the
			 * corresponding store form.
			 */
			cpu.exclusive_value = element_size == 4 ?
				first | (second << 32) : first;
			cpu.exclusive_value2 = element_size == 8 ? second : 0;
			cpu.exclusive_version = reservation_version(key);
			cpu.exclusive_size = static_cast<std::uint8_t>(total_size);
		} else {
			const auto first = cpu.read_register(transfer, false);
			const auto second = cpu.read_register(second_transfer, false);
			std::scoped_lock lock{exclusive_memory_mutex};
			std::uint64_t current_first = 0;
			std::uint64_t current_second = 0;
			std::uintptr_t key = 0;
			if (!atomic_load_pair_value(memory, address, element_size,
					std::memory_order_relaxed, current_first,
					current_second, key))
				return memory_fault(address, instruction, true);
			const auto monitored_first = element_size == 4 ?
				current_first | (current_second << 32) : current_first;
			const auto monitored_second = element_size == 8 ?
				current_second : 0;
			bool success = cpu.exclusive_address == address &&
				cpu.exclusive_size == total_size &&
				cpu.exclusive_key == key &&
				cpu.exclusive_version == reservation_version(key) &&
				cpu.exclusive_value == monitored_first &&
				cpu.exclusive_value2 == monitored_second;
			const MonitoredValue monitored{cpu.exclusive_key,
				cpu.exclusive_value, cpu.exclusive_value2};
			cpu.clear_exclusive_monitor();
			if (success) {
				std::uintptr_t stored_key = 0;
				const auto store_result = atomic_store_exclusive_pair_value(
					memory, address, element_size, monitored, first, second,
					ordered ? std::memory_order_release :
						std::memory_order_relaxed,
					stored_key);
				if (store_result == AtomicStoreResult::fault)
					return memory_fault(address, instruction, true);
				if (store_result == AtomicStoreResult::stored)
					reservation_store(cpu, stored_key);
				else
					success = false;
			}
			cpu.write_register(status, success ? 0 : 1, false);
		}
		cpu.pc += 4;
		return continue_after(pc, instruction);
	}
	if (((instruction >> 10) & 0x1fU) != 0x1fU ||
	    (!exclusive && status != 0x1fU) ||
	    (exclusive && load && status != 0x1fU))
		return illegal(pc, instruction);
	const std::size_t size = std::size_t{1} << size_code;
	if ((base_register == 31U && (address & 15U) != 0) ||
	    single_copy_alignment_faults(address, size))
		return alignment_fault(address, instruction, !load);
	if (load) {
		if (exclusive) {
			std::scoped_lock lock{exclusive_memory_mutex};
			std::uint64_t value = 0;
			std::uintptr_t key = 0;
			if (!atomic_load_value(memory, address, size,
					       ordered ? std::memory_order_acquire :
						std::memory_order_relaxed,
					       value, key))
				return memory_fault(address, instruction, false);
			cpu.write_register(transfer, value, false);
			cpu.exclusive_address = address;
			cpu.exclusive_key = key;
			cpu.exclusive_value = value;
			cpu.exclusive_value2 = 0;
			cpu.exclusive_version = reservation_version(key);
			cpu.exclusive_size = static_cast<std::uint8_t>(size);
		} else {
			/* LDAR does not establish or inspect an exclusive reservation.
			 * Serializing polling acquire-loads with LDXR/STXR lets an
			 * aggressive waiter starve the thread that must advance the
			 * observed value.  The acquire fence below provides the ordered
			 * load semantics without involving the exclusive monitor.
			 */
			std::uint64_t value = 0;
			std::uintptr_t key = 0;
			if (!atomic_load_value(memory, address, size,
					       std::memory_order_acquire, value, key))
				return memory_fault(address, instruction, false);
			cpu.write_register(transfer, value, false);
		}
	} else {
		const auto value = cpu.read_register(transfer, false);
		std::scoped_lock lock{exclusive_memory_mutex};
		if (exclusive) {
			std::uint64_t current = 0;
			std::uintptr_t key = 0;
			if (!atomic_load_value(memory, address, size,
					       std::memory_order_relaxed, current, key))
				return memory_fault(address, instruction, true);
			bool success = cpu.exclusive_address == address &&
				cpu.exclusive_size == size &&
				cpu.exclusive_key == key &&
				cpu.exclusive_version == reservation_version(key) &&
				cpu.exclusive_value == current;
			const MonitoredValue monitored{cpu.exclusive_key,
				cpu.exclusive_value, 0};
			cpu.clear_exclusive_monitor();
			if (success) {
				std::uintptr_t stored_key = 0;
				const auto store_result = atomic_store_exclusive_value(
					memory, address, size, monitored, value,
					ordered ? std::memory_order_release :
						std::memory_order_relaxed,
					stored_key);
				if (store_result == AtomicStoreResult::fault)
					return memory_fault(address, instruction, true);
				if (store_result == AtomicStoreResult::stored)
					reservation_store(cpu, stored_key);
				else {
					success = false;
				}
			}
			cpu.write_register(status, success ? 0 : 1, false);
		} else {
			std::uintptr_t key = 0;
			if (atomic_store_value(memory, address, size, value,
					std::memory_order_release, key) !=
			    AtomicStoreResult::stored)
				return memory_fault(address, instruction, true);
			/* ARM reservations are invalidated by a write even when the
			 * stored bits equal the old value.  The sequence is keyed by the
			 * translated host reservation granule, so shared aliases observe
			 * the invalidation without unrelated atomics causing livelock.
			 */
			reservation_store(cpu, key);
		}
	}
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult load_store_compare_swap(CpuState &cpu,
	GuestMemory &memory, std::uint32_t instruction)
{
	const auto pc = cpu.pc;
	if (((instruction >> 10) & 31U) != 31U)
		return illegal(pc, instruction);
	const std::size_t size = std::size_t{1} << (instruction >> 30);
	const unsigned compare_register = (instruction >> 16) & 31U;
	const unsigned replacement_register = instruction & 31U;
	const unsigned base_register = (instruction >> 5) & 31U;
	const auto tagged_address = cpu.read_register(base_register, true);
	const auto address = cpu.untag_data_address(tagged_address);
	if ((base_register == 31U && (address & 15U) != 0) ||
	    single_copy_alignment_faults(address, size))
		return alignment_fault(address, instruction, true);
	const auto compare = cpu.read_register(compare_register, false);
	const auto replacement = cpu.read_register(replacement_register, false);
	const bool acquire = (instruction & 0x0040'0000U) != 0 &&
		compare_register != 31U;
	const bool release = (instruction & 0x0000'8000U) != 0;
	std::scoped_lock lock{exclusive_memory_mutex};
	std::uint64_t observed = 0;
	std::uintptr_t key = 0;
	if (!atomic_compare_swap_value(memory, address, size, compare, replacement,
			acquire, release, observed, key))
		return memory_fault(address, instruction, true);
	/* The architecture permits a CAS read to clear reservations even when its
	 * comparison fails. This implementation consistently chooses that behavior.
	 */
	reservation_store(cpu, key);
	cpu.write_register(compare_register, observed, false);
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult load_store_compare_swap_pair(CpuState &cpu,
	GuestMemory &memory, std::uint32_t instruction)
{
	const auto pc = cpu.pc;
	const unsigned compare_register = (instruction >> 16) & 31U;
	const unsigned replacement_register = instruction & 31U;
	if (((instruction >> 10) & 31U) != 31U ||
	    (compare_register & 1U) != 0 || (replacement_register & 1U) != 0)
		return illegal(pc, instruction);
	const unsigned size_code = instruction >> 30;
	if (size_code > 1U)
		return illegal(pc, instruction);
	const std::size_t element_size = std::size_t{4} << size_code;
	const auto total_size = element_size * 2;
	const unsigned base_register = (instruction >> 5) & 31U;
	const auto tagged_address = cpu.read_register(base_register, true);
	const auto address = cpu.untag_data_address(tagged_address);
	if ((base_register == 31U && (address & 15U) != 0) ||
	    single_copy_alignment_faults(address, total_size))
		return alignment_fault(address, instruction, true);
	const auto compare_first = cpu.read_register(compare_register, false);
	const auto compare_second = cpu.read_register(compare_register + 1, false);
	const auto replacement_first = cpu.read_register(
		replacement_register, false);
	const auto replacement_second = cpu.read_register(
		replacement_register + 1, false);
	const bool acquire = (instruction & 0x0040'0000U) != 0;
	const bool release = (instruction & 0x0000'8000U) != 0;
	std::scoped_lock lock{exclusive_memory_mutex};
	std::uint64_t observed_first = 0;
	std::uint64_t observed_second = 0;
	std::uintptr_t key = 0;
	if (!atomic_compare_swap_pair_value(memory, address, element_size,
			compare_first, compare_second, replacement_first,
			replacement_second, acquire, release, observed_first,
			observed_second, key))
		return memory_fault(address, instruction, true);
	reservation_store(cpu, key);
	cpu.write_register(compare_register, observed_first, false);
	cpu.write_register(compare_register + 1, observed_second, false);
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult load_store_atomic(CpuState &cpu, GuestMemory &memory,
	std::uint32_t instruction)
{
	const auto pc = cpu.pc;
	const unsigned o3 = (instruction >> 15) & 1U;
	const unsigned opcode = (instruction >> 12) & 7U;
	if (o3 && opcode != 0U)
		return illegal(pc, instruction);
	const std::size_t size = std::size_t{1} << (instruction >> 30);
	const unsigned operation = o3 ? 8U : opcode;
	const unsigned source = (instruction >> 16) & 31U;
	const unsigned destination = instruction & 31U;
	const unsigned base_register = (instruction >> 5) & 31U;
	const auto tagged_address = cpu.read_register(base_register, true);
	const auto address = cpu.untag_data_address(tagged_address);
	if ((base_register == 31U && (address & 15U) != 0) ||
	    single_copy_alignment_faults(address, size))
		return alignment_fault(address, instruction, true);
	const auto operand = cpu.read_register(source, false);
	const bool acquire = (instruction & 0x0080'0000U) != 0 &&
		destination != 31U;
	const bool release = (instruction & 0x0040'0000U) != 0;
	std::scoped_lock lock{exclusive_memory_mutex};
	std::uint64_t observed = 0;
	std::uintptr_t key = 0;
	if (!atomic_rmw_value(memory, address, size, operation, operand, acquire,
			release, observed, key))
		return memory_fault(address, instruction, true);
	reservation_store(cpu, key);
	cpu.write_register(destination, observed, false);
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

RunResult load_store_rcpc(CpuState &cpu, GuestMemory &memory,
	std::uint32_t instruction)
{
	const auto pc = cpu.pc;
	const bool no_offset =
		(instruction & 0x3fff'fc00U) == 0x38bf'c000U;
	const unsigned size_code = instruction >> 30;
	const unsigned operation = no_offset ? 1U : (instruction >> 22) & 3U;
	/* Unscaled opc=00 is STLUR, opc=01 is zero-extending LDAPUR, and the
	 * remaining allocated combinations are the signed LDAPUR forms. */
	if (!no_offset && ((operation == 2U && size_code == 3U) ||
	    (operation == 3U && size_code >= 2U)))
		return illegal(pc, instruction);
	const bool load = operation != 0U;
	const bool signed_load = operation >= 2U;
	const bool result_64 = size_code == 3U || operation == 2U;
	const std::size_t size = std::size_t{1} << size_code;
	const auto offset = no_offset ? std::int64_t{0} : sign_extend(
		(instruction >> 12) & 0x1ffU, 9);
	const unsigned base_register = (instruction >> 5) & 31U;
	const unsigned transfer = instruction & 31U;
	const auto base = cpu.read_register(base_register, true);
	const auto tagged_address = base + static_cast<std::uint64_t>(offset);
	const auto address = cpu.untag_data_address(tagged_address);
	if (single_copy_alignment_faults(address, size))
		return alignment_fault(address, instruction, !load);

	if (load) {
		std::uint64_t value = 0;
		std::uintptr_t key = 0;
		if (!atomic_load_value(memory, address, size,
				std::memory_order_acquire, value, key))
			return memory_fault(address, instruction, false);
		if (signed_load)
			value = static_cast<std::uint64_t>(sign_extend(value,
				static_cast<unsigned>(size * 8)));
		if (!result_64)
			value &= 0xffff'ffffU;
		cpu.write_register(transfer, value, false);
	} else {
		const auto value = cpu.read_register(transfer, false);
		std::scoped_lock lock{exclusive_memory_mutex};
		std::uintptr_t key = 0;
		if (atomic_store_value(memory, address, size, value,
				std::memory_order_release, key) !=
		    AtomicStoreResult::stored)
			return memory_fault(address, instruction, true);
		reservation_store(cpu, key);
	}
	cpu.pc += 4;
	return continue_after(pc, instruction);
}

} // namespace uml::cpu::arm64::jit::semantic
