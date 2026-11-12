// SPDX-License-Identifier: GPL-2.0-or-later
//
// The control families of the AArch32 semantic library: branches and
// interworking, exceptions, the APSR and the coprocessor registers the
// profile has, barriers and hints (semantics.hpp).

#include "cpu/arm64/aarch32/semantics_internal.hpp"

#include <atomic>

namespace uml::cpu::arm64::aarch32::detail {

bool exec_branch(Exec &x, const OpInfo &info)
{
	const Operands &o = x.o;
	switch (info.op) {
	case Op::b:
		x.branch_write_pc(x.pc_value() + static_cast<std::uint32_t>(o.imm32));
		return true;
	case Op::bl: {
		/* LR is the address of the next instruction, with the T bit in T32. */
		x.set_reg(reg_lr, x.thumb ? x.pc_value() | 1U : x.pc_value() - 4U);
		const bool to_thumb = o.targetInstrSet == InstrSet::T32;
		const std::uint32_t base = o.targetInstrSet == InstrSet::A32
			? x.pc_value() & ~3U : x.pc_value();
		x.branch_to(to_thumb, base + static_cast<std::uint32_t>(o.imm32));
		return true;
	}
	case Op::blx: {
		const std::uint32_t target = x.reg(o.m);
		x.set_reg(reg_lr, x.thumb ? (x.pc + 2U) | 1U : x.pc + 4U);
		x.bx_write_pc(target);
		return true;
	}
	case Op::bx:
		x.bx_write_pc(x.reg(o.m));
		return true;
	case Op::cbz:
	case Op::cbnz: {
		const bool zero = x.reg(o.n) == 0U;
		if (o.nonzero != zero)
			x.branch_write_pc(x.pc_value() + static_cast<std::uint32_t>(o.imm32));
		return true;
	}
	case Op::tb: {
		std::uint32_t entry = 0;
		if (o.is_tbh) {
			std::uint16_t half = 0;
			if (!x.read(x.reg(o.n) + (x.reg(o.m) << 1), &half, 2))
				return false;
			entry = half;
		} else {
			std::uint8_t byte = 0;
			if (!x.read(x.reg(o.n) + x.reg(o.m), &byte, 1))
				return false;
			entry = byte;
		}
		x.branch_write_pc(x.pc_value() + 2U * entry);
		return true;
	}
	default:
		return x.raise_fatal();
	}
}

namespace {

/* The host's generic timer, which the profile reads directly. */
[[nodiscard]] std::uint64_t read_counter() noexcept
{
	std::uint64_t value;
	asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(value) : : "memory");
	return value;
}

[[nodiscard]] std::uint64_t read_frequency() noexcept
{
	std::uint64_t value;
	asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
	return value;
}

} // namespace

bool exec_system(Exec &x, const OpInfo &info)
{
	const Operands &o = x.o;
	switch (info.op) {
	case Op::svc:
		x.result.event = StepEvent::supervisor_call;
		x.result.syndrome = static_cast<std::uint32_t>(o.imm32);
		return true;
	case Op::bkpt:
		x.result.event = StepEvent::breakpoint;
		x.result.syndrome = o.imm16;
		return false;
	case Op::udf:
		return x.raise_undefined();
	case Op::it:
	case Op::nop:
		return true;
	case Op::dmb:
	case Op::dsb:
	case Op::isb:
		std::atomic_thread_fence(std::memory_order_seq_cst);
		return true;
	case Op::mrs: {
		/* The APSR from User mode: N, Z, C, V, Q and GE, with the mode
		 * field reading User and the rest zero. */
		std::uint32_t value = (x.cpu.nzcv & 0xf000'0000U) | 0x10U;
		if (x.cpu.a32_q != 0U)
			value |= 1U << 27;
		for (unsigned lane = 0; lane < 4U; ++lane)
			if (x.ge(lane))
				value |= 1U << (16U + lane);
		x.set_reg(o.d, value);
		return true;
	}
	case Op::msr: {
		/* CPSRWriteByInstr() from User mode writes N, Z, C, V and Q with
		 * the mask's f field and the GE flags with its s field; the x and
		 * c fields reach privileged bits and are ignored (the decoder
		 * already rejected x). */
		const std::uint32_t value = info.shape == Shape::immediate
			? static_cast<std::uint32_t>(o.imm32) : x.reg(o.n);
		const unsigned mask = (x.inst.isa == Isa::a32 ? x.inst.word >> 16
							      : x.inst.word >> 8) & 0xfU;
		if ((mask & 8U) != 0U) {
			x.cpu.nzcv = (x.cpu.nzcv & 0x0fff'ffffU) | (value & 0xf000'0000U);
			x.cpu.a32_q = static_cast<std::uint8_t>(value >> 27 & 1U);
		}
		if ((mask & 4U) != 0U)
			for (unsigned lane = 0; lane < 4U; ++lane)
				x.set_ge_lane(lane, (value >> (16U + lane) & 1U) != 0U);
		return true;
	}
	case Op::coprocessor: {
		switch (x.inst.sysreg) {
		case SysReg::tpidruro:
			x.set_reg(o.t, x.cpu.a32_tpidruro);
			return true;
		case SysReg::tpidrurw_read:
			x.set_reg(o.t, static_cast<std::uint32_t>(x.cpu.tls));
			return true;
		case SysReg::tpidrurw_write:
			x.cpu.tls = x.reg(o.t);
			return true;
		case SysReg::cntfrq:
			x.set_reg(o.t, static_cast<std::uint32_t>(read_frequency()));
			return true;
		case SysReg::cntvct: {
			const std::uint64_t count = read_counter();
			x.set_reg(o.t, static_cast<std::uint32_t>(count));
			x.set_reg(o.t2, static_cast<std::uint32_t>(count >> 32));
			return true;
		}
		case SysReg::cp15_dmb:
		case SysReg::cp15_dsb:
		case SysReg::cp15_isb:
			/* The barrier registers execute only when the kernel
			 * enabled them (SCTLR.CP15BEN); otherwise the access is
			 * UNDEFINED. */
			if (x.cpu.a32_cp15ben == 0U)
				return x.raise_undefined();
			std::atomic_thread_fence(std::memory_order_seq_cst);
			return true;
		case SysReg::none:
			break;
		}
		return x.raise_undefined();
	}
	default:
		return x.raise_fatal();
	}
}

} // namespace uml::cpu::arm64::aarch32::detail
