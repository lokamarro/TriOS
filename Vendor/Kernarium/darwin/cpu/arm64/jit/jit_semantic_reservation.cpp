// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_semantic_reservation.hpp"

#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/single_copy_alignment.hpp"
#include "cpu/host_capabilities.hpp"

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

ExclusiveMemoryMutex exclusive_memory_mutex;

constexpr std::uintptr_t reservation_granule = 16;
constexpr std::size_t reservation_slot_count = 1U << 15;

struct ReservationSlot final {
	/*
	 * Hash collisions are allowed to fail a guest store-conditional
	 * spuriously.  Therefore a monotonically increasing version per hash
	 * bucket is sufficient: every store to any colliding reservation
	 * granule invalidates the snapshot, without needing a separately locked
	 * key/value pair.
	 *
	 * Native JIT store blocks may finish concurrently on several vCPUs.
	 * Keeping the version atomic lets their post-store invalidation remain
	 * lock-free while semantic exclusive operations continue to serialize
	 * the actual memory transaction with exclusive_memory_mutex.
	 */
	std::atomic<std::uint64_t> version{};
};

std::array<ReservationSlot, reservation_slot_count> reservation_slots;

[[nodiscard]] std::uintptr_t fallback_reservation_key(
	std::uint64_t address) noexcept
{
	/* Direct host locations are naturally aligned.  Mark synthetic guest-VA
	 * keys in bit zero so the two namespaces cannot alias.
	 */
	return static_cast<std::uintptr_t>(
		(address & ~(reservation_granule - 1)) | 1);
}

[[nodiscard]] std::uintptr_t host_reservation_key(
	const std::byte *pointer) noexcept
{
	return reinterpret_cast<std::uintptr_t>(pointer) &
		~(reservation_granule - 1);
}

[[nodiscard]] std::uintptr_t reservation_key(GuestMemory &memory,
	std::uint64_t address, std::size_t size, MemoryAccess access) noexcept
{
	auto *pointer = memory.reservation_pointer(address, size, access);
	return pointer != nullptr ? host_reservation_key(pointer) :
		fallback_reservation_key(address);
}

[[nodiscard]] std::size_t slot_index(std::uintptr_t key) noexcept
{
	auto hash = key >> 4;
	hash ^= hash >> 17;
	return hash & (reservation_slot_count - 1);
}

[[nodiscard]] ReservationSlot &reservation_slot(std::uintptr_t key) noexcept
{
	return reservation_slots[slot_index(key)];
}

[[nodiscard]] std::size_t reservation_slot_index(const std::byte *host) noexcept
{
	return slot_index(host_reservation_key(host));
}

[[nodiscard]] std::uint64_t reservation_version(std::uintptr_t key) noexcept
{
	return reservation_slot(key).version.load(std::memory_order_acquire);
}

void reservation_store(CpuState &writer, std::uintptr_t key) noexcept
{
	auto &slot = reservation_slot(key);
	const auto before = slot.version.fetch_add(1, std::memory_order_release);
	/*
	 * The writer's own reservation survives a store to another granule,
	 * also one that hashes to the reserved granule's slot: when no other
	 * store has touched the slot since the writer's snapshot (`before` is
	 * still its version), the writer's snapshot moves up with the slot.
	 * The store of any other vCPU moved the version first, and fails the
	 * writer's store-exclusive as a collision may.
	 */
	if (writer.exclusive_size != 0 && writer.exclusive_version == before &&
	    key != writer.exclusive_key &&
	    &slot == &reservation_slot(writer.exclusive_key))
		writer.exclusive_version = before + 1;
}

template<typename Integer>
[[nodiscard]] std::uint64_t atomic_load_at(
	std::byte *pointer, std::memory_order order) noexcept
{
	auto &value = *reinterpret_cast<Integer *>(pointer);
	return std::atomic_ref<Integer>{value}.load(order);
}

template<typename Integer>
void atomic_store_at(std::byte *pointer, std::uint64_t value,
			     std::memory_order order) noexcept
{
	auto &destination = *reinterpret_cast<Integer *>(pointer);
	std::atomic_ref<Integer>{destination}.store(
		static_cast<Integer>(value), order);
}

template<typename Integer>
[[nodiscard]] std::uint64_t atomic_compare_swap_at(std::byte *pointer,
	std::uint64_t compare, std::uint64_t replacement,
	std::memory_order success_order, std::memory_order failure_order) noexcept
{
	auto &value = *reinterpret_cast<Integer *>(pointer);
	auto expected = static_cast<Integer>(compare);
	(void)std::atomic_ref<Integer>{value}.compare_exchange_strong(
		expected, static_cast<Integer>(replacement), success_order,
		failure_order);
	return expected;
}

void atomic_compare_swap_pair_128_at(std::byte *pointer,
	std::uint64_t compare_first, std::uint64_t compare_second,
	std::uint64_t replacement_first, std::uint64_t replacement_second,
	std::uint64_t &observed_first, std::uint64_t &observed_second) noexcept
{
	/* CASPAL is deliberately stronger than the requested guest ordering. The
	 * slow path favors a compact, race-free operation over weakening the host
	 * instruction for each encoding variant. Fixed adjacent registers satisfy
	 * CASP's architectural pair constraints.
	 */
	__asm__ volatile(
		"mov x9, %6\n"
		"mov x12, %2\n"
		"mov x13, %3\n"
		"mov x14, %4\n"
		"mov x15, %5\n"
		"caspal x12, x13, x14, x15, [x9]\n"
		"mov %0, x12\n"
		"mov %1, x13"
		: "=r"(observed_first), "=r"(observed_second)
		: "r"(compare_first), "r"(compare_second),
		  "r"(replacement_first), "r"(replacement_second), "r"(pointer)
		: "x9", "x12", "x13", "x14", "x15", "cc", "memory");
}

void atomic_load_pair_128_at(std::byte *pointer, std::uint64_t &first,
	std::uint64_t &second) noexcept
{
	/* The snapshot only seeds a later validating CASP or supplies an atomic
	 * pair load. Clear the implementation reservation immediately.
	 */
	__asm__ volatile(
		"ldxp %0, %1, [%2]\n"
		"clrex"
		: "=&r"(first), "=&r"(second)
		: "r"(pointer)
		: "memory");
}

/*
 * A direct pointer the LSE2 rule admits although it is not naturally
 * aligned: the host implements FEAT_LSE2 and the access stays inside one
 * 16-byte block, so one host LDAR or STLR is single-copy atomic. The
 * stronger host order is correct for every scalar form routed here.
 */
[[nodiscard]] bool lse2_host_pointer(const std::byte *pointer,
	std::size_t size) noexcept
{
	return size <= sizeof(std::uint64_t) &&
		!arm64::detail::single_copy_alignment_faults(
			reinterpret_cast<std::uintptr_t>(pointer), size,
			host_capabilities().has(ArmFeature::lse2));
}

[[nodiscard]] bool naturally_aligned(const std::byte *pointer,
	std::size_t size) noexcept
{
	return reinterpret_cast<std::uintptr_t>(pointer) % size == 0;
}

/*
 * A memory that offers atomic pointers records a failed translation as the
 * access's fault. Report it instead of completing the access through the
 * byte-wise fallback below: the fault repair of another vCPU could let that
 * fallback succeed as a read-modify-write that is not atomic.
 */
[[nodiscard]] bool atomic_translation_failed(GuestMemory &memory) noexcept
{
	return memory.has_fault();
}

/*
 * A read-modify-write the FEAT_LSE2 rule admits although it is not naturally
 * aligned: one host CASAL loop, which the host's own LSE2 keeps single-copy
 * atomic inside the 16-byte block. `size` is 2, 4 or 8.
 */
template<typename Result>
[[nodiscard]] std::uint64_t lse2_read_modify_write(std::byte *pointer,
	std::size_t size, Result result) noexcept
{
	auto expected = arm64::detail::lse2_acquire_load(pointer, size);
	for (;;) {
		const auto actual = arm64::detail::lse2_compare_swap(
			pointer, size, expected, result(expected));
		if (actual == expected)
			return expected;
		expected = actual;
	}
}

[[nodiscard]] bool atomic_load_value(GuestMemory &memory,
	std::uint64_t address, std::size_t size, std::memory_order order,
	std::uint64_t &value, std::uintptr_t &key)
{
	auto *pointer = memory.atomic_pointer(
		address, size, MemoryAccess::read);
	if (pointer != nullptr && naturally_aligned(pointer, size)) {
		/* An ordered load is LDAR, whose RCsc order the exclusive
		 * load-acquires need; it is also correct for the RCpc forms. A
		 * C++ acquire load would compile to the weaker LDAPR. */
		const auto host_order = order == std::memory_order_relaxed ?
			std::memory_order_relaxed : std::memory_order_seq_cst;
		switch (size) {
		case 1: value = atomic_load_at<std::uint8_t>(pointer, host_order); break;
		case 2: value = atomic_load_at<std::uint16_t>(pointer, host_order); break;
		case 4: value = atomic_load_at<std::uint32_t>(pointer, host_order); break;
		case 8: value = atomic_load_at<std::uint64_t>(pointer, host_order); break;
		default: return false;
		}
		key = host_reservation_key(pointer);
		return true;
	}
	if (pointer != nullptr && lse2_host_pointer(pointer, size)) {
		value = arm64::detail::lse2_acquire_load(pointer, size);
		key = host_reservation_key(pointer);
		return true;
	}
	if (atomic_translation_failed(memory))
		return false;

	std::array<std::byte, 8> bytes{};
	if (!memory.read(address, std::span{bytes}.first(size)))
		return false;
	value = arm64::detail::decode_little_endian(std::span{bytes}.first(size));
	if (order != std::memory_order_relaxed)
		std::atomic_thread_fence(std::memory_order_acquire);
	key = reservation_key(memory, address, size, MemoryAccess::read);
	return true;
}

[[nodiscard]] AtomicStoreResult atomic_store_value(GuestMemory &memory,
	std::uint64_t address, std::size_t size, std::uint64_t value,
	std::memory_order order, std::uintptr_t &key)
{
	auto *pointer = memory.atomic_pointer(
		address, size, MemoryAccess::write);
	if (pointer != nullptr && naturally_aligned(pointer, size)) {
		key = host_reservation_key(pointer);
		switch (size) {
		case 1: atomic_store_at<std::uint8_t>(pointer, value, order); break;
		case 2: atomic_store_at<std::uint16_t>(pointer, value, order); break;
		case 4: atomic_store_at<std::uint32_t>(pointer, value, order); break;
		case 8: atomic_store_at<std::uint64_t>(pointer, value, order); break;
		default: return AtomicStoreResult::fault;
		}
		return AtomicStoreResult::stored;
	}
	if (pointer != nullptr && lse2_host_pointer(pointer, size)) {
		key = host_reservation_key(pointer);
		arm64::detail::lse2_release_store(pointer, size, value);
		return AtomicStoreResult::stored;
	}
	if (atomic_translation_failed(memory))
		return AtomicStoreResult::fault;

	key = reservation_key(memory, address, size, MemoryAccess::write);
	std::array<std::byte, 8> bytes{};
	arm64::detail::encode_little_endian(value, std::span{bytes}.first(size));
	if (order != std::memory_order_relaxed)
		std::atomic_thread_fence(std::memory_order_release);
	if (!memory.write(address, std::span{bytes}.first(size)))
		return AtomicStoreResult::fault;
	/* A emulator without a direct write pointer must perform the write before
	 * it can distinguish an access fault (notably a Linux COW fault) from a
	 * supported fallback access.  A successful fallback write is therefore
	 * the same reservation location established by its corresponding load;
	 * direct read/write aliases are checked above before any store occurs.
	 */
	return AtomicStoreResult::stored;
}

[[nodiscard]] AtomicStoreResult atomic_store_exclusive_value(
	GuestMemory &memory, std::uint64_t address, std::size_t size,
	const MonitoredValue &monitored, std::uint64_t value,
	std::memory_order order, std::uintptr_t &key)
{
	auto *pointer = memory.atomic_pointer(
		address, size, MemoryAccess::write);
	const bool aligned = pointer != nullptr && naturally_aligned(pointer, size);
	if (aligned || (pointer != nullptr && lse2_host_pointer(pointer, size))) {
		key = host_reservation_key(pointer);
		if (key != monitored.key)
			return AtomicStoreResult::reservation_mismatch;
		const auto mask = size == 8 ? ~std::uint64_t{0} :
			(std::uint64_t{1} << (size * 8)) - 1;
		const auto compare = monitored.first & mask;
		const auto failure_order = std::memory_order_relaxed;
		std::uint64_t observed = 0;
		if (!aligned) {
			observed = arm64::detail::lse2_compare_swap(
				pointer, size, compare, value);
		} else {
			switch (size) {
			case 1: observed = atomic_compare_swap_at<std::uint8_t>(pointer,
				compare, value, order, failure_order); break;
			case 2: observed = atomic_compare_swap_at<std::uint16_t>(pointer,
				compare, value, order, failure_order); break;
			case 4: observed = atomic_compare_swap_at<std::uint32_t>(pointer,
				compare, value, order, failure_order); break;
			case 8: observed = atomic_compare_swap_at<std::uint64_t>(pointer,
				compare, value, order, failure_order); break;
			default: return AtomicStoreResult::fault;
			}
		}
		return observed == compare ? AtomicStoreResult::stored :
			AtomicStoreResult::reservation_mismatch;
	}
	if (atomic_translation_failed(memory))
		return AtomicStoreResult::fault;

	/* No direct pointer: the memory's own write is the only writer, so the
	 * caller's check of the value under the exclusive gate stands in for the
	 * compare-and-swap. */
	key = reservation_key(memory, address, size, MemoryAccess::write);
	if (key != monitored.key)
		return AtomicStoreResult::reservation_mismatch;
	std::array<std::byte, 8> bytes{};
	arm64::detail::encode_little_endian(value, std::span{bytes}.first(size));
	if (order != std::memory_order_relaxed)
		std::atomic_thread_fence(std::memory_order_release);
	return memory.write(address, std::span{bytes}.first(size)) ?
		AtomicStoreResult::stored : AtomicStoreResult::fault;
}

[[nodiscard]] bool atomic_compare_swap_value(GuestMemory &memory,
	std::uint64_t address, std::size_t size, std::uint64_t compare,
	std::uint64_t replacement, bool acquire, bool release,
	std::uint64_t &observed, std::uintptr_t &key)
{
	const auto success_order = acquire && release ? std::memory_order_acq_rel :
		acquire ? std::memory_order_acquire :
		release ? std::memory_order_release : std::memory_order_relaxed;
	const auto failure_order = acquire ? std::memory_order_acquire :
		std::memory_order_relaxed;
	auto *pointer = memory.atomic_pointer(address, size, MemoryAccess::write);
	if (pointer != nullptr && naturally_aligned(pointer, size)) {
		switch (size) {
		case 1:
			observed = atomic_compare_swap_at<std::uint8_t>(pointer,
				compare, replacement, success_order, failure_order);
			break;
		case 2:
			observed = atomic_compare_swap_at<std::uint16_t>(pointer,
				compare, replacement, success_order, failure_order);
			break;
		case 4:
			observed = atomic_compare_swap_at<std::uint32_t>(pointer,
				compare, replacement, success_order, failure_order);
			break;
		case 8:
			observed = atomic_compare_swap_at<std::uint64_t>(pointer,
				compare, replacement, success_order, failure_order);
			break;
		default: return false;
		}
		key = host_reservation_key(pointer);
		return true;
	}
	if (pointer != nullptr) {
		/* Misaligned inside one 16-byte block: CASAL under host LSE2. */
		observed = arm64::detail::lse2_compare_swap(
			pointer, size, compare, replacement);
		key = host_reservation_key(pointer);
		return true;
	}
	if (atomic_translation_failed(memory))
		return false;

	std::array<std::byte, 8> bytes{};
	if (!memory.read(address, std::span{bytes}.first(size)))
		return false;
	observed = arm64::detail::decode_little_endian(std::span{bytes}.first(size));
	if (observed == (compare & (size == 8 ? ~std::uint64_t{0} :
		((std::uint64_t{1} << (size * 8)) - 1)))) {
		if (release)
			std::atomic_thread_fence(std::memory_order_release);
		arm64::detail::encode_little_endian(replacement,
			std::span{bytes}.first(size));
	}
	/* Select the architecture-permitted write-back behavior on a failed
	 * comparison too. Besides deterministic reservation effects, this
	 * validates write permission for every atomic read-modify-write access. */
	if (!memory.write(address, std::span{bytes}.first(size)))
		return false;
	if (acquire)
		std::atomic_thread_fence(std::memory_order_acquire);
	key = reservation_key(memory, address, size, MemoryAccess::write);
	return true;
}

[[nodiscard]] bool atomic_compare_swap_pair_value(GuestMemory &memory,
	std::uint64_t address, std::size_t element_size,
	std::uint64_t compare_first, std::uint64_t compare_second,
	std::uint64_t replacement_first, std::uint64_t replacement_second,
	bool acquire, bool release, std::uint64_t &observed_first,
	std::uint64_t &observed_second, std::uintptr_t &key)
{
	const auto total_size = element_size * 2;
	std::array<std::byte, 16> bytes{};
	auto *pointer = memory.atomic_pointer(
		address, total_size, MemoryAccess::write);
	if (pointer != nullptr) {
		/* The single-copy rule keeps a 16-byte pair 16-byte aligned; an
		 * 8-byte pair may sit misaligned inside one 16-byte block. */
		key = host_reservation_key(pointer);
		if (total_size == 8) {
			const auto packed_compare =
				(compare_first & 0xffff'ffffU) |
				((compare_second & 0xffff'ffffU) << 32);
			const auto packed_replacement =
				(replacement_first & 0xffff'ffffU) |
				((replacement_second & 0xffff'ffffU) << 32);
			const auto success_order = acquire && release ?
				std::memory_order_acq_rel : acquire ?
				std::memory_order_acquire : release ?
				std::memory_order_release : std::memory_order_relaxed;
			const auto failure_order = acquire ?
				std::memory_order_acquire : std::memory_order_relaxed;
			const auto packed_observed = naturally_aligned(pointer, 8) ?
				atomic_compare_swap_at<std::uint64_t>(pointer,
					packed_compare, packed_replacement,
					success_order, failure_order) :
				arm64::detail::lse2_compare_swap(pointer, 8,
					packed_compare, packed_replacement);
			observed_first = packed_observed & 0xffff'ffffU;
			observed_second = packed_observed >> 32;
			return true;
		}
		atomic_compare_swap_pair_128_at(pointer, compare_first,
			compare_second, replacement_first, replacement_second,
			observed_first, observed_second);
		return true;
	}
	if (atomic_translation_failed(memory))
		return false;
	if (!memory.read(address, std::span{bytes}.first(total_size)))
		return false;
	key = reservation_key(memory, address, total_size, MemoryAccess::write);
	observed_first = arm64::detail::decode_little_endian(
		std::span{bytes}.first(element_size));
	observed_second = arm64::detail::decode_little_endian(
		std::span{bytes}.subspan(element_size, element_size));
	const auto mask = element_size == 8 ? ~std::uint64_t{0} :
		std::uint64_t{0xffff'ffffU};
	if (observed_first != (compare_first & mask) ||
	    observed_second != (compare_second & mask)) {
		/* Choose the permitted write-back of the observed pair so failed
		 * comparisons still validate atomic write permission. */
		if (!memory.write(address, std::span{bytes}.first(total_size)))
			return false;
		if (acquire)
			std::atomic_thread_fence(std::memory_order_acquire);
		return true;
	}
	if (release)
		std::atomic_thread_fence(std::memory_order_release);
	arm64::detail::encode_little_endian(replacement_first,
		std::span{bytes}.first(element_size));
	arm64::detail::encode_little_endian(replacement_second,
		std::span{bytes}.subspan(element_size, element_size));
	if (!memory.write(address, std::span{bytes}.first(total_size)))
		return false;
	if (acquire)
		std::atomic_thread_fence(std::memory_order_acquire);
	return true;
}

[[nodiscard]] constexpr std::uint64_t atomic_operation_result(unsigned operation,
	std::uint64_t observed, std::uint64_t operand, std::size_t size) noexcept
{
	const auto mask = size == 8 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << (size * 8)) - 1;
	observed &= mask;
	operand &= mask;
	switch (operation) {
	case 0: return (observed + operand) & mask;
	case 1: return observed & ~operand & mask;
	case 2: return observed ^ operand;
	case 3: return observed | operand;
	case 4:
		return arm64::detail::sign_extend(observed, static_cast<unsigned>(size * 8)) >=
			arm64::detail::sign_extend(operand, static_cast<unsigned>(size * 8)) ?
			observed : operand;
	case 5:
		return arm64::detail::sign_extend(observed, static_cast<unsigned>(size * 8)) <=
			arm64::detail::sign_extend(operand, static_cast<unsigned>(size * 8)) ?
			observed : operand;
	case 6: return std::max(observed, operand);
	case 7: return std::min(observed, operand);
	case 8: return operand;
	default: return observed;
	}
}

template<typename Integer>
[[nodiscard]] std::uint64_t atomic_rmw_at(std::byte *pointer,
	unsigned operation, std::uint64_t operand, std::memory_order order) noexcept
{
	auto &value = *reinterpret_cast<Integer *>(pointer);
	auto reference = std::atomic_ref<Integer>{value};
	auto expected = reference.load(std::memory_order_relaxed);
	const auto failure_order = order == std::memory_order_acquire ||
		order == std::memory_order_acq_rel ? std::memory_order_acquire :
		std::memory_order_relaxed;
	for (;;) {
		const auto observed = expected;
		const auto desired = static_cast<Integer>(atomic_operation_result(
			operation, observed, operand, sizeof(Integer)));
		if (reference.compare_exchange_weak(
				expected, desired, order, failure_order))
			return observed;
	}
}

[[nodiscard]] bool atomic_rmw_value(GuestMemory &memory,
	std::uint64_t address, std::size_t size, unsigned operation,
	std::uint64_t operand, bool acquire, bool release,
	std::uint64_t &observed, std::uintptr_t &key)
{
	const auto order = acquire && release ? std::memory_order_acq_rel :
		acquire ? std::memory_order_acquire :
		release ? std::memory_order_release : std::memory_order_relaxed;
	auto *pointer = memory.atomic_pointer(address, size, MemoryAccess::write);
	if (pointer != nullptr && naturally_aligned(pointer, size)) {
		switch (size) {
		case 1: observed = atomic_rmw_at<std::uint8_t>(pointer,
			operation, operand, order); break;
		case 2: observed = atomic_rmw_at<std::uint16_t>(pointer,
			operation, operand, order); break;
		case 4: observed = atomic_rmw_at<std::uint32_t>(pointer,
			operation, operand, order); break;
		case 8: observed = atomic_rmw_at<std::uint64_t>(pointer,
			operation, operand, order); break;
		default: return false;
		}
		key = host_reservation_key(pointer);
		return true;
	}
	if (pointer != nullptr) {
		observed = lse2_read_modify_write(pointer, size,
			[&](std::uint64_t current) {
				return atomic_operation_result(
					operation, current, operand, size);
			});
		key = host_reservation_key(pointer);
		return true;
	}
	if (atomic_translation_failed(memory))
		return false;

	std::array<std::byte, 8> bytes{};
	if (!memory.read(address, std::span{bytes}.first(size)))
		return false;
	observed = arm64::detail::decode_little_endian(std::span{bytes}.first(size));
	if (release)
		std::atomic_thread_fence(std::memory_order_release);
	arm64::detail::encode_little_endian(
		atomic_operation_result(operation, observed, operand, size),
		std::span{bytes}.first(size));
	if (!memory.write(address, std::span{bytes}.first(size)))
		return false;
	if (acquire)
		std::atomic_thread_fence(std::memory_order_acquire);
	key = reservation_key(memory, address, size, MemoryAccess::write);
	return true;
}

[[nodiscard]] bool atomic_load_pair_value(GuestMemory &memory,
	std::uint64_t address, std::size_t element_size, std::memory_order order,
	std::uint64_t &first, std::uint64_t &second, std::uintptr_t &key)
{
	const auto total_size = element_size * 2;
	std::array<std::byte, 16> bytes{};
	auto *pointer = memory.atomic_pointer(
		address, total_size, MemoryAccess::read);
	if (pointer != nullptr) {
		key = host_reservation_key(pointer);
		if (total_size == 16) {
			/* The single-copy rule keeps a 16-byte pair aligned. LDXP
			 * supplies one atomic 128-bit snapshot on the AArch64 host;
			 * CLREX keeps this internal implementation detail from
			 * retaining a reservation. */
			atomic_load_pair_128_at(pointer, first, second);
			if (order != std::memory_order_relaxed)
				std::atomic_thread_fence(order);
			return true;
		}
		const auto packed = !naturally_aligned(pointer, 8) ?
			arm64::detail::lse2_acquire_load(pointer, 8) :
			order == std::memory_order_relaxed ?
			atomic_load_at<std::uint64_t>(pointer,
				std::memory_order_relaxed) :
			atomic_load_at<std::uint64_t>(pointer,
				std::memory_order_seq_cst);
		first = packed & 0xffff'ffffU;
		second = packed >> 32;
		return true;
	}
	if (atomic_translation_failed(memory))
		return false;
	if (!memory.read(address, std::span{bytes}.first(total_size)))
		return false;
	key = reservation_key(memory, address, total_size, MemoryAccess::read);
	if (order != std::memory_order_relaxed)
		std::atomic_thread_fence(order);
	first = arm64::detail::decode_little_endian(
		std::span{bytes}.first(element_size));
	second = arm64::detail::decode_little_endian(
		std::span{bytes}.subspan(element_size, element_size));
	return true;
}

[[nodiscard]] AtomicStoreResult atomic_store_exclusive_pair_value(
	GuestMemory &memory, std::uint64_t address, std::size_t element_size,
	const MonitoredValue &monitored, std::uint64_t first,
	std::uint64_t second, std::memory_order order, std::uintptr_t &key)
{
	const auto total_size = element_size * 2;
	auto *pointer = memory.atomic_pointer(
		address, total_size, MemoryAccess::write);
	if (pointer != nullptr) {
		key = host_reservation_key(pointer);
		if (key != monitored.key)
			return AtomicStoreResult::reservation_mismatch;
		if (total_size == 8) {
			const auto packed = (first & 0xffff'ffffU) | (second << 32);
			const auto observed = naturally_aligned(pointer, 8) ?
				atomic_compare_swap_at<std::uint64_t>(pointer,
					monitored.first, packed, order,
					std::memory_order_relaxed) :
				arm64::detail::lse2_compare_swap(pointer, 8,
					monitored.first, packed);
			return observed == monitored.first ? AtomicStoreResult::stored :
				AtomicStoreResult::reservation_mismatch;
		}
		/* The single-copy rule keeps a 16-byte pair aligned. */
		std::uint64_t observed_first = 0;
		std::uint64_t observed_second = 0;
		atomic_compare_swap_pair_128_at(pointer, monitored.first,
			monitored.second, first, second, observed_first,
			observed_second);
		return observed_first == monitored.first &&
			observed_second == monitored.second ?
			AtomicStoreResult::stored : AtomicStoreResult::reservation_mismatch;
	}
	if (atomic_translation_failed(memory))
		return AtomicStoreResult::fault;

	/* No direct pointer: see atomic_store_exclusive_value. */
	key = reservation_key(
		memory, address, total_size, MemoryAccess::write);
	if (key != monitored.key)
		return AtomicStoreResult::reservation_mismatch;
	std::array<std::byte, 16> bytes{};
	arm64::detail::encode_little_endian(first,
		std::span{bytes}.first(element_size));
	arm64::detail::encode_little_endian(second,
		std::span{bytes}.subspan(element_size, element_size));
	if (order != std::memory_order_relaxed)
		std::atomic_thread_fence(order);
	return memory.write(address, std::span{bytes}.first(total_size)) ?
		AtomicStoreResult::stored : AtomicStoreResult::fault;
}

void invalidate_reservations_for_store_locked(CpuState &writer,
	GuestMemory &memory, std::uint64_t address, std::size_t size)
{
	/* Every successful ordinary store invalidates every reservation granule it
	 * touches, even when the stored bits compare equal to the old value. Keep
	 * direct host aliases and fallback guest-address reservations in the same
	 * namespaces used by the exclusive helpers above.
	 */
	for (std::size_t offset = 0; offset < size;) {
		const auto current = address + offset;
		auto *pointer = memory.reservation_pointer(
			current, 1, MemoryAccess::write);
		reservation_store(writer, pointer != nullptr ?
			host_reservation_key(pointer) :
			fallback_reservation_key(current));
		const auto remaining_in_granule = reservation_granule -
			(static_cast<std::uintptr_t>(current) &
			 (reservation_granule - 1));
		offset += std::min<std::size_t>(
			size - offset, remaining_in_granule);
	}
}

[[nodiscard]] bool ordinary_store(CpuState &writer, GuestMemory &memory,
	std::uint64_t address, std::span<const std::byte> bytes)
{
	/* Serialize the write itself with the exclusive operations. Doing
	 * only the version update under the lock leaves a window in which STXR can
	 * incorrectly succeed after a conflicting ordinary store has committed.
	 */
	std::scoped_lock lock{exclusive_memory_mutex};
	if (!memory.write(address, bytes))
		return false;
	invalidate_reservations_for_store_locked(writer, memory, address,
		bytes.size());
	return true;
}

[[nodiscard]] bool simd_structure_address(CpuState &cpu,
	GuestMemory &memory, std::uint64_t tagged_address,
	std::size_t transfer_size, std::size_t element_size,
	MemoryAccess access, std::uint64_t &address, std::uint64_t &fault_address)
{
	address = cpu.untag_data_address(tagged_address);
	for (std::size_t completed = 0; completed < transfer_size;
	     completed += element_size) {
		const auto current = address + completed;
		const auto access_size =
			std::min(element_size, transfer_size - completed);
		if (!memory.validate_access(current, access_size, access)) {
			fault_address = memory.last_fault().address;
			return false;
		}
	}
	return true;
}

void invalidate_store_ranges(CpuState &writer,
	std::span<const JitStoreRange> ranges) noexcept
{
	for (const auto &range : ranges) {
		for (std::uint64_t offset = 0; offset < range.size;) {
			const auto current = range.address + offset;
			reservation_store(writer, host_reservation_key(
				reinterpret_cast<std::byte *>(current)));
			const auto remaining_in_granule = reservation_granule -
				(static_cast<std::uintptr_t>(current) &
				 (reservation_granule - 1));
			offset += std::min<std::uint64_t>(
				range.size - offset, remaining_in_granule);
		}
	}
}

} // namespace uml::cpu::arm64::jit::semantic
