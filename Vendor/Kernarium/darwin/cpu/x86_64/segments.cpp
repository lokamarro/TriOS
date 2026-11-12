// SPDX-License-Identifier: GPL-2.0-or-later
#include "segments.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace uml::cpu::x86_64 {
namespace {

constexpr std::size_t gdt_entries = 16;
constexpr std::size_t ldt_entries = 8192;
constexpr std::size_t tls_first = 12;
constexpr std::size_t ldt_entry = 10;
constexpr std::uint16_t ldt_selector = ldt_entry << 3U;
constexpr std::uint16_t tss_selector = 8U << 3U;
/* Every access the engines check inline is at most this many bytes past
 * its offset; a larger one leaves for the exact check. */
constexpr std::uint64_t inline_access_slack = 63;
constexpr std::uint64_t never_low = std::uint64_t{1} << 32U;

/* The GDT Linux gives every task on an x86-64 kernel: the kernel's flat
 * segments at privilege 0; the 32-bit user code, user data and 64-bit user
 * code segments; the TSS; the LDT while the task has one; the three TLS
 * entries, which `tls_descriptors` holds; and the per-CPU segment, a
 * read-only expand-down data segment whose limit is the CPU number, which
 * is 0 for every guest vCPU, as RDPID reports. */
constexpr std::array<std::uint64_t, gdt_entries> fixed_gdt{
	0,
	0x00cf9b000000ffffULL,
	0x00af9b000000ffffULL,
	0x00cf93000000ffffULL,
	0x00cffb000000ffffULL,
	0x00cff3000000ffffULL,
	0x00affb000000ffffULL,
	0,
	0x00008b0000000067ULL,
	0,
	0,
	0,
	0,
	0,
	0,
	0x0040f50000000000ULL,
};
constexpr std::uint64_t ldt_system_descriptor = 0x000082000000ffffULL;

[[nodiscard]] constexpr std::uint32_t descriptor_base(
	std::uint64_t descriptor) noexcept
{
	return static_cast<std::uint32_t>(((descriptor >> 16U) & 0xffffffU) |
		((descriptor >> 32U) & 0xff000000U));
}

[[nodiscard]] constexpr std::uint32_t descriptor_limit(
	std::uint64_t descriptor) noexcept
{
	const auto raw = static_cast<std::uint32_t>((descriptor & 0xffffU) |
		((descriptor >> 32U) & 0xf0000U));
	return ((descriptor >> 55U) & 1U) != 0 ? (raw << 12U) | 0xfffU : raw;
}

[[nodiscard]] constexpr unsigned descriptor_type(std::uint64_t d) noexcept
{
	return static_cast<unsigned>((d >> 40U) & 0xfU);
}

[[nodiscard]] constexpr bool descriptor_system(std::uint64_t d) noexcept
{
	return ((d >> 44U) & 1U) == 0;
}

[[nodiscard]] constexpr unsigned descriptor_dpl(std::uint64_t d) noexcept
{
	return static_cast<unsigned>((d >> 45U) & 3U);
}

[[nodiscard]] constexpr bool descriptor_present(std::uint64_t d) noexcept
{
	return ((d >> 47U) & 1U) != 0;
}

[[nodiscard]] constexpr bool descriptor_code(std::uint64_t d) noexcept
{
	return !descriptor_system(d) && (descriptor_type(d) & 8U) != 0;
}

[[nodiscard]] constexpr bool descriptor_data(std::uint64_t d) noexcept
{
	return !descriptor_system(d) && (descriptor_type(d) & 8U) == 0;
}

[[nodiscard]] constexpr bool descriptor_conforming(std::uint64_t d) noexcept
{
	return descriptor_code(d) && (descriptor_type(d) & 4U) != 0;
}

[[nodiscard]] constexpr bool descriptor_readable(std::uint64_t d) noexcept
{
	return descriptor_data(d) || (descriptor_code(d) &&
		(descriptor_type(d) & 2U) != 0);
}

[[nodiscard]] constexpr bool descriptor_writable(std::uint64_t d) noexcept
{
	return descriptor_data(d) && (descriptor_type(d) & 2U) != 0;
}

[[nodiscard]] constexpr bool descriptor_long(std::uint64_t d) noexcept
{
	return ((d >> 53U) & 1U) != 0;
}

[[nodiscard]] constexpr bool descriptor_big(std::uint64_t d) noexcept
{
	return ((d >> 54U) & 1U) != 0;
}

[[nodiscard]] constexpr std::uint32_t selector_error(
	std::uint16_t selector) noexcept
{
	return selector & 0xfffcU;
}

[[nodiscard]] constexpr bool null_selector(std::uint16_t selector) noexcept
{
	return (selector & 0xfffcU) == 0;
}

[[nodiscard]] constexpr SegmentFault general_protection(
	std::uint32_t error = 0) noexcept
{
	return {general_protection_vector, error};
}

[[nodiscard]] constexpr std::size_t index_of(SegmentRegister reg) noexcept
{
	return static_cast<std::size_t>(reg);
}

/* The descriptor `selector` names, or nothing for a null selector and for
 * one past its table's limit. An LDT slot the task never filled reads as
 * zero, a system descriptor nothing accepts -- the fault and the answer of
 * a selector past Linux's own LDT limit. */
[[nodiscard]] std::optional<std::uint64_t> descriptor(const CpuState &cpu,
	std::uint16_t selector) noexcept
{
	const std::size_t index = selector >> 3U;
	if ((selector & 4U) != 0) {
		if (cpu.ldt == nullptr || index >= ldt_entries)
			return std::nullopt;
		return __atomic_load_n(cpu.ldt + index, __ATOMIC_RELAXED);
	}
	if (index == 0 || index >= gdt_entries)
		return std::nullopt;
	if (index >= tls_first && index < tls_first + cpu.tls_descriptors.size())
		return cpu.tls_descriptors[index - tls_first];
	if (index == ldt_entry)
		return cpu.ldt != nullptr ? ldt_system_descriptor : 0;
	return fixed_gdt[index];
}

[[nodiscard]] SegmentCache cache_of(std::uint64_t descriptor) noexcept
{
	SegmentCache cache{};
	cache.base = descriptor_base(descriptor);
	cache.limit = descriptor_limit(descriptor);
	cache.attributes = static_cast<std::uint16_t>(descriptor >> 40U);
	cache.usable = true;
	return cache;
}

/* The descriptor a cache came from, enough of it for the predicates. */
[[nodiscard]] constexpr std::uint64_t cache_descriptor(
	const SegmentCache &cache) noexcept
{
	return static_cast<std::uint64_t>(cache.attributes) << 40U;
}

void compute_check(SegmentCache &cache) noexcept
{
	cache.check_low = never_low;
	cache.check_span = 0;
	const auto d = cache_descriptor(cache);
	/* Only a writable data segment checks inline: an access through any
	 * other may be one its type refuses, which the exact check decides. */
	if (!cache.usable || !descriptor_writable(d))
		return;
	/* A segment spanning all 4 GiB of offsets refuses none: the i386 TLS
	 * sequences address their variables at offsets just below 4 GiB. */
	if ((descriptor_type(d) & 4U) == 0 && cache.limit == 0xffffffffU) {
		cache.check_low = 0;
		cache.check_span = ~std::uint64_t{};
		return;
	}
	if ((descriptor_type(d) & 4U) != 0) {
		const std::uint64_t upper = descriptor_big(d) ? 0xffffffffU : 0xffffU;
		if (cache.limit >= upper)
			return;
		const std::uint64_t room = upper - cache.limit - 1U;
		if (room < inline_access_slack)
			return;
		cache.check_low = std::uint64_t{cache.limit} + 1U;
		cache.check_span = room - inline_access_slack;
		return;
	}
	if (cache.limit < inline_access_slack)
		return;
	cache.check_low = 0;
	cache.check_span = cache.limit - inline_access_slack;
}

[[nodiscard]] bool flat_data(const SegmentCache &cache) noexcept
{
	const auto d = cache_descriptor(cache);
	return cache.usable && descriptor_writable(d) &&
		(descriptor_type(d) & 4U) == 0 && cache.base == 0 &&
		cache.limit == 0xffffffffU;
}

[[nodiscard]] ExecutionMode mode_of(const SegmentCache &code) noexcept
{
	return descriptor_long(cache_descriptor(code)) ?
		ExecutionMode::long64 : ExecutionMode::compat32;
}

/* The checks of a load of DS, ES, FS or GS: a null selector loads an
 * unusable segment; anything else must be a present data or readable code
 * segment CPL 3 may use. */
[[nodiscard]] SegmentFault check_data_load(const CpuState &cpu,
	std::uint16_t selector, std::optional<std::uint64_t> &loaded) noexcept
{
	loaded.reset();
	if ((selector & 4U) == 0 && null_selector(selector))
		return {};
	const auto found = descriptor(cpu, selector);
	if (!found)
		return general_protection(selector_error(selector));
	const auto d = *found;
	if (descriptor_system(d) || !descriptor_readable(d))
		return general_protection(selector_error(selector));
	if (!descriptor_conforming(d) &&
		(descriptor_dpl(d) < 3U || descriptor_dpl(d) < (selector & 3U)))
		return general_protection(selector_error(selector));
	if (!descriptor_present(d))
		return {segment_not_present_vector, selector_error(selector)};
	loaded = d;
	return {};
}

/* The checks of a load of SS: a writable data segment of privilege 3 named
 * with RPL 3; a null selector, which CPL 3 may not load, raises #GP(0). */
[[nodiscard]] SegmentFault check_stack_load(const CpuState &cpu,
	std::uint16_t selector, std::uint64_t &loaded) noexcept
{
	if ((selector & 4U) == 0 && null_selector(selector))
		return general_protection();
	const auto found = descriptor(cpu, selector);
	if (!found)
		return general_protection(selector_error(selector));
	const auto d = *found;
	if ((selector & 3U) != 3U || !descriptor_writable(d) ||
		descriptor_dpl(d) != 3U)
		return general_protection(selector_error(selector));
	if (!descriptor_present(d))
		return {stack_segment_vector, selector_error(selector)};
	loaded = d;
	return {};
}

enum class CodeTransfer : std::uint8_t {
	/* JMP or CALL: RPL may be anything; the new CPL is 3. */
	call,
	/* RET or IRET: the selector's RPL is the new CPL and must be 3. */
	ret,
};

/* The checks of a far transfer's CS. A code segment must be flat and 32-
 * or 64-bit: Linux refuses 16-bit segments, and one with a base or a limit
 * is a limitation of this machine, which raises #GP for it. */
[[nodiscard]] SegmentFault check_code_load(const CpuState &cpu,
	std::uint16_t selector, CodeTransfer transfer,
	std::uint64_t &loaded) noexcept
{
	if ((selector & 4U) == 0 && null_selector(selector))
		return general_protection();
	const auto found = descriptor(cpu, selector);
	if (!found)
		return general_protection(selector_error(selector));
	const auto d = *found;
	if (!descriptor_code(d))
		return general_protection(selector_error(selector));
	if (transfer == CodeTransfer::ret && (selector & 3U) != 3U)
		return general_protection(selector_error(selector));
	if (descriptor_conforming(d) ? descriptor_dpl(d) > 3U :
		descriptor_dpl(d) != 3U)
		return general_protection(selector_error(selector));
	if (!descriptor_present(d))
		return {segment_not_present_vector, selector_error(selector)};
	if ((descriptor_long(d) && descriptor_big(d)) ||
		(!descriptor_long(d) && !descriptor_big(d)) ||
		descriptor_base(d) != 0 || descriptor_limit(d) != 0xffffffffU)
		return general_protection(selector_error(selector));
	loaded = d;
	return {};
}

[[nodiscard]] constexpr bool canonical(std::uint64_t address) noexcept
{
	return static_cast<std::uint64_t>(
		static_cast<std::int64_t>(address << 16U) >> 16U) == address;
}

void set_zero_flag(CpuState &cpu, bool value) noexcept
{
	materialize_flags(cpu);
	if (value)
		cpu.rflags |= rflags_zero;
	else
		cpu.rflags &= ~rflags_zero;
}

/* A general register written at `width`: a 32-bit write zero-extends, a
 * narrower one keeps the bits above it. */
void write_register(CpuState &cpu, const Operand &operand,
	std::uint64_t value) noexcept
{
	auto &target = cpu.registers[operand.reg.index & 15U];
	switch (operand.width) {
	case OperandWidth::word:
		target = (target & ~std::uint64_t{0xffff}) | (value & 0xffffU);
		break;
	case OperandWidth::dword:
		target = static_cast<std::uint32_t>(value);
		break;
	default:
		target = value;
		break;
	}
}

[[nodiscard]] constexpr std::uint64_t width_mask(unsigned bytes) noexcept
{
	return bytes >= 8 ? ~std::uint64_t{} :
		(std::uint64_t{1} << (bytes * 8U)) - 1U;
}

/* One instruction's segment-checked accesses to guest memory. */
class Accesses final {
public:
	Accesses(CpuState &cpu, SegmentMemory &memory,
		const Instruction &instruction) noexcept :
		cpu_(cpu), memory_(memory), instruction_(instruction)
	{
	}

	[[nodiscard]] const SegmentResult &result() const noexcept
	{
		return result_;
	}

	[[nodiscard]] bool compat() const noexcept
	{
		return cpu_.execution_mode == ExecutionMode::compat32;
	}

	/* The offset a memory operand names, at its address size. */
	[[nodiscard]] std::uint64_t offset(const Operand &operand) const noexcept
	{
		const auto &memory = operand.memory;
		std::uint64_t address{};
		if (memory.absolute_address) {
			address = memory.absolute;
		} else if (memory.rip_relative) {
			address = cpu_.rip + instruction_.length +
				static_cast<std::uint64_t>(
					static_cast<std::int64_t>(memory.displacement));
		} else {
			address = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(memory.displacement));
			if (memory.base != MemoryOperand::no_register)
				address += cpu_.registers[memory.base & 15U];
			if (memory.index != MemoryOperand::no_register &&
				!memory.vector_index)
				address += cpu_.registers[memory.index & 15U] *
					memory.scale;
		}
		if (memory.address_16)
			return address & 0xffffU;
		if (memory.address_32)
			return address & 0xffffffffU;
		return address;
	}

	/* Checks an access of `size` bytes at `offset` through `reg`, and
	 * forms its linear address. */
	[[nodiscard]] bool linear(SegmentRegister reg, std::uint64_t offset,
		std::size_t size, bool write, std::uint64_t &address) noexcept
	{
		const bool stack = reg == SegmentRegister::ss;
		if (!compat()) {
			address = offset;
			if (reg == SegmentRegister::fs)
				address += cpu_.fs_base;
			else if (reg == SegmentRegister::gs)
				address += cpu_.gs_base;
			if (!canonical(address) ||
				!canonical(address + size - 1U))
				return fail(stack ?
					SegmentFault{stack_segment_vector, 0} :
					general_protection());
			return true;
		}
		const auto &cache = cpu_.segments[index_of(reg)];
		const auto d = cache_descriptor(cache);
		const SegmentFault fault = stack ?
			SegmentFault{stack_segment_vector, 0} : general_protection();
		if (!cache.usable || (write && !descriptor_writable(d)) ||
			(!write && !descriptor_readable(d)))
			return fail(general_protection());
		const auto last = offset + size - 1U;
		if (descriptor_data(d) && (descriptor_type(d) & 4U) != 0) {
			const std::uint64_t upper = descriptor_big(d) ?
				0xffffffffU : 0xffffU;
			if (offset <= cache.limit || last > upper)
				return fail(fault);
		} else if (last > cache.limit && cache.limit != 0xffffffffU) {
			/* Intel leaves an access that wraps the offsets of a 4 GiB
			 * segment implementation-specific; it passes here, as the
			 * inline check passes it. */
			return fail(fault);
		}
		std::uint64_t base = cache.base;
		if (reg == SegmentRegister::fs)
			base = cpu_.fs_base;
		else if (reg == SegmentRegister::gs)
			base = cpu_.gs_base;
		address = static_cast<std::uint32_t>(base + offset);
		return true;
	}

	/* Reads or writes `data` at `offset` through `reg`. In compatibility
	 * mode the linear address wraps at 4 GiB, so an access that crosses it
	 * is two. */
	[[nodiscard]] bool read(SegmentRegister reg, std::uint64_t offset,
		std::span<std::byte> data) noexcept
	{
		std::uint64_t address{};
		if (!linear(reg, offset, data.size(), false, address))
			return false;
		return transfer(address, data.data(), data.size(), false);
	}

	[[nodiscard]] bool write(SegmentRegister reg, std::uint64_t offset,
		std::span<const std::byte> data) noexcept
	{
		std::uint64_t address{};
		if (!linear(reg, offset, data.size(), true, address))
			return false;
		return transfer(address, const_cast<std::byte *>(data.data()),
			data.size(), true);
	}

	[[nodiscard]] bool read_value(SegmentRegister reg, std::uint64_t offset,
		unsigned bytes, std::uint64_t &value) noexcept
	{
		std::array<std::byte, 8> data{};
		if (!read(reg, offset, std::span{data}.first(bytes)))
			return false;
		value = 0;
		std::memcpy(&value, data.data(), bytes);
		return true;
	}

	/* An operand's value: a register, an immediate or memory. */
	[[nodiscard]] bool operand_value(const Operand &operand, unsigned bytes,
		std::uint64_t &value) noexcept
	{
		switch (operand.kind) {
		case OperandKind::reg:
			value = cpu_.registers[operand.reg.index & 15U] &
				width_mask(bytes);
			return true;
		case OperandKind::immediate:
			value = operand.immediate & width_mask(bytes);
			return true;
		case OperandKind::memory:
			return read_value(operand_segment(operand.memory),
				offset(operand), bytes, value);
		default:
			return fail(general_protection());
		}
	}

	[[nodiscard]] bool fail(SegmentFault fault) noexcept
	{
		result_.outcome = SegmentOutcome::fault;
		result_.fault = fault;
		return false;
	}

	/* The stack's address size, and its pointer at `adjust` bytes from
	 * RSP, wrapped. */
	[[nodiscard]] std::uint64_t stack_offset(std::int64_t adjust) const noexcept
	{
		const auto value = cpu_.gpr(Register::rsp) +
			static_cast<std::uint64_t>(adjust);
		return compat() ? static_cast<std::uint32_t>(value) : value;
	}

	[[nodiscard]] bool stack_read(std::int64_t adjust, unsigned bytes,
		std::uint64_t &value) noexcept
	{
		return read_value(SegmentRegister::ss, stack_offset(adjust), bytes,
			value);
	}

	[[nodiscard]] bool stack_write(std::int64_t adjust, unsigned bytes,
		std::uint64_t value) noexcept
	{
		std::array<std::byte, 8> data{};
		std::memcpy(data.data(), &value, bytes);
		return write(SegmentRegister::ss, stack_offset(adjust),
			std::span<const std::byte>{data}.first(bytes));
	}

	/* Every byte of an access the stack will make, checked before any of
	 * it is written, so a far CALL's pushes are all or nothing. */
	[[nodiscard]] bool stack_probe(std::int64_t adjust, unsigned bytes,
		bool write) noexcept
	{
		std::uint64_t address{};
		return linear(SegmentRegister::ss, stack_offset(adjust), bytes,
			write, address);
	}

private:
	[[nodiscard]] bool transfer(std::uint64_t address, std::byte *data,
		std::size_t size, bool write) noexcept
	{
		std::size_t first = size;
		if (compat() && address + size > (std::uint64_t{1} << 32U))
			first = static_cast<std::size_t>(
				(std::uint64_t{1} << 32U) - address);
		const auto move = [&](std::uint64_t at, std::byte *bytes,
			std::size_t count) noexcept {
			if (count == 0)
				return true;
			return write ? memory_.write(at,
				std::span<const std::byte>{bytes, count}) :
				memory_.read(at, std::span<std::byte>{bytes, count});
		};
		if (move(address, data, first) &&
			move(0, data + first, size - first))
			return true;
		result_.outcome = SegmentOutcome::memory_fault;
		return false;
	}

	CpuState &cpu_;
	SegmentMemory &memory_;
	const Instruction &instruction_;
	SegmentResult result_{};
};

/* Loads `reg` from `selector` after the checks a selector load makes,
 * which `descriptor` already passed. */
void commit_data(CpuState &cpu, SegmentRegister reg, std::uint16_t selector,
	std::optional<std::uint64_t> descriptor) noexcept
{
	auto &cache = cpu.segments[index_of(reg)];
	cpu.segment_selectors[index_of(reg)] = selector;
	if (descriptor)
		cache = cache_of(*descriptor);
	else
		cache = SegmentCache{};
	/* A load of FS or GS sets its base from the descriptor: zero for a
	 * null selector, as Intel processors load it. */
	const std::uint64_t base = descriptor ? descriptor_base(*descriptor) : 0;
	if (reg == SegmentRegister::fs)
		cpu.fs_base = base;
	else if (reg == SegmentRegister::gs)
		cpu.gs_base = base;
}

[[nodiscard]] SegmentFault load_selector(CpuState &cpu, SegmentRegister reg,
	std::uint16_t selector) noexcept
{
	if (reg == SegmentRegister::ss) {
		std::uint64_t loaded{};
		if (const auto fault = check_stack_load(cpu, selector, loaded))
			return fault;
		cpu.segment_selectors[index_of(reg)] = selector;
		cpu.segments[index_of(reg)] = cache_of(loaded);
		return {};
	}
	std::optional<std::uint64_t> loaded;
	if (const auto fault = check_data_load(cpu, selector, loaded))
		return fault;
	commit_data(cpu, reg, selector, loaded);
	return {};
}

void commit_code(CpuState &cpu, std::uint16_t selector,
	std::uint64_t descriptor) noexcept
{
	cpu.segment_selectors[index_of(SegmentRegister::cs)] =
		static_cast<std::uint16_t>((selector & ~3U) | 3U);
	cpu.segments[index_of(SegmentRegister::cs)] = cache_of(descriptor);
	cpu.execution_mode = mode_of(cpu.segments[index_of(SegmentRegister::cs)]);
}

[[nodiscard]] SegmentResult finish(CpuState &cpu, ExecutionMode mode,
	bool flat) noexcept
{
	refresh_segment_checks(cpu);
	return {cpu.execution_mode != mode || cpu.flat_segments != flat ?
		SegmentOutcome::retired_changed : SegmentOutcome::retired, {},
		false};
}

[[nodiscard]] std::uint64_t next_rip(const CpuState &cpu,
	const Instruction &instruction) noexcept
{
	return advance_instruction_pointer(cpu.execution_mode, cpu.rip,
		instruction.length);
}

[[nodiscard]] unsigned operand_bytes(OperandWidth width) noexcept
{
	return static_cast<unsigned>(width);
}

/* The target of a far transfer, at the new mode: a compatibility-mode
 * offset is 32 bits and inside the flat code segment; a 64-bit one must be
 * canonical. */
[[nodiscard]] bool far_target(std::uint64_t descriptor, std::uint64_t offset,
	std::uint64_t &rip) noexcept
{
	if (descriptor_long(descriptor)) {
		if (!canonical(offset))
			return false;
		rip = offset;
		return true;
	}
	rip = static_cast<std::uint32_t>(offset);
	return true;
}

[[nodiscard]] SegmentResult far_jump_or_call(CpuState &cpu,
	SegmentMemory &memory, const Instruction &instruction) noexcept
{
	Accesses accesses{cpu, memory, instruction};
	const auto mode = cpu.execution_mode;
	const bool flat = cpu.flat_segments;
	std::uint64_t offset{};
	std::uint64_t selector{};
	unsigned bytes{};
	if (instruction.source.kind == OperandKind::immediate) {
		bytes = operand_bytes(instruction.source.width);
		offset = instruction.source.immediate & width_mask(bytes);
		selector = instruction.auxiliary.immediate & 0xffffU;
	} else {
		bytes = operand_bytes(instruction.source.width);
		const auto segment = operand_segment(instruction.source.memory);
		const auto at = accesses.offset(instruction.source);
		if (!accesses.read_value(segment, at, bytes, offset))
			return accesses.result();
		const auto selector_at = accesses.compat() ?
			static_cast<std::uint32_t>(at + bytes) : at + bytes;
		if (!accesses.read_value(segment, selector_at, 2, selector))
			return accesses.result();
	}
	std::uint64_t loaded{};
	const auto code = static_cast<std::uint16_t>(selector);
	if (const auto fault = check_code_load(cpu, code, CodeTransfer::call,
		loaded))
		return {SegmentOutcome::fault, fault, false};
	std::uint64_t target{};
	if (!far_target(loaded, offset, target))
		return {SegmentOutcome::fault, general_protection(), false};
	if (instruction.operation == Operation::far_call) {
		/* CS, then the return offset, at the operand size, through the
		 * stack of the mode the call leaves. A 32-bit push writes CS
		 * zero-extended. */
		const auto size = static_cast<std::int64_t>(bytes);
		if (!accesses.stack_probe(-2 * size, 2U * bytes, true))
			return accesses.result();
		const auto old_cs = cpu.segment_selectors[index_of(
			SegmentRegister::cs)];
		const auto ret = next_rip(cpu, instruction) & width_mask(bytes);
		if (!accesses.stack_write(-size, bytes, old_cs) ||
			!accesses.stack_write(-2 * size, bytes, ret))
			return accesses.result();
		cpu.gpr(Register::rsp) = accesses.stack_offset(-2 * size);
	}
	commit_code(cpu, code, loaded);
	cpu.rip = target;
	return finish(cpu, mode, flat);
}

[[nodiscard]] SegmentResult far_return(CpuState &cpu, SegmentMemory &memory,
	const Instruction &instruction) noexcept
{
	Accesses accesses{cpu, memory, instruction};
	const auto mode = cpu.execution_mode;
	const bool flat = cpu.flat_segments;
	const auto bytes = operand_bytes(instruction.destination.width);
	const auto size = static_cast<std::int64_t>(bytes);
	std::uint64_t offset{};
	std::uint64_t selector{};
	if (!accesses.stack_read(0, bytes, offset) ||
		!accesses.stack_read(size, bytes, selector))
		return accesses.result();
	std::uint64_t loaded{};
	const auto code = static_cast<std::uint16_t>(selector);
	if (const auto fault = check_code_load(cpu, code, CodeTransfer::ret,
		loaded))
		return {SegmentOutcome::fault, fault, false};
	std::uint64_t target{};
	if (!far_target(loaded, offset, target))
		return {SegmentOutcome::fault, general_protection(), false};
	const auto adjustment = instruction.source.kind == OperandKind::immediate ?
		static_cast<std::int64_t>(instruction.source.immediate & 0xffffU) : 0;
	cpu.gpr(Register::rsp) = accesses.stack_offset(2 * size + adjustment);
	commit_code(cpu, code, loaded);
	cpu.rip = target;
	return finish(cpu, mode, flat);
}

[[nodiscard]] SegmentResult interrupt_return(CpuState &cpu,
	SegmentMemory &memory, const Instruction &instruction) noexcept
{
	Accesses accesses{cpu, memory, instruction};
	const auto mode = cpu.execution_mode;
	const bool flat = cpu.flat_segments;
	const auto bytes = operand_bytes(instruction.destination.width);
	const auto size = static_cast<std::int64_t>(bytes);
	const bool long_mode = !accesses.compat();
	std::uint64_t offset{};
	std::uint64_t selector{};
	std::uint64_t flags{};
	std::uint64_t stack{};
	std::uint64_t stack_selector{};
	if (!accesses.stack_read(0, bytes, offset) ||
		!accesses.stack_read(size, bytes, selector) ||
		!accesses.stack_read(2 * size, bytes, flags))
		return accesses.result();
	/* 64-bit mode pops SS:RSP whatever the privilege; compatibility mode
	 * at CPL 3 never returns to an outer level, so it pops three slots. */
	if (long_mode && (!accesses.stack_read(3 * size, bytes, stack) ||
		!accesses.stack_read(4 * size, bytes, stack_selector)))
		return accesses.result();
	std::uint64_t loaded{};
	const auto code = static_cast<std::uint16_t>(selector);
	if (const auto fault = check_code_load(cpu, code, CodeTransfer::ret,
		loaded))
		return {SegmentOutcome::fault, fault, false};
	std::uint64_t stack_descriptor{};
	const auto stack_code = static_cast<std::uint16_t>(stack_selector);
	if (long_mode) {
		if (const auto fault = check_stack_load(cpu, stack_code,
			stack_descriptor))
			return {SegmentOutcome::fault, fault, false};
	}
	std::uint64_t target{};
	if (!far_target(loaded, offset, target))
		return {SegmentOutcome::fault, general_protection(), false};
	/* At CPL 3 IRET loads what POPF loads, plus RF; IF, IOPL, VM, VIF and
	 * VIP keep their values. Setting TF arms #DB after the next
	 * instruction. */
	materialize_flags(cpu);
	const std::uint64_t mask = bytes == 2 ? 0x4dd5U : 0x254dd5U;
	const auto before = cpu.rflags;
	cpu.rflags = (before & ~mask) | (flags & mask) | rflags_reserved;
	const bool trap = (cpu.rflags & ~before & rflags_trap) != 0;
	if (long_mode) {
		cpu.gpr(Register::rsp) = stack & width_mask(bytes);
		cpu.segment_selectors[index_of(SegmentRegister::ss)] = stack_code;
		cpu.segments[index_of(SegmentRegister::ss)] =
			cache_of(stack_descriptor);
	} else {
		cpu.gpr(Register::rsp) = accesses.stack_offset(3 * size);
	}
	commit_code(cpu, code, loaded);
	cpu.rip = target;
	auto result = finish(cpu, mode, flat);
	result.trap_armed = trap;
	return result;
}

[[nodiscard]] SegmentResult load_segment_register(CpuState &cpu,
	SegmentMemory &memory, const Instruction &instruction) noexcept
{
	Accesses accesses{cpu, memory, instruction};
	const auto mode = cpu.execution_mode;
	const bool flat = cpu.flat_segments;
	const auto reg = static_cast<SegmentRegister>(instruction.vector_group);
	std::uint64_t selector{};
	if (!accesses.operand_value(instruction.source, 2, selector))
		return accesses.result();
	if (const auto fault = load_selector(cpu, reg,
		static_cast<std::uint16_t>(selector)))
		return {SegmentOutcome::fault, fault, false};
	cpu.rip = next_rip(cpu, instruction);
	return finish(cpu, mode, flat);
}

[[nodiscard]] SegmentResult pop_segment_register(CpuState &cpu,
	SegmentMemory &memory, const Instruction &instruction) noexcept
{
	Accesses accesses{cpu, memory, instruction};
	const auto mode = cpu.execution_mode;
	const bool flat = cpu.flat_segments;
	const auto reg = static_cast<SegmentRegister>(instruction.vector_group);
	const auto bytes = operand_bytes(instruction.destination.width);
	std::uint64_t selector{};
	if (!accesses.stack_read(0, bytes, selector))
		return accesses.result();
	const auto rsp = accesses.stack_offset(static_cast<std::int64_t>(bytes));
	if (const auto fault = load_selector(cpu, reg,
		static_cast<std::uint16_t>(selector)))
		return {SegmentOutcome::fault, fault, false};
	cpu.gpr(Register::rsp) = rsp;
	cpu.rip = next_rip(cpu, instruction);
	return finish(cpu, mode, flat);
}

[[nodiscard]] SegmentResult load_far_pointer(CpuState &cpu,
	SegmentMemory &memory, const Instruction &instruction) noexcept
{
	Accesses accesses{cpu, memory, instruction};
	const auto mode = cpu.execution_mode;
	const bool flat = cpu.flat_segments;
	const auto reg = static_cast<SegmentRegister>(instruction.vector_group);
	const auto bytes = operand_bytes(instruction.source.width);
	const auto segment = operand_segment(instruction.source.memory);
	const auto at = accesses.offset(instruction.source);
	std::uint64_t offset{};
	std::uint64_t selector{};
	if (!accesses.read_value(segment, at, bytes, offset))
		return accesses.result();
	const auto selector_at = accesses.compat() ?
		static_cast<std::uint32_t>(at + bytes) : at + bytes;
	if (!accesses.read_value(segment, selector_at, 2, selector))
		return accesses.result();
	if (const auto fault = load_selector(cpu, reg,
		static_cast<std::uint16_t>(selector)))
		return {SegmentOutcome::fault, fault, false};
	write_register(cpu, instruction.destination, offset);
	cpu.rip = next_rip(cpu, instruction);
	return finish(cpu, mode, flat);
}

/* LAR: valid for a code or data segment CPL 3 may use, a conforming code
 * segment whatever its privilege, and the system descriptors 64-bit mode
 * admits -- an LDT, a TSS or a call gate -- of privilege 3; Linux's are all
 * privilege 0. The present bit is not checked. */
[[nodiscard]] bool access_rights_valid(std::uint64_t d,
	std::uint16_t selector) noexcept
{
	if (descriptor_system(d)) {
		const auto type = descriptor_type(d);
		if (type != 2U && type != 9U && type != 11U && type != 12U)
			return false;
	} else if (descriptor_conforming(d)) {
		return true;
	}
	return descriptor_dpl(d) >= 3U && descriptor_dpl(d) >= (selector & 3U);
}

[[nodiscard]] bool limit_valid(std::uint64_t d, std::uint16_t selector) noexcept
{
	if (descriptor_system(d)) {
		const auto type = descriptor_type(d);
		if (type != 2U && type != 9U && type != 11U)
			return false;
	} else if (descriptor_conforming(d)) {
		return true;
	}
	return descriptor_dpl(d) >= 3U && descriptor_dpl(d) >= (selector & 3U);
}

[[nodiscard]] SegmentResult inspect_segment(CpuState &cpu,
	SegmentMemory &memory, const Instruction &instruction) noexcept
{
	Accesses accesses{cpu, memory, instruction};
	const auto mode = cpu.execution_mode;
	const bool flat = cpu.flat_segments;
	std::uint64_t value{};
	if (!accesses.operand_value(instruction.source, 2, value))
		return accesses.result();
	const auto selector = static_cast<std::uint16_t>(value);
	const auto found = descriptor(cpu, selector);
	switch (instruction.operation) {
	case Operation::load_access_rights: {
		const bool valid = found && access_rights_valid(*found, selector);
		set_zero_flag(cpu, valid);
		if (valid)
			write_register(cpu, instruction.destination,
				(*found >> 32U) & (instruction.destination.width ==
					OperandWidth::word ? 0xff00U : 0x00ffff00U));
		break;
	}
	case Operation::load_segment_limit: {
		const auto limit = segment_limit(cpu, selector);
		set_zero_flag(cpu, limit.has_value());
		if (limit)
			write_register(cpu, instruction.destination, *limit);
		break;
	}
	case Operation::verify_segment: {
		bool valid = false;
		if (found && !descriptor_system(*found)) {
			const auto d = *found;
			const bool usable = descriptor_conforming(d) ||
				(descriptor_dpl(d) >= 3U &&
					descriptor_dpl(d) >= (selector & 3U));
			valid = usable && (instruction.vector_group == 4 ?
				descriptor_readable(d) : descriptor_writable(d));
		}
		set_zero_flag(cpu, valid);
		break;
	}
	default:
		return {SegmentOutcome::fault, general_protection(), false};
	}
	cpu.rip = next_rip(cpu, instruction);
	return finish(cpu, mode, flat);
}

/* SLDT and STR, as Linux emulates them under UMIP: SLDT names the GDT's LDT
 * entry while the task has an LDT and is otherwise null; STR names the TSS.
 * A register destination takes the operand size and keeps the bits above
 * it, even a 32-bit one; memory takes two bytes. */
[[nodiscard]] SegmentResult store_segment_table(CpuState &cpu,
	SegmentMemory &memory, const Instruction &instruction) noexcept
{
	Accesses accesses{cpu, memory, instruction};
	const auto mode = cpu.execution_mode;
	const bool flat = cpu.flat_segments;
	const std::uint16_t value = instruction.vector_group == 0 ?
		(cpu.ldt != nullptr ? ldt_selector : 0) : tss_selector;
	const auto &destination = instruction.destination;
	if (destination.kind == OperandKind::memory) {
		std::array<std::byte, 2> data{};
		std::memcpy(data.data(), &value, data.size());
		if (!accesses.write(operand_segment(destination.memory),
			accesses.offset(destination), data))
			return accesses.result();
	} else {
		auto &target = cpu.registers[destination.reg.index & 15U];
		const auto mask = width_mask(operand_bytes(destination.width));
		target = (target & ~mask) | value;
	}
	cpu.rip = next_rip(cpu, instruction);
	return finish(cpu, mode, flat);
}

[[nodiscard]] std::uint64_t register_value(const CpuState &cpu,
	std::uint8_t index, unsigned bytes) noexcept
{
	return cpu.registers[index & 15U] & width_mask(bytes);
}

/* A register written at `bytes`: four zero-extend, one and two merge. */
void set_register(CpuState &cpu, std::uint8_t index, unsigned bytes,
	std::uint64_t value) noexcept
{
	auto &target = cpu.registers[index & 15U];
	if (bytes >= 4) {
		target = bytes == 4 ? static_cast<std::uint32_t>(value) : value;
		return;
	}
	const auto mask = width_mask(bytes);
	target = (target & ~mask) | (value & mask);
}

/* An address register advanced at its address size: a 16-bit one keeps the
 * bits above, a 32-bit one zero-extends. */
void advance_address(CpuState &cpu, std::uint8_t index,
	const MemoryOperand &memory, std::int64_t amount) noexcept
{
	const auto value = cpu.registers[index & 15U] +
		static_cast<std::uint64_t>(amount);
	set_register(cpu, index, memory.address_16 ? 2U :
		memory.address_32 ? 4U : 8U, value);
}

[[nodiscard]] bool string_operation(Operation operation) noexcept
{
	switch (operation) {
	case Operation::string_move:
	case Operation::string_compare:
	case Operation::string_store:
	case Operation::string_load:
	case Operation::string_scan:
		return true;
	default:
		return false;
	}
}

void record_subtract(CpuState &cpu, std::uint64_t left, std::uint64_t right,
	unsigned bytes) noexcept
{
	materialize_flags(cpu);
	auto &flags = cpu.deferred_flags;
	flags = {};
	flags.left = left;
	flags.right = right;
	flags.result = (left - right) & width_mask(bytes);
	flags.operation = DeferredFlagOperation::subtract;
	flags.width = static_cast<std::uint8_t>(bytes);
}

/* MOVS, CMPS, STOS, LODS and SCAS, element by element, each element's
 * registers committed before the next, so the element that faults leaves
 * the count and pointers at itself, as x86 does. */
[[nodiscard]] SegmentResult string_step(CpuState &cpu, SegmentMemory &memory,
	const Instruction &instruction, std::uint64_t elements) noexcept
{
	Accesses accesses{cpu, memory, instruction};
	const auto operation = instruction.operation;
	const auto &destination = instruction.destination;
	const auto &source = instruction.source;
	const auto &addressing = destination.kind == OperandKind::memory ?
		destination.memory : source.memory;
	const unsigned bytes = operand_bytes(destination.kind ==
		OperandKind::memory ? destination.width : source.width);
	const auto count_bytes = addressing.address_16 ? 2U :
		addressing.address_32 ? 4U : 8U;
	const bool repeated = instruction.repeat != 0;
	const bool compares = operation == Operation::string_compare ||
		operation == Operation::string_scan;
	const std::int64_t step = (cpu.rflags & rflags_direction) != 0 ?
		-static_cast<std::int64_t>(bytes) : static_cast<std::int64_t>(bytes);
	const auto element = [&](const Operand &operand, std::uint64_t &value) {
		if (operand.kind == OperandKind::reg) {
			value = register_value(cpu, operand.reg.index, bytes);
			return true;
		}
		return accesses.read_value(operand_segment(operand.memory),
			accesses.offset(operand), bytes, value);
	};
	for (std::uint64_t done = 0;; ++done) {
		if (repeated && register_value(cpu, 1, count_bytes) == 0)
			break;
		if (repeated && done == elements) {
			cpu.rflags |= rflags_resume;
			return {SegmentOutcome::yielded, {}, false};
		}
		std::uint64_t left{};
		std::uint64_t right{};
		switch (operation) {
		case Operation::string_move:
		case Operation::string_store: {
			if (!element(source, right))
				return accesses.result();
			std::array<std::byte, 8> data{};
			std::memcpy(data.data(), &right, bytes);
			if (!accesses.write(operand_segment(destination.memory),
				accesses.offset(destination),
				std::span<const std::byte>{data}.first(bytes)))
				return accesses.result();
			break;
		}
		case Operation::string_load:
			if (!element(source, right))
				return accesses.result();
			set_register(cpu, 0, bytes, right);
			break;
		default:
			if (!element(destination, left) || !element(source, right))
				return accesses.result();
			record_subtract(cpu, left, right, bytes);
			break;
		}
		if (operation != Operation::string_store &&
			operation != Operation::string_scan)
			advance_address(cpu, 6, addressing, step);
		if (operation != Operation::string_load)
			advance_address(cpu, 7, addressing, step);
		if (!repeated)
			break;
		set_register(cpu, 1, count_bytes,
			register_value(cpu, 1, count_bytes) - 1U);
		if (compares) {
			const bool equal = left == right;
			if ((instruction.repeat == 0xf3 && !equal) ||
				(instruction.repeat == 0xf2 && equal))
				break;
		}
	}
	cpu.rip = next_rip(cpu, instruction);
	return {SegmentOutcome::retired, {}, false};
}

[[nodiscard]] bool stack_operation(const Instruction &instruction) noexcept
{
	switch (instruction.operation) {
	case Operation::push:
	case Operation::pop:
	case Operation::push_all:
	case Operation::pop_all:
	case Operation::push_flags:
	case Operation::pop_flags:
	case Operation::call:
	case Operation::call_indirect:
	case Operation::ret:
	case Operation::enter:
	case Operation::leave:
		return true;
	case Operation::segment_stack:
		return instruction.vector_control != 0;
	default:
		return false;
	}
}

/* The stack instructions of a task whose SS has a base or a limit, which
 * the engines' stack gadgets, addressing through ESP alone, cannot honour:
 * every access goes through SS, and nothing commits until all of them
 * have. */
[[nodiscard]] SegmentResult stack_step(CpuState &cpu, SegmentMemory &memory,
	const Instruction &instruction) noexcept
{
	Accesses accesses{cpu, memory, instruction};
	std::int64_t sp{};
	const auto push = [&](std::uint64_t value, unsigned bytes) {
		sp -= static_cast<std::int64_t>(bytes);
		return accesses.stack_write(sp, bytes, value);
	};
	const auto pop = [&](std::uint64_t &value, unsigned bytes) {
		const bool done = accesses.stack_read(sp, bytes, value);
		sp += static_cast<std::int64_t>(bytes);
		return done;
	};
	auto next = next_rip(cpu, instruction);
	const auto rsp = [&] { return accesses.stack_offset(sp); };
	std::uint64_t value{};
	bool trap = false;
	switch (instruction.operation) {
	case Operation::push: {
		const auto bytes = operand_bytes(instruction.source.width);
		if (!accesses.operand_value(instruction.source, bytes, value) ||
			!push(value, bytes))
			return accesses.result();
		cpu.gpr(Register::rsp) = rsp();
		break;
	}
	case Operation::segment_stack: {
		const auto bytes = operand_bytes(instruction.source.width);
		value = cpu.segment_selectors[instruction.vector_group % 6U];
		if (!push(value, bytes))
			return accesses.result();
		cpu.gpr(Register::rsp) = rsp();
		break;
	}
	case Operation::pop: {
		const auto &target = instruction.destination;
		const auto bytes = operand_bytes(target.width);
		if (!pop(value, bytes))
			return accesses.result();
		if (target.kind == OperandKind::memory) {
			/* The address follows the incremented ESP. */
			const auto saved = cpu.gpr(Register::rsp);
			cpu.gpr(Register::rsp) = rsp();
			std::array<std::byte, 8> data{};
			std::memcpy(data.data(), &value, bytes);
			const bool stored = accesses.write(
				operand_segment(target.memory), accesses.offset(target),
				std::span<const std::byte>{data}.first(bytes));
			if (!stored) {
				cpu.gpr(Register::rsp) = saved;
				return accesses.result();
			}
		} else {
			cpu.gpr(Register::rsp) = rsp();
			set_register(cpu, target.reg.index, bytes, value);
		}
		break;
	}
	case Operation::push_all: {
		const auto bytes = operand_bytes(instruction.destination.width);
		const auto original = cpu.gpr(Register::rsp);
		for (std::uint8_t index = 0; index < 8; ++index) {
			const auto pushed = index == 4 ? original :
				cpu.registers[index];
			if (!push(pushed & width_mask(bytes), bytes))
				return accesses.result();
		}
		cpu.gpr(Register::rsp) = rsp();
		break;
	}
	case Operation::pop_all: {
		const auto bytes = operand_bytes(instruction.destination.width);
		std::array<std::uint64_t, 8> popped{};
		for (int index = 7; index >= 0; --index)
			if (!pop(popped[static_cast<std::size_t>(index)], bytes))
				return accesses.result();
		for (std::uint8_t index = 0; index < 8; ++index)
			if (index != 4)
				set_register(cpu, index, bytes, popped[index]);
		cpu.gpr(Register::rsp) = rsp();
		break;
	}
	case Operation::push_flags: {
		const auto bytes = operand_bytes(instruction.source.width);
		materialize_flags(cpu);
		/* The image leaves out RF and VM. */
		if (!push(cpu.rflags & ~0x30000ULL & width_mask(bytes), bytes))
			return accesses.result();
		cpu.gpr(Register::rsp) = rsp();
		break;
	}
	case Operation::pop_flags: {
		const auto bytes = operand_bytes(instruction.destination.width);
		if (!pop(value, bytes))
			return accesses.result();
		materialize_flags(cpu);
		/* At CPL 3 with IOPL 0, as Linux runs user code, POPF changes
		 * the arithmetic flags, TF, DF, NT, AC and ID; RF clears. */
		const std::uint64_t mask = bytes == 2 ? 0x4dd5U : 0x254dd5U;
		const auto before = cpu.rflags;
		cpu.rflags = ((before & ~mask) | (value & mask) | rflags_reserved) &
			~rflags_resume;
		trap = (cpu.rflags & ~before & rflags_trap) != 0;
		cpu.gpr(Register::rsp) = rsp();
		break;
	}
	case Operation::call:
	case Operation::call_indirect: {
		std::uint64_t target{};
		if (instruction.operation == Operation::call) {
			target = static_cast<std::uint32_t>(next +
				instruction.source.immediate);
		} else if (!accesses.operand_value(instruction.source, 4,
			target)) {
			return accesses.result();
		}
		if (!push(next, 4))
			return accesses.result();
		cpu.gpr(Register::rsp) = rsp();
		next = static_cast<std::uint32_t>(target);
		break;
	}
	case Operation::ret: {
		if (!pop(value, 4))
			return accesses.result();
		sp += static_cast<std::int64_t>(instruction.source.immediate &
			0xffffU);
		cpu.gpr(Register::rsp) = rsp();
		next = static_cast<std::uint32_t>(value);
		break;
	}
	case Operation::enter: {
		const auto bytes = operand_bytes(instruction.destination.width);
		const auto allocation = instruction.source.immediate & 0xffffU;
		const auto level = instruction.auxiliary.immediate & 0x1fU;
		if (!push(cpu.gpr(Register::rbp), bytes))
			return accesses.result();
		const auto frame = rsp();
		auto frame_pointer = cpu.gpr(Register::rbp);
		if (level > 0) {
			for (std::uint64_t index = 1; index < level; ++index) {
				frame_pointer = static_cast<std::uint32_t>(
					frame_pointer - bytes);
				std::uint64_t saved{};
				if (!accesses.read_value(SegmentRegister::ss,
					frame_pointer, bytes, saved) || !push(saved, bytes))
					return accesses.result();
			}
			if (!push(frame, bytes))
				return accesses.result();
		}
		sp -= static_cast<std::int64_t>(allocation);
		set_register(cpu, 5, bytes, frame);
		cpu.gpr(Register::rsp) = rsp();
		break;
	}
	case Operation::leave: {
		const auto bytes = operand_bytes(instruction.destination.width);
		const auto frame = static_cast<std::uint32_t>(cpu.gpr(Register::rbp));
		if (!accesses.read_value(SegmentRegister::ss, frame, bytes, value))
			return accesses.result();
		cpu.gpr(Register::rsp) = static_cast<std::uint32_t>(frame + bytes);
		set_register(cpu, 5, bytes, value);
		break;
	}
	default:
		return {SegmentOutcome::run, {}, false};
	}
	cpu.rip = next;
	return {SegmentOutcome::retired, {}, trap};
}

/* The bytes an explicit memory operand of `instruction` covers, and
 * whether the instruction writes them; zero bytes for one it never
 * accesses. XSAVE and XRSTOR check the legacy area and the header, and the
 * AVX component only when RFBM (EDX:EAX, in XCR0) selects it. */
[[nodiscard]] std::size_t operand_access(const CpuState &cpu,
	const Instruction &instruction, const Operand &operand,
	bool &write) noexcept
{
	const bool destination = &operand == &instruction.destination;
	write = destination;
	const auto reg = (instruction.vector_control >> 3U) & 7U;
	const std::size_t image = instruction.vector_prefix != 0 ? 1U : 2U;
	switch (instruction.operation) {
	case Operation::lea:
	case Operation::nop:
		return 0;
	case Operation::compare:
	case Operation::test:
	case Operation::bit_test:
	case Operation::vector_test:
	case Operation::vector_compare_flags:
		write = false;
		break;
	case Operation::exchange:
	case Operation::exchange_add:
	case Operation::compare_exchange:
		write = true;
		break;
	case Operation::compare_exchange_pair:
		write = true;
		return static_cast<std::size_t>(instruction.auxiliary.immediate);
	case Operation::cache_flush:
		write = false;
		return 1;
	case Operation::mxcsr:
		write = instruction.vector_group == 3;
		return 4;
	case Operation::extended_state:
		write = instruction.vector_group == 0 || instruction.vector_group == 4;
		if (instruction.vector_group <= 1)
			return 512U;
		return (cpu.registers[0] & 4U) != 0 ? 832U : 576U;
	case Operation::bound:
		write = false;
		return 2U * operand_bytes(operand.width);
	case Operation::x87:
		switch (instruction.vector_opcode) {
		case 0xd8:
		case 0xda:
			write = false;
			return 4;
		case 0xdc:
			write = false;
			return 8;
		case 0xde:
			write = false;
			return 2;
		case 0xd9:
			write = reg == 2 || reg == 3 || reg == 6 || reg == 7;
			return reg == 4 || reg == 6 ? 14U * image :
				reg == 5 || reg == 7 ? 2U : 4U;
		case 0xdb:
			write = reg == 1 || reg == 2 || reg == 3 || reg == 7;
			return reg == 5 || reg == 7 ? 10U : 4U;
		case 0xdd:
			write = reg == 1 || reg == 2 || reg == 3 || reg == 6 ||
				reg == 7;
			return reg == 4 || reg == 6 ? (image == 2 ? 108U : 94U) :
				reg == 7 ? 2U : 8U;
		case 0xdf:
			write = reg == 1 || reg == 2 || reg == 3 || reg == 6 ||
				reg == 7;
			return reg == 4 || reg == 6 ? 10U :
				reg == 5 || reg == 7 ? 8U : 2U;
		default:
			return 0;
		}
	default:
		break;
	}
	return operand_bytes(operand.width);
}

} // namespace

std::uint64_t tls_descriptor(std::uint64_t head, std::uint64_t tail) noexcept
{
	if ((head | tail) == 0)
		return 0;
	/* struct user_desc's flags: seg_32bit, contents (2 bits),
	 * read_exec_only, limit_in_pages, seg_not_present, useable. */
	const auto base = static_cast<std::uint32_t>(head >> 32U);
	const auto limit = static_cast<std::uint32_t>(tail) & 0xfffffU;
	const auto flags = static_cast<std::uint32_t>(tail >> 32U);
	const std::uint64_t seg_32bit = flags & 1U;
	const std::uint64_t contents = (flags >> 1U) & 3U;
	const std::uint64_t read_exec_only = (flags >> 3U) & 1U;
	const std::uint64_t limit_in_pages = (flags >> 4U) & 1U;
	const std::uint64_t not_present = (flags >> 5U) & 1U;
	const std::uint64_t useable = (flags >> 6U) & 1U;
	const std::uint64_t type = ((read_exec_only ^ 1U) << 1U) |
		(contents << 2U) | 1U;
	return (limit & 0xffffU) |
		(static_cast<std::uint64_t>(base & 0xffffffU) << 16U) |
		(type << 40U) | (1ULL << 44U) | (3ULL << 45U) |
		((not_present ^ 1U) << 47U) |
		(static_cast<std::uint64_t>(limit >> 16U) << 48U) |
		(useable << 52U) | (seg_32bit << 54U) | (limit_in_pages << 55U) |
		(static_cast<std::uint64_t>(base >> 24U) << 56U);
}

SegmentRegister operand_segment(const MemoryOperand &memory) noexcept
{
	if (memory.segment == Segment::fs)
		return SegmentRegister::fs;
	if (memory.segment == Segment::gs)
		return SegmentRegister::gs;
	switch (memory.legacy_segment) {
	case LegacySegment::es: return SegmentRegister::es;
	case LegacySegment::cs: return SegmentRegister::cs;
	case LegacySegment::ss: return SegmentRegister::ss;
	case LegacySegment::ds: return SegmentRegister::ds;
	case LegacySegment::none: break;
	}
	if (!memory.rip_relative && !memory.absolute_address &&
		(memory.base == 4 || memory.base == 5))
		return SegmentRegister::ss;
	return SegmentRegister::ds;
}

void refresh_segment_checks(CpuState &cpu) noexcept
{
	for (auto &cache : cpu.segments)
		compute_check(cache);
	cpu.flat_segments = cpu.execution_mode == ExecutionMode::long64 ||
		(flat_data(cpu.segments[index_of(SegmentRegister::es)]) &&
			flat_data(cpu.segments[index_of(SegmentRegister::ss)]) &&
			flat_data(cpu.segments[index_of(SegmentRegister::ds)]));
}

SegmentFault enter_segments(CpuState &cpu) noexcept
{
	auto &selectors = cpu.segment_selectors;
	const auto code = selectors[index_of(SegmentRegister::cs)];
	std::uint64_t code_descriptor{};
	/* What IRET to CPL 3 checks, CS before SS. */
	SegmentFault fault = check_code_load(cpu, code, CodeTransfer::ret,
		code_descriptor);
	if (fault) {
		cpu.execution_mode = ExecutionMode::long64;
		return fault;
	}
	cpu.segments[index_of(SegmentRegister::cs)] = cache_of(code_descriptor);
	cpu.execution_mode = mode_of(cpu.segments[index_of(SegmentRegister::cs)]);
	const auto stack = selectors[index_of(SegmentRegister::ss)];
	std::uint64_t stack_descriptor{};
	fault = check_stack_load(cpu, stack, stack_descriptor);
	if (fault)
		return fault;
	cpu.segments[index_of(SegmentRegister::ss)] = cache_of(stack_descriptor);
	for (const auto reg : {SegmentRegister::es, SegmentRegister::ds,
		SegmentRegister::fs, SegmentRegister::gs}) {
		auto &selector = selectors[index_of(reg)];
		std::optional<std::uint64_t> loaded;
		if (check_data_load(cpu, selector, loaded)) {
			selector = 0;
			loaded.reset();
		}
		cpu.segments[index_of(reg)] = loaded ? cache_of(*loaded) :
			SegmentCache{};
	}
	refresh_segment_checks(cpu);
	return {};
}

bool segment_instruction(const Instruction &instruction) noexcept
{
	switch (instruction.operation) {
	case Operation::segment_move:
		return instruction.vector_opcode == 0x8e;
	case Operation::segment_stack:
		return instruction.vector_control == 0;
	case Operation::load_far_pointer:
	case Operation::far_call:
	case Operation::far_jump:
	case Operation::far_ret:
	case Operation::interrupt_ret:
	case Operation::load_access_rights:
	case Operation::load_segment_limit:
	case Operation::verify_segment:
	case Operation::store_segment_table:
		return true;
	default:
		return false;
	}
}

SegmentResult execute_segment_instruction(CpuState &cpu,
	SegmentMemory &memory, const Instruction &instruction) noexcept
{
	switch (instruction.operation) {
	case Operation::segment_move:
		return load_segment_register(cpu, memory, instruction);
	case Operation::segment_stack:
		return pop_segment_register(cpu, memory, instruction);
	case Operation::load_far_pointer:
		return load_far_pointer(cpu, memory, instruction);
	case Operation::far_call:
	case Operation::far_jump:
		return far_jump_or_call(cpu, memory, instruction);
	case Operation::far_ret:
		return far_return(cpu, memory, instruction);
	case Operation::interrupt_ret:
		return interrupt_return(cpu, memory, instruction);
	case Operation::load_access_rights:
	case Operation::load_segment_limit:
	case Operation::verify_segment:
		return inspect_segment(cpu, memory, instruction);
	case Operation::store_segment_table:
		return store_segment_table(cpu, memory, instruction);
	default:
		return {SegmentOutcome::fault, general_protection(), false};
	}
}

bool segment_guarded(const Instruction &instruction) noexcept
{
	bool through_fs_gs = false;
	for (const auto *operand : {&instruction.destination,
		&instruction.source, &instruction.auxiliary}) {
		if (operand->kind != OperandKind::memory)
			continue;
		if (operand->memory.legacy_segment == LegacySegment::cs)
			return instruction.operation != Operation::lea &&
				instruction.operation != Operation::nop;
		if (operand->memory.segment != Segment::none)
			through_fs_gs = true;
	}
	if (!through_fs_gs)
		return false;
	switch (instruction.operation) {
	case Operation::string_move:
	case Operation::string_compare:
	case Operation::string_store:
	case Operation::string_load:
	case Operation::string_scan:
	case Operation::xlat:
	case Operation::vector_gather:
	case Operation::vector_mask_store:
	case Operation::extended_state:
		return true;
	case Operation::x87:
		/* FRSTOR and FNSAVE: 94 or 108 bytes. */
		return instruction.vector_opcode == 0xdd &&
			(((instruction.vector_control >> 3U) & 7U) == 4U ||
				((instruction.vector_control >> 3U) & 7U) == 6U);
	default:
		return false;
	}
}

std::optional<std::uint32_t> segment_limit(const CpuState &cpu,
	std::uint16_t selector) noexcept
{
	const auto found = descriptor(cpu, selector);
	if (!found || !limit_valid(*found, selector))
		return std::nullopt;
	return descriptor_limit(*found);
}

void AddressSpaceSegmentMemory::record(std::uint64_t address,
	::uml::memory::MemoryAccess access) noexcept
{
	if (memory_.has_fault()) {
		const auto fault = memory_.last_fault();
		fault_ = {fault.address, fault.access, false};
	} else {
		fault_ = {address, access, false};
	}
}

bool AddressSpaceSegmentMemory::read(std::uint64_t address,
	std::span<std::byte> destination) noexcept
{
	if (memory_.read(address, destination))
		return true;
	record(address, ::uml::memory::MemoryAccess::read);
	return false;
}

bool AddressSpaceSegmentMemory::write(std::uint64_t address,
	std::span<const std::byte> source) noexcept
{
	if (!memory_.write(address, source)) {
		record(address, ::uml::memory::MemoryAccess::write);
		return false;
	}
	/* x86 guests use 4 KiB pages. */
	constexpr std::uint64_t page = 4096;
	for (auto at = address & ~(page - 1U); at < address + source.size();
		at += page)
		if (memory_.is_executable(at))
			memory_.invalidate_code(at, execution_context_);
	return true;
}

namespace {

/* The instruction RIP names, or why there is none. */
[[nodiscard]] bool decode_step(CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, SegmentStep &step) noexcept
{
	const auto decoded = decode(memory, cpu.rip, cpu.execution_mode);
	if (decoded.status == DecodeStatus::fetch_fault) {
		step.fetch_fault = true;
		step.fault = {decoded.fault_address,
			::uml::memory::MemoryAccess::execute, false};
		return false;
	}
	if (decoded.status != DecodeStatus::decoded) {
		step.illegal = true;
		return false;
	}
	step.instruction = decoded.instruction;
	return true;
}

} // namespace

SegmentStep check_segment_step(CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, void *execution_context,
	std::uint64_t string_elements) noexcept
{
	SegmentStep step{};
	if (!decode_step(cpu, memory, step))
		return step;
	AddressSpaceSegmentMemory segment_memory{memory, execution_context};
	step.result = prepare_segment_step(cpu, segment_memory,
		step.instruction, string_elements);
	if (step.result.outcome == SegmentOutcome::memory_fault)
		step.fault = segment_memory.fault();
	if (step.result.outcome == SegmentOutcome::run && !cpu.flat_segments)
		for (auto *operand : {&step.instruction.destination,
			&step.instruction.source, &step.instruction.auxiliary})
			if (operand->kind == OperandKind::memory)
				operand->memory.segment_base = true;
	return step;
}

SegmentStep run_segment_step(CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory,
	void *execution_context) noexcept
{
	SegmentStep step{};
	if (!decode_step(cpu, memory, step))
		return step;
	if (!segment_instruction(step.instruction)) {
		step.illegal = true;
		return step;
	}
	AddressSpaceSegmentMemory segment_memory{memory, execution_context};
	step.result = execute_segment_instruction(cpu, segment_memory,
		step.instruction);
	if (step.result.outcome == SegmentOutcome::memory_fault)
		step.fault = segment_memory.fault();
	return step;
}

SegmentBypass::SegmentBypass(CpuState &cpu) noexcept : cpu_(cpu)
{
	for (auto &cache : cpu_.segments) {
		cache.check_low = 0;
		cache.check_span = ~std::uint64_t{};
	}
	cpu_.segment_bypass = 1;
}

SegmentBypass::~SegmentBypass()
{
	cpu_.segment_bypass = 0;
	refresh_segment_checks(cpu_);
}

SegmentResult prepare_segment_step(CpuState &cpu, SegmentMemory &memory,
	const Instruction &instruction, std::uint64_t string_elements) noexcept
{
	/* The segment model checks its own instructions as it runs them, and
	 * long mode checks no limit. */
	if (segment_instruction(instruction) ||
		cpu.execution_mode != ExecutionMode::compat32)
		return {SegmentOutcome::run, {}, false};
	if (string_operation(instruction.operation))
		return string_step(cpu, memory, instruction,
			std::max<std::uint64_t>(string_elements, 1));
	if (instruction.operation == Operation::xlat) {
		/* AL indexes the table at (E)BX, which no operand names. */
		Accesses accesses{cpu, memory, instruction};
		const auto table = accesses.offset(instruction.source);
		const auto &table_memory = instruction.source.memory;
		const auto offset = (table + (cpu.registers[0] & 0xffU)) &
			(table_memory.address_16 ? 0xffffU : 0xffffffffU);
		std::uint64_t value{};
		if (!accesses.read_value(operand_segment(table_memory), offset, 1,
			value))
			return accesses.result();
		set_register(cpu, 0, 1, value);
		cpu.rip = next_rip(cpu, instruction);
		return {SegmentOutcome::retired, {}, false};
	}
	if (stack_operation(instruction) &&
		!flat_data(cpu.segments[index_of(SegmentRegister::ss)]))
		return stack_step(cpu, memory, instruction);
	Accesses accesses{cpu, memory, instruction};
	for (const auto *operand : {&instruction.destination,
		&instruction.source, &instruction.auxiliary}) {
		if (operand->kind != OperandKind::memory ||
			operand->memory.vector_index)
			continue;
		bool write{};
		const auto size = operand_access(cpu, instruction, *operand, write);
		if (size == 0)
			continue;
		auto offset = accesses.offset(*operand);
		/* POP to memory addresses through the incremented ESP. */
		if (instruction.operation == Operation::pop &&
			operand->memory.base == 4)
			offset = static_cast<std::uint32_t>(offset +
				operand_bytes(operand->width));
		std::uint64_t address{};
		if (!accesses.linear(operand_segment(operand->memory), offset,
			size, write, address))
			return accesses.result();
	}
	return {SegmentOutcome::run, {}, false};
}

} // namespace uml::cpu::x86_64
