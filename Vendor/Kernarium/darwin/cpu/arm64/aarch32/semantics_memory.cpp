// SPDX-License-Identifier: GPL-2.0-or-later
//
// The memory families of the AArch32 semantic library: single and multiple
// loads and stores, the exclusives, and the acquire/release group
// (semantics.hpp). Every access forms a 32-bit address; the alignment check
// precedes the translation; a load changes no register before its last
// element is read, and a store writes nothing until every page it touches
// translates (STM and PUSH excepted, which store the words ahead of the one
// that faults).

#include "cpu/arm64/aarch32/semantics_internal.hpp"

#include <atomic>

namespace uml::cpu::arm64::aarch32::detail {

namespace {

using ::uml::memory::MemoryAccess;

struct Single final {
	unsigned size;
	bool load;
	bool sign;
	bool pair;
};

[[nodiscard]] constexpr Single single_of(Op op) noexcept
{
	switch (op) {
	case Op::ldr: case Op::ldrt: return {4, true, false, false};
	case Op::ldrb: case Op::ldrbt: return {1, true, false, false};
	case Op::ldrh: case Op::ldrht: return {2, true, false, false};
	case Op::ldrsb: case Op::ldrsbt: return {1, true, true, false};
	case Op::ldrsh: case Op::ldrsht: return {2, true, true, false};
	case Op::ldrd: return {8, true, false, true};
	case Op::str: case Op::strt: return {4, false, false, false};
	case Op::strb: case Op::strbt: return {1, false, false, false};
	case Op::strh: case Op::strht: return {2, false, false, false};
	case Op::strd: return {8, false, false, true};
	default: return {0, false, false, false};
	}
}

[[nodiscard]] constexpr bool is_unprivileged(Op op) noexcept
{
	switch (op) {
	case Op::ldrt: case Op::ldrbt: case Op::ldrht: case Op::ldrsbt:
	case Op::ldrsht: case Op::strt: case Op::strbt: case Op::strht:
		return true;
	default:
		return false;
	}
}

} // namespace

bool exec_single_transfer(Exec &x, const OpInfo &info)
{
	const Operands &o = x.o;
	if (info.op == Op::prefetch)
		return true;
	const Single kind = single_of(info.op);
	if (kind.size == 0U)
		return x.raise_fatal();

	/* The address, and the base register's value after the access. */
	std::uint32_t address;
	std::uint32_t offset_address = 0;
	bool writeback = false;
	if (info.shape == Shape::literal) {
		const std::uint32_t base = x.pc_aligned();
		const auto offset = static_cast<std::uint32_t>(o.imm32);
		address = o.add ? base + offset : base - offset;
	} else {
		const std::uint32_t base = x.reg(o.n);
		std::uint32_t offset;
		if (is_unprivileged(info.op)) {
			offset = o.register_form
				? shift_c(x.reg(o.m), o.shift_t, static_cast<unsigned>(o.shift_n),
					x.flag_c()).value
				: static_cast<std::uint32_t>(o.imm32);
			offset_address = o.add ? base + offset : base - offset;
			address = o.postindex ? base : offset_address;
			writeback = o.postindex;
		} else {
			offset = info.shape == Shape::reg
				? shift_c(x.reg(o.m), o.shift_t, static_cast<unsigned>(o.shift_n),
					x.flag_c()).value
				: static_cast<std::uint32_t>(o.imm32);
			offset_address = o.add ? base + offset : base - offset;
			address = o.index ? offset_address : base;
			writeback = o.wback;
		}
	}

	if (kind.pair) {
		/* MemA: a doubleword transfer needs word alignment, and a 4-aligned
		 * one is two word accesses. */
		const MemoryAccess access = kind.load ? MemoryAccess::read : MemoryAccess::write;
		if ((address & 3U) != 0U)
			return x.raise_alignment(address, access);
		if (kind.load) {
			std::uint64_t data;
			if (!x.read(address, &data, 8))
				return false;
			if (writeback)
				x.set_reg(o.n, offset_address);
			x.set_reg(o.t, static_cast<std::uint32_t>(data));
			x.set_reg(o.t2, static_cast<std::uint32_t>(data >> 32));
			return true;
		}
		const std::uint64_t data = std::uint64_t{x.reg(o.t2)} << 32 | x.reg(o.t);
		if (!x.write(address, &data, 8))
			return false;
		if (writeback)
			x.set_reg(o.n, offset_address);
		return true;
	}

	if (kind.load) {
		std::uint32_t data = 0;
		if (!x.read(address, &data, kind.size))
			return false;
		if (kind.sign) {
			data = kind.size == 1U
				? static_cast<std::uint32_t>(static_cast<std::int8_t>(data))
				: static_cast<std::uint32_t>(static_cast<std::int16_t>(data));
		}
		if (writeback)
			x.set_reg(o.n, offset_address);
		if (o.t == static_cast<std::int32_t>(reg_pc))
			x.load_write_pc(data);
		else
			x.set_reg(o.t, data);
		return true;
	}

	const std::uint32_t value = x.reg(o.t);
	if (!x.write(address, &value, kind.size))
		return false;
	if (writeback)
		x.set_reg(o.n, offset_address);
	return true;
}

bool exec_multiple_transfer(Exec &x, const OpInfo &info)
{
	const Operands &o = x.o;
	const bool load = info.op == Op::ldm;
	const unsigned n = info.stack ? reg_sp : static_cast<unsigned>(o.n);
	const bool writeback = info.stack ? true : o.wback;
	const unsigned registers = o.registers;
	const unsigned count = static_cast<unsigned>(std::popcount(registers));
	const std::uint32_t base = x.reg(n);
	const std::uint32_t span = 4U * count;
	std::uint32_t start;
	std::uint32_t final_base;
	switch (info.transfer) {
	case Mode::ia: start = base; final_base = base + span; break;
	case Mode::ib: start = base + 4U; final_base = base + span; break;
	case Mode::da: start = base - span + 4U; final_base = base - span; break;
	default: start = base - span; final_base = base - span; break;
	}
	/* MemA: every element needs word alignment; an unaligned block faults
	 * at its first word. */
	if ((start & 3U) != 0U)
		return x.raise_alignment(start,
			load ? MemoryAccess::read : MemoryAccess::write);
	std::array<std::uint32_t, 16> words{};
	if (load) {
		if (!x.read(start, words.data(), span))
			return false;
		unsigned index = 0;
		for (unsigned r = 0; r < 15U; ++r)
			if ((registers >> r & 1U) != 0U)
				x.set_reg(r, words[index++]);
		if (writeback)
			x.set_reg(n, final_base);
		if ((registers >> 15 & 1U) != 0U)
			x.load_write_pc(words[index]);
		return true;
	}
	unsigned index = 0;
	for (unsigned r = 0; r < 16U; ++r)
		if ((registers >> r & 1U) != 0U)
			words[index++] = x.reg(r);
	if (!x.write_words(start, words.data(), count))
		return false;
	if (writeback)
		x.set_reg(n, final_base);
	return true;
}

namespace {

[[nodiscard]] std::uint64_t atomic_load(const std::byte *host, unsigned size,
	int order) noexcept
{
	switch (size) {
	case 1: return __atomic_load_n(reinterpret_cast<const std::uint8_t *>(host), order);
	case 2: return __atomic_load_n(reinterpret_cast<const std::uint16_t *>(host), order);
	case 4: return __atomic_load_n(reinterpret_cast<const std::uint32_t *>(host), order);
	default: return __atomic_load_n(reinterpret_cast<const std::uint64_t *>(host), order);
	}
}

void atomic_store(std::byte *host, unsigned size, std::uint64_t value, int order) noexcept
{
	switch (size) {
	case 1: __atomic_store_n(reinterpret_cast<std::uint8_t *>(host),
			static_cast<std::uint8_t>(value), order); break;
	case 2: __atomic_store_n(reinterpret_cast<std::uint16_t *>(host),
			static_cast<std::uint16_t>(value), order); break;
	case 4: __atomic_store_n(reinterpret_cast<std::uint32_t *>(host),
			static_cast<std::uint32_t>(value), order); break;
	default: __atomic_store_n(reinterpret_cast<std::uint64_t *>(host), value, order); break;
	}
}

/* A host compare-and-swap of the recorded value, true on success. */
[[nodiscard]] bool atomic_swap(std::byte *host, unsigned size, std::uint64_t expected,
	std::uint64_t desired, int order) noexcept
{
	switch (size) {
	case 1: {
		auto want = static_cast<std::uint8_t>(expected);
		return __atomic_compare_exchange_n(reinterpret_cast<std::uint8_t *>(host), &want,
			static_cast<std::uint8_t>(desired), false, order, __ATOMIC_RELAXED);
	}
	case 2: {
		auto want = static_cast<std::uint16_t>(expected);
		return __atomic_compare_exchange_n(reinterpret_cast<std::uint16_t *>(host), &want,
			static_cast<std::uint16_t>(desired), false, order, __ATOMIC_RELAXED);
	}
	case 4: {
		auto want = static_cast<std::uint32_t>(expected);
		return __atomic_compare_exchange_n(reinterpret_cast<std::uint32_t *>(host), &want,
			static_cast<std::uint32_t>(desired), false, order, __ATOMIC_RELAXED);
	}
	default: {
		auto want = expected;
		return __atomic_compare_exchange_n(reinterpret_cast<std::uint64_t *>(host), &want,
			desired, false, order, __ATOMIC_RELAXED);
	}
	}
}

struct Exclusive final {
	unsigned size;
	bool load;
	/* LDAEX, STLEX, LDA, STL: ordered; the plain LDA and STL do not use
	 * the monitor. */
	bool ordered;
	bool monitor;
};

[[nodiscard]] constexpr Exclusive exclusive_of(Op op) noexcept
{
	switch (op) {
	case Op::ldrex: return {4, true, false, true};
	case Op::ldrexb: return {1, true, false, true};
	case Op::ldrexh: return {2, true, false, true};
	case Op::ldrexd: return {8, true, false, true};
	case Op::strex: return {4, false, false, true};
	case Op::strexb: return {1, false, false, true};
	case Op::strexh: return {2, false, false, true};
	case Op::strexd: return {8, false, false, true};
	case Op::ldaex: return {4, true, true, true};
	case Op::ldaexb: return {1, true, true, true};
	case Op::ldaexh: return {2, true, true, true};
	case Op::ldaexd: return {8, true, true, true};
	case Op::stlex: return {4, false, true, true};
	case Op::stlexb: return {1, false, true, true};
	case Op::stlexh: return {2, false, true, true};
	case Op::stlexd: return {8, false, true, true};
	case Op::lda: return {4, true, true, false};
	case Op::ldab: return {1, true, true, false};
	case Op::ldah: return {2, true, true, false};
	case Op::stl: return {4, false, true, false};
	case Op::stlb: return {1, false, true, false};
	case Op::stlh: return {2, false, true, false};
	default: return {0, false, false, false};
	}
}

} // namespace

bool exec_exclusive(Exec &x, const OpInfo &info)
{
	const Operands &o = x.o;
	if (info.op == Op::clrex) {
		x.mem.clear_exclusive(x.cpu);
		return true;
	}
	const Exclusive kind = exclusive_of(info.op);
	if (kind.size == 0U)
		return x.raise_fatal();
	const unsigned size = kind.size;
	const std::uint32_t address = x.reg(o.n) + static_cast<std::uint32_t>(o.imm32);
	const MemoryAccess access = kind.load ? MemoryAccess::read : MemoryAccess::write;
	/* The access must be aligned to its size. */
	if ((address & (size - 1U)) != 0U)
		return x.raise_alignment(address, access);
	std::byte *host = x.atomic_pointer(address, size, access);
	if (host == nullptr)
		return false;

	if (kind.load) {
		const std::uint64_t value = kind.monitor
			? x.mem.load_exclusive(x.cpu, address, host, size, kind.ordered)
			: atomic_load(host, size, __ATOMIC_ACQUIRE);
		x.set_reg(o.t, static_cast<std::uint32_t>(value));
		if (size == 8U)
			x.set_reg(o.t2, static_cast<std::uint32_t>(value >> 32));
		return true;
	}

	std::uint64_t value = x.reg(o.t);
	if (size == 8U)
		value |= std::uint64_t{x.reg(o.t2)} << 32;
	if (!kind.monitor) {
		/* STL: a release store. */
		atomic_store(host, size, value, __ATOMIC_RELEASE);
		x.note_stored(host, size);
		return true;
	}
	const bool passed = x.mem.store_exclusive(x.cpu, address, host, size, value, kind.ordered);
	if (passed)
		x.note_stored(host, size);
	x.set_reg(o.d, passed ? 0U : 1U);
	return true;
}

} // namespace uml::cpu::arm64::aarch32::detail

namespace uml::cpu::arm64::aarch32 {

/* The default monitor: CpuState's record and a compare-and-swap. */
std::uint64_t A32Memory::load_exclusive(CpuState &cpu, std::uint32_t address,
	std::byte *host, unsigned size, bool acquire) noexcept
{
	const std::uint64_t value = detail::atomic_load(host, size,
		acquire ? __ATOMIC_ACQUIRE : __ATOMIC_RELAXED);
	cpu.exclusive_address = address;
	cpu.exclusive_key = reinterpret_cast<std::uintptr_t>(host);
	cpu.exclusive_value = value;
	cpu.exclusive_value2 = 0;
	cpu.exclusive_version = 0;
	cpu.exclusive_size = static_cast<std::uint8_t>(size);
	return value;
}

/* A store-exclusive passes when the monitor holds this address and the
 * location still holds the value the load-exclusive read: one host
 * compare-and-swap. The monitor is cleared either way. */
bool A32Memory::store_exclusive(CpuState &cpu, std::uint32_t address, std::byte *host,
	unsigned size, std::uint64_t value, bool release) noexcept
{
	const bool held = cpu.exclusive_size == size && cpu.exclusive_address == address
		&& cpu.exclusive_key == reinterpret_cast<std::uintptr_t>(host);
	const std::uint64_t expected = cpu.exclusive_value;
	cpu.clear_exclusive_monitor();
	return held && detail::atomic_swap(host, size, expected, value,
		release ? __ATOMIC_RELEASE : __ATOMIC_RELAXED);
}

void A32Memory::clear_exclusive(CpuState &cpu) noexcept
{
	cpu.clear_exclusive_monitor();
}

} // namespace uml::cpu::arm64::aarch32
