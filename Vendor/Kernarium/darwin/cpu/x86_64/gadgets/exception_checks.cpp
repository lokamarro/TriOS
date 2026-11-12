// SPDX-License-Identifier: GPL-2.0-or-later
#include "exception_checks.hpp"

#include "cpu/x86_64/fp_model.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>

namespace uml::cpu::x86_64::gadgets {
namespace {

[[nodiscard]] constexpr unsigned modrm_reg(std::uint8_t modrm) noexcept
{
	return (modrm >> 3U) & 7U;
}

[[nodiscard]] constexpr bool modrm_register(std::uint8_t modrm) noexcept
{
	return (modrm >> 6U) == 3U;
}

[[nodiscard]] bool fwait(const Instruction &instruction) noexcept
{
	return instruction.operation == Operation::nop &&
		instruction.vector_opcode == 0x9b;
}

[[nodiscard]] bool uses_mmx(const Instruction &instruction) noexcept
{
	return instruction.destination.kind == OperandKind::mmx_reg ||
		instruction.source.kind == OperandKind::mmx_reg ||
		instruction.auxiliary.kind == OperandKind::mmx_reg;
}

/* An x87 escape, D8 to DF, rather than EMMS. */
[[nodiscard]] bool x87_escape(const Instruction &instruction) noexcept
{
	return instruction.operation == Operation::x87 &&
		instruction.vector_opcode >= 0xd8;
}

/* The bytes an x87 memory form reads or writes, which is also the
 * alignment #AC asks of it: the environment and state images want four,
 * or two in their 16-bit form, and an 80-bit operand wants eight. */
[[nodiscard]] std::size_t x87_memory_alignment(
	const Instruction &instruction) noexcept
{
	const auto reg = modrm_reg(instruction.vector_control);
	const std::size_t image = instruction.vector_prefix != 0 ? 2U : 4U;
	switch (instruction.vector_opcode) {
	case 0xd8:
	case 0xda:
		return 4;
	case 0xdc:
		return 8;
	case 0xde:
		return 2;
	case 0xd9:
		switch (reg) {
		case 4:
		case 6: return image;
		case 5:
		case 7: return 2;
		default: return 4;
		}
	case 0xdb:
		return reg == 5 || reg == 7 ? 8U : 4U;
	case 0xdd:
		switch (reg) {
		case 4:
		case 6: return image;
		case 7: return 2;
		default: return 8;
		}
	case 0xdf:
		return reg >= 4 ? 8U : 2U;
	default:
		return 0;
	}
}

[[nodiscard]] std::size_t stack_bytes(const CpuState &cpu) noexcept
{
	return cpu.execution_mode == ExecutionMode::compat32 ? 4U : 8U;
}

[[nodiscard]] std::uint64_t stack_address(const CpuState &cpu,
	std::uint64_t address) noexcept
{
	return cpu.execution_mode == ExecutionMode::compat32 ?
		static_cast<std::uint32_t>(address) : address;
}

[[nodiscard]] constexpr bool misaligned(std::uint64_t address,
	std::size_t bytes) noexcept
{
	return bytes > 1 && bytes <= 8 && (address & (bytes - 1U)) != 0;
}

/* The count register a repeated string instruction reads, at its address
 * size. */
[[nodiscard]] std::uint64_t repeat_count(const CpuState &cpu,
	const Operand &operand) noexcept
{
	const auto count = cpu.gpr(Register::rcx);
	if (operand.memory.address_16)
		return count & 0xffffU;
	if (operand.memory.address_32)
		return count & 0xffffffffU;
	return count;
}

} // namespace

bool waits_for_x87(const Instruction &instruction) noexcept
{
	if (fwait(instruction) || uses_mmx(instruction))
		return true;
	if (instruction.operation != Operation::x87)
		return false;
	if (!x87_escape(instruction))
		return true;
	const auto modrm = instruction.vector_control;
	const auto reg = modrm_reg(modrm);
	switch (instruction.vector_opcode) {
	case 0xd9:
		/* FNSTENV and FNSTCW. */
		return modrm_register(modrm) || (reg != 6 && reg != 7);
	case 0xdb:
		/* FNENI, FNDISI, FNCLEX, FNINIT and FNSETPM. */
		return !(modrm == 0xe0 || modrm == 0xe1 || modrm == 0xe2 ||
			modrm == 0xe3 || modrm == 0xe4);
	case 0xdd:
		/* FNSAVE and FNSTSW. */
		return modrm_register(modrm) || (reg != 6 && reg != 7);
	case 0xdf:
		/* FNSTSW AX. */
		return modrm != 0xe0;
	default:
		return true;
	}
}

bool x87_loads_state(const Instruction &instruction) noexcept
{
	if (!x87_escape(instruction) ||
		modrm_register(instruction.vector_control))
		return false;
	const auto reg = modrm_reg(instruction.vector_control);
	return (instruction.vector_opcode == 0xd9 && (reg == 4 || reg == 5)) ||
		(instruction.vector_opcode == 0xdd && reg == 4);
}

bool x87_computes(const Instruction &instruction) noexcept
{
	return x87_escape(instruction) && waits_for_x87(instruction) &&
		!x87_loads_state(instruction);
}

X87Compare x87_compare(const Instruction &instruction) noexcept
{
	if (!x87_escape(instruction))
		return X87Compare::none;
	const auto modrm = instruction.vector_control;
	const auto reg = modrm_reg(modrm);
	const auto opcode = instruction.vector_opcode;
	if (!modrm_register(modrm))
		return (opcode == 0xd8 || opcode == 0xda || opcode == 0xdc ||
				opcode == 0xde) && (reg == 2 || reg == 3) ?
			X87Compare::status : X87Compare::none;
	switch (opcode) {
	case 0xd8:
	case 0xdc:
		return reg == 2 || reg == 3 ? X87Compare::status : X87Compare::none;
	case 0xd9:
		return modrm == 0xe4 ? X87Compare::status : X87Compare::none;
	case 0xda:
		return modrm == 0xe9 ? X87Compare::status : X87Compare::none;
	case 0xdb:
	case 0xdf:
		return reg == 5 || reg == 6 ? X87Compare::flags : X87Compare::none;
	case 0xdd:
		return reg == 4 || reg == 5 ? X87Compare::status : X87Compare::none;
	case 0xde:
		return reg == 2 || modrm == 0xd9 ?
			X87Compare::status : X87Compare::none;
	default:
		return X87Compare::none;
	}
}

bool x87_converts_load(const Instruction &instruction) noexcept
{
	return x87_escape(instruction) &&
		!modrm_register(instruction.vector_control) &&
		modrm_reg(instruction.vector_control) == 0 &&
		(instruction.vector_opcode == 0xd9 ||
			instruction.vector_opcode == 0xdd);
}

std::size_t x87_store_bytes(const Instruction &instruction) noexcept
{
	if (!x87_escape(instruction) ||
		modrm_register(instruction.vector_control))
		return 0;
	const auto reg = modrm_reg(instruction.vector_control);
	switch (instruction.vector_opcode) {
	case 0xd9:
		return reg == 2 || reg == 3 ? 4U : 0U;
	case 0xdb:
		return reg == 7 ? 10U : reg >= 1 && reg <= 3 ? 4U : 0U;
	case 0xdd:
		return reg >= 1 && reg <= 3 ? 8U : 0U;
	case 0xdf:
		return reg == 6 ? 10U : reg == 7 ? 8U :
			reg >= 1 && reg <= 3 ? 2U : 0U;
	default:
		return 0;
	}
}

bool simd_floating(const Instruction &instruction) noexcept
{
	const auto opcode = instruction.vector_opcode;
	switch (instruction.operation) {
	case Operation::vector_convert:
	case Operation::vector_scalar_convert:
	case Operation::vector_compare_flags:
	case Operation::vector_fma:
		return true;
	case Operation::vector_alu:
		return opcode == 0x51 || opcode == 0x58 || opcode == 0x59 ||
			(opcode >= 0x5c && opcode <= 0x5f) || opcode == 0x7c ||
			opcode == 0x7d || opcode == 0xc2 || opcode == 0xd0;
	case Operation::vector_map2_misc:
		/* VCVTPH2PS. */
		return opcode == 0x13;
	case Operation::vector_integer_map3:
		/* ROUNDPS to ROUNDSD, DPPS, DPPD and VCVTPS2PH. */
		return (opcode >= 0x08 && opcode <= 0x0b) || opcode == 0x40 ||
			opcode == 0x41 || opcode == 0x1d;
	default:
		return false;
	}
}

bool needs_check(const Instruction &instruction, unsigned checked) noexcept
{
	if ((checked & check_x87) != 0 &&
		(instruction.operation == Operation::x87 ||
			fwait(instruction) || uses_mmx(instruction)))
		return true;
	if ((checked & check_simd) != 0 && simd_floating(instruction))
		return true;
	if ((checked & check_alignment) == 0)
		return false;
	switch (instruction.operation) {
	case Operation::nop:
	case Operation::lea:
		return false;
	case Operation::push:
	case Operation::pop:
	case Operation::push_all:
	case Operation::pop_all:
	case Operation::push_flags:
	case Operation::pop_flags:
	case Operation::leave:
	case Operation::enter:
	case Operation::call:
	case Operation::call_indirect:
	case Operation::ret:
	case Operation::interrupt_ret:
	case Operation::segment_stack:
		return true;
	default:
		return instruction.destination.kind == OperandKind::memory ||
			instruction.source.kind == OperandKind::memory ||
			instruction.auxiliary.kind == OperandKind::memory;
	}
}

std::uint64_t effective_address(const CpuState &cpu, const Gadget &gadget,
	const Operand &operand) noexcept
{
	const auto &memory = operand.memory;
	std::uint64_t address{};
	if (memory.absolute_address) {
		address = memory.absolute;
	} else if (memory.rip_relative) {
		address = gadget.address + gadget.instruction.length +
			static_cast<std::uint64_t>(
				static_cast<std::int64_t>(memory.displacement));
	} else {
		address = static_cast<std::uint64_t>(
			static_cast<std::int64_t>(memory.displacement));
		if (memory.base != MemoryOperand::no_register)
			address += cpu.registers[memory.base];
		if (memory.index != MemoryOperand::no_register &&
			!memory.vector_index)
			address += cpu.registers[memory.index] * memory.scale;
	}
	if (memory.address_16)
		address &= 0xffffU;
	else if (memory.address_32)
		address &= 0xffffffffU;
	switch (memory.segment) {
	case Segment::none:
		return address;
	case Segment::fs:
		address += cpu.fs_base;
		break;
	case Segment::gs:
		address += cpu.gs_base;
		break;
	}
	return cpu.execution_mode == ExecutionMode::compat32 ?
		static_cast<std::uint32_t>(address) : address;
}

bool misaligned_access(const CpuState &cpu, const Gadget &gadget) noexcept
{
	const auto &instruction = gadget.instruction;
	const auto rsp = cpu.gpr(Register::rsp);
	const auto operand_bytes = [](const Operand &operand) noexcept {
		return static_cast<std::size_t>(operand.width);
	};
	switch (instruction.operation) {
	case Operation::nop:
	case Operation::lea:
	case Operation::extended_state:
	case Operation::vector_gather:
	case Operation::cache_flush:
	case Operation::far_call:
	case Operation::far_jump:
		return false;
	case Operation::push:
	case Operation::push_flags:
	case Operation::segment_stack: {
		const bool pushing = instruction.operation != Operation::segment_stack ||
			instruction.vector_control != 0;
		const auto &stack = pushing ? instruction.source :
			instruction.destination;
		const auto bytes = operand_bytes(stack);
		if (instruction.source.kind == OperandKind::memory &&
			misaligned(effective_address(cpu, gadget, instruction.source),
				bytes))
			return true;
		return misaligned(stack_address(cpu, pushing ? rsp - bytes : rsp),
			bytes);
	}
	case Operation::pop:
	case Operation::pop_flags:
	case Operation::pop_all: {
		const auto bytes = operand_bytes(instruction.destination);
		if (misaligned(stack_address(cpu, rsp), bytes))
			return true;
		return instruction.destination.kind == OperandKind::memory &&
			misaligned(effective_address(cpu, gadget,
				instruction.destination), bytes);
	}
	case Operation::push_all: {
		const auto bytes = operand_bytes(instruction.destination);
		return misaligned(stack_address(cpu, rsp - bytes), bytes);
	}
	case Operation::call:
	case Operation::call_indirect:
	case Operation::enter: {
		if (instruction.operation == Operation::call_indirect &&
			instruction.source.kind == OperandKind::memory &&
			misaligned(effective_address(cpu, gadget, instruction.source),
				operand_bytes(instruction.source)))
			return true;
		const auto bytes = stack_bytes(cpu);
		return misaligned(stack_address(cpu, rsp - bytes), bytes);
	}
	case Operation::ret:
	case Operation::interrupt_ret:
		return misaligned(stack_address(cpu, rsp), stack_bytes(cpu));
	case Operation::leave:
		return misaligned(stack_address(cpu, cpu.gpr(Register::rbp)),
			stack_bytes(cpu));
	case Operation::string_move:
	case Operation::string_compare:
	case Operation::string_store:
	case Operation::string_load:
	case Operation::string_scan: {
		const Operand *first = instruction.source.kind ==
			OperandKind::memory ? &instruction.source :
			&instruction.destination;
		if (instruction.repeat != 0 &&
			repeat_count(cpu, *first) == 0)
			return false;
		for (const auto *operand :
			{&instruction.source, &instruction.destination}) {
			if (operand->kind == OperandKind::memory &&
				misaligned(effective_address(cpu, gadget, *operand),
					operand_bytes(*operand)))
				return true;
		}
		return false;
	}
	case Operation::x87:
		return instruction.source.kind == OperandKind::memory &&
			misaligned(effective_address(cpu, gadget, instruction.source),
				x87_memory_alignment(instruction));
	default:
		break;
	}
	for (const auto *operand : {&instruction.destination,
		&instruction.source, &instruction.auxiliary}) {
		if (operand->kind == OperandKind::memory &&
			!operand->memory.vector_index &&
			misaligned(effective_address(cpu, gadget, *operand),
				operand_bytes(*operand)))
			return true;
	}
	return false;
}

namespace {

using VectorValue = std::array<std::byte, 32>;

/* An operand's register, or the bytes its memory operand reads with zeros
 * after them; false when that memory is unreadable. */
[[nodiscard]] bool operand_value(const CpuState &cpu, const Gadget &gadget,
	const Operand &operand, ::uml::memory::GuestAddressSpace &memory,
	VectorValue &value)
{
	value = {};
	if (operand.kind == OperandKind::vector_reg) {
		value = cpu.vectors[operand.reg.index & 15U];
		return true;
	}
	if (operand.kind != OperandKind::memory)
		return false;
	const auto bytes = std::min(static_cast<std::size_t>(operand.width),
		value.size());
	return memory.read(effective_address(cpu, gadget, operand),
		std::span<std::byte>{value}.first(bytes));
}

/* The 128-bit part `index` of an operand's bytes. */
[[nodiscard]] FpVector part(const VectorValue &value,
	std::size_t index) noexcept
{
	FpVector bytes;
	std::memcpy(bytes.data(), value.data() + index * bytes.size(),
		bytes.size());
	return bytes;
}

[[nodiscard]] std::uint32_t operand_bytes(const Operand &operand) noexcept
{
	return static_cast<std::uint32_t>(operand.width);
}

} // namespace

std::optional<FpSteps> simd_fp_steps(const CpuState &cpu,
	const Gadget &gadget, ::uml::memory::GuestAddressSpace &memory)
{
	const auto &instruction = gadget.instruction;
	const auto opcode = instruction.vector_opcode;
	const auto prefix = instruction.vector_prefix;
	const auto &destination = instruction.destination;
	const auto &rm = instruction.auxiliary;
	const bool ymm = destination.width == OperandWidth::ymm;
	/* x86's first, second and third sources of the operation. */
	VectorValue a{};
	VectorValue b{};
	VectorValue c{};
	const auto read = [&](const Operand &operand, VectorValue &value) {
		return operand_value(cpu, gadget, operand, memory, value);
	};
	FpOperation operation{};
	/* Whether the operation reads one operand across both of its 128-bit
	 * halves, and else computes each half of the operands alone. */
	bool halves = false;
	std::size_t parts = 1;
	/* A packed operation on a single or a double of `prefix`. */
	const auto packed = [&](FpKind kind, bool dbl) {
		operation = {kind, dbl ? 8U : 4U, dbl ? 2U : 4U, 0, 0};
		parts = ymm ? 2U : 1U;
	};
	const auto scalar = [&](FpKind kind, bool dbl) {
		operation = {kind, dbl ? 8U : 4U, 1U, 0, 0};
	};
	switch (instruction.operation) {
	case Operation::vector_alu: {
		FpKind kind{};
		switch (opcode) {
		case 0x51: kind = FpKind::sqrt; break;
		case 0x58: kind = FpKind::add; break;
		case 0x59: kind = FpKind::mul; break;
		case 0x5c: kind = FpKind::sub; break;
		case 0x5d: kind = FpKind::min; break;
		case 0x5e: kind = FpKind::div; break;
		case 0x5f: kind = FpKind::max; break;
		case 0x7c: kind = FpKind::hadd; break;
		case 0x7d: kind = FpKind::hsub; break;
		case 0xc2: kind = FpKind::cmp; break;
		case 0xd0: kind = FpKind::addsub; break;
		default: return std::nullopt;
		}
		/* HADD, HSUB and ADDSUB are PD with 66 and PS with F2; the rest
		 * PS, PD, SS and SD with none, 66, F3 and F2. */
		const bool horizontal = opcode == 0x7c || opcode == 0x7d ||
			opcode == 0xd0;
		const bool dbl = horizontal ? prefix == 1 :
			prefix == 1 || prefix == 3;
		if (!horizontal && prefix >= 2)
			scalar(kind, dbl);
		else
			packed(kind, dbl);
		if (opcode == 0x51) {
			if (!read(rm, a))
				return std::nullopt;
		} else if (!read(instruction.source, a) || !read(rm, b)) {
			return std::nullopt;
		}
		if (opcode == 0xc2) {
			operation.immediate = instruction.vector_control;
			operation.flags = instruction.vex ? fp_cmp_vex : 0U;
		}
		break;
	}
	case Operation::vector_fma: {
		/* 132 is destination * r/m + vvvv, 213 vvvv * destination + r/m,
		 * and 231 vvvv * r/m + destination. */
		VectorValue accumulator{};
		VectorValue vvvv{};
		VectorValue source{};
		if (!read(destination, accumulator) ||
			!read(instruction.source, vvvv) || !read(rm, source))
			return std::nullopt;
		const unsigned form = opcode >> 4U;
		const unsigned type = opcode & 0x0fU;
		a = form == 0x9U ? accumulator : vvvv;
		b = form == 0xaU ? accumulator : source;
		c = form == 0x9U ? vvvv : form == 0xaU ? source : accumulator;
		if (type >= 9U && (type & 1U) != 0)
			scalar(FpKind::fma, instruction.vector_group != 0);
		else
			packed(FpKind::fma, instruction.vector_group != 0);
		/* FNMADD and FNMSUB negate the product, FMSUB and FNMSUB the
		 * addend, FMADDSUB in its even lanes and FMSUBADD in its odd
		 * ones. */
		if (type >= 0xcU)
			operation.flags |= fp_fma_negate_product;
		if (type == 0xaU || type == 0xbU || type == 0xeU || type == 0xfU)
			operation.flags |= fp_fma_negate_addend;
		else if (type == 6U)
			operation.flags |= fp_fma_negate_even_addends;
		else if (type == 7U)
			operation.flags |= fp_fma_negate_odd_addends;
		break;
	}
	case Operation::vector_compare_flags:
		/* COMISS and COMISD with none and 66, and UCOMISS and UCOMISD,
		 * which signal only for a signalling NaN. */
		scalar(FpKind::comis, prefix == 1);
		operation.flags = opcode == 0x2eU ? fp_comis_quiet : 0U;
		if (!read(instruction.source, a) || !read(rm, b))
			return std::nullopt;
		break;
	case Operation::vector_convert: {
		if (!read(rm, a))
			return std::nullopt;
		const auto bytes = operand_bytes(rm);
		if (opcode == 0x5a) {
			/* CVTPS2PD and CVTSS2SD widen singles, with none and F3;
			 * CVTPD2PS and CVTSD2SS narrow doubles, with 66 and F2. */
			if (prefix == 0 || prefix == 2) {
				operation = {FpKind::widen, 4U, bytes / 4U, 0, 0};
			} else {
				operation = {FpKind::narrow, 8U, bytes / 8U, 0, 0};
				halves = true;
			}
			break;
		}
		/* CVTDQ2PS, CVTPS2DQ and CVTTPS2DQ with none, 66 and F3, and
		 * CVTTPD2DQ, CVTDQ2PD and CVTPD2DQ with 66, F3 and F2; CVTDQ2PD
		 * is exact. */
		if (opcode == 0xe6 && prefix == 2)
			return std::nullopt;
		const bool doubles = opcode == 0xe6;
		const bool from_integer = opcode == 0x5b && prefix == 0;
		operation = {from_integer ? FpKind::from_int : FpKind::to_int,
			doubles ? 8U : 4U, bytes / (doubles ? 8U : 4U), 0, 0};
		if (!from_integer && (prefix == 2 || (doubles && prefix == 1)))
			operation.flags = fp_to_int_truncate;
		halves = true;
		break;
	}
	case Operation::vector_scalar_convert: {
		/* CVTTPS2PI, CVTTPD2PI, CVTTSS2SI and CVTTSD2SI truncate, and
		 * the CVT forms round as MXCSR does, with none, 66, F3 and F2.
		 * Conversions from integers raise precision as the host does. */
		if (opcode != 0x2c && opcode != 0x2d)
			return std::nullopt;
		if (!read(rm, a))
			return std::nullopt;
		const bool dbl = prefix == 1 || prefix == 3;
		operation = {FpKind::to_int, dbl ? 8U : 4U,
			operand_bytes(rm) / (dbl ? 8U : 4U), 0, 0};
		if (opcode == 0x2c)
			operation.flags |= fp_to_int_truncate;
		if (prefix >= 2 && destination.width == OperandWidth::qword)
			operation.flags |= fp_to_int64;
		break;
	}
	case Operation::vector_map2_misc:
		/* VCVTPH2PS. */
		if (opcode != 0x13 || !read(rm, a))
			return std::nullopt;
		operation = {FpKind::half_widen, 2U, operand_bytes(rm) / 2U, 0, 0};
		break;
	case Operation::vector_integer_map3: {
		const unsigned control = instruction.vector_control;
		if (opcode >= 0x08 && opcode <= 0x0b) {
			/* ROUNDPS, ROUNDPD, ROUNDSS and ROUNDSD. */
			if (!read(rm, a))
				return std::nullopt;
			if (opcode >= 0x0a)
				scalar(FpKind::round, (opcode & 1U) != 0);
			else
				packed(FpKind::round, (opcode & 1U) != 0);
		} else if (opcode == 0x40 || opcode == 0x41) {
			if (!read(instruction.source, a) || !read(rm, b))
				return std::nullopt;
			packed(FpKind::dpp, opcode == 0x41);
		} else if (opcode == 0x1d) {
			/* VCVTPS2PH narrows the singles of its source. */
			if (!read(instruction.source, a))
				return std::nullopt;
			operation = {FpKind::half_narrow, 4U,
				operand_bytes(instruction.source) / 4U, 0, 0};
			halves = true;
		} else {
			return std::nullopt;
		}
		operation.immediate = static_cast<std::uint8_t>(control);
		break;
	}
	default:
		return std::nullopt;
	}
	FpSteps steps{};
	for (std::size_t index = 0; index < parts; ++index) {
		FpValues values{};
		if (halves) {
			values.sources[0] = part(a, 0);
			values.sources[1] = part(a, 1);
		} else {
			values.sources[0] = part(a, index);
			values.sources[1] = part(b, index);
			values.sources[2] = part(c, index);
		}
		steps |= x86_fp_compute(operation, cpu.mxcsr, values);
	}
	return steps;
}

void clear_host_floating_exceptions() noexcept
{
	__asm__ volatile("msr fpsr, xzr" ::: "memory");
}

std::uint32_t host_floating_exceptions() noexcept
{
	std::uint64_t status{};
	__asm__ volatile("mrs %0, fpsr" : "=r"(status) :: "memory");
	/* FPSR's IOC, DZC, OFC, UFC, IXC and IDC, as MXCSR's IE, ZE, OE, UE,
	 * PE and DE. */
	std::uint32_t flags{};
	if ((status & 0x01U) != 0)
		flags |= 0x01U;
	if ((status & 0x02U) != 0)
		flags |= 0x04U;
	if ((status & 0x04U) != 0)
		flags |= 0x08U;
	if ((status & 0x08U) != 0)
		flags |= 0x10U;
	if ((status & 0x10U) != 0)
		flags |= 0x20U;
	if ((status & 0x80U) != 0)
		flags |= 0x02U;
	return flags;
}

} // namespace uml::cpu::x86_64::gadgets
