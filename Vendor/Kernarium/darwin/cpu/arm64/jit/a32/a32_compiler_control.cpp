// SPDX-License-Identifier: GPL-2.0-or-later
//
// The operation switch, branches and interworking, exceptions, hints and
// barriers, the APSR and the coprocessor registers EL0 may reach.

#include "a32_compiler.hpp"

#include "a32_encoders.hpp"
#include "a32_operations.hpp"

namespace uml::cpu::arm64::jit::a32 {

using aarch32::InstrSet;
using aarch32::Status;

A32Compiler::Flow A32Compiler::emit_operation() {
  const auto &inst = cur().inst;
  if (inst.status == Status::nop)
    return Flow::next;
  const auto info = op_info(inst.form);
  switch (info.op) {
  case Op::adc: case Op::add: case Op::and_: case Op::bic: case Op::cmn:
  case Op::cmp: case Op::eor: case Op::mov: case Op::mvn: case Op::orn:
  case Op::orr: case Op::rsb: case Op::rsc: case Op::sbc: case Op::sub:
  case Op::teq: case Op::tst:
    return emit_data_processing(info);
  case Op::adr: case Op::addw: case Op::subw: case Op::movw: case Op::movt:
    return emit_address_forms(info);
  case Op::mul: case Op::mla: case Op::mls: case Op::umull: case Op::umlal:
  case Op::smull: case Op::smlal: case Op::umaal: case Op::sdiv: case Op::udiv:
    return emit_multiply(info);
  case Op::bfc: case Op::bfi: case Op::sbfx: case Op::ubfx: case Op::clz:
  case Op::rbit: case Op::rev: case Op::rev16: case Op::revsh:
    return emit_bit_operation(info);
  case Op::sxtb: case Op::sxth: case Op::uxtb: case Op::uxth: case Op::sxtab:
  case Op::sxtah: case Op::uxtab: case Op::uxtah: case Op::sxtb16:
  case Op::uxtb16: case Op::sxtab16: case Op::uxtab16: case Op::pkh:
  case Op::sel:
    return emit_extend(info);
  case Op::qadd: case Op::qsub: case Op::qdadd: case Op::qdsub: case Op::ssat:
  case Op::usat:
    return emit_saturate(info);
  case Op::smla_xy: case Op::smul_xy: case Op::smlal_xy: case Op::smlaw:
  case Op::smulw: case Op::smlad: case Op::smlsd: case Op::smuad:
  case Op::smusd: case Op::smlald: case Op::smlsld: case Op::smmla:
  case Op::smmls: case Op::smmul:
    return emit_dsp_multiply(info);
  case Op::parallel:
    return emit_parallel(info);
  case Op::ssat16: case Op::usat16: case Op::usad8: case Op::usada8:
    return emit_arith_through_helper();
  case Op::library:
    return emit_extension(info);
  case Op::b: case Op::bl: case Op::blx: case Op::bx: case Op::cbz:
  case Op::cbnz:
    return emit_branch(info);
  case Op::svc:
    exit_event(Event::system_call, next_key(), inst.next_it.bits());
    return Flow::end;
  case Op::bkpt:
    exit_event(Event::breakpoint, key_of(cur()), cur().it);
    return Flow::end;
  case Op::it:
  case Op::nop:
    return Flow::next;
  case Op::dmb:
    emit(barrier_option_is_store() ? enc::dmb_ishst() : enc::dmb_ish());
    return Flow::next;
  case Op::dsb:
    emit(enc::dsb_sy());
    return Flow::next;
  case Op::isb:
    emit(enc::isb());
    emit_code_change_check();
    return Flow::next;
  case Op::clrex:
    emit(0xd503'3f5fU); // clrex
    clear_exclusive_monitor();
    return Flow::next;
  case Op::ldr: case Op::ldrb: case Op::ldrh: case Op::ldrsb: case Op::ldrsh:
  case Op::ldrd: case Op::ldm: case Op::str: case Op::strb: case Op::strh:
  case Op::strd: case Op::stm: case Op::ldrt: case Op::ldrbt: case Op::ldrht:
  case Op::ldrsbt: case Op::ldrsht: case Op::strt: case Op::strbt:
  case Op::strht: case Op::ldrex: case Op::ldrexb: case Op::ldrexh:
  case Op::ldrexd: case Op::strex: case Op::strexb: case Op::strexh:
  case Op::strexd: case Op::ldaex: case Op::ldaexb: case Op::ldaexh:
  case Op::ldaexd: case Op::stlex: case Op::stlexb: case Op::stlexh:
  case Op::stlexd: case Op::lda: case Op::ldab: case Op::ldah: case Op::stl:
  case Op::stlb: case Op::stlh: case Op::tb: case Op::prefetch:
    return emit_memory(info);
  case Op::mrs:
  case Op::msr:
    return emit_status_register(info);
  case Op::coprocessor:
    return emit_coprocessor();
  case Op::vector:
    return emit_vector(info);
  default:
    return emit_other(info);
  }
}

bool A32Compiler::barrier_option_is_store() const noexcept {
  const auto word = cur().inst.word;
  return (word & 3U) == 2U;
}

// ---------------------------------------------------------------------------
// Writing the PC.

/* The key of the interworking branch target in `value` (BXWritePC): bit 0
 * selects T32, which takes the halfword address shifted up one bit; an A32
 * target keeps its low bits, so bit 1 reaches the PC-alignment exit. The key
 * is left in x8. */
void A32Compiler::build_interwork_key(unsigned value) {
  emit(enc::and_i(0, value, *enc::logical_immediate_field(1U)));
  emit(enc::x::lslv(8, value, 0));
  emit(enc::lsl_i(1, 0, 1));
  emit(enc::x::bic(8, 8, 1));
  emit(enc::x::lsl_i(0, 0, 62));
  emit(enc::x::movz(1, 0x8000U, 3)); // 1 << 63
  emit(enc::x::orr(0, 0, 1));
  emit(enc::x::orr(8, 8, 0));
}

/* The key of a Thumb branch target in `value` (BranchWritePC): bit 0 is
 * ignored. The key is left in x8. */
void A32Compiler::build_thumb_key(unsigned value) {
  emit(enc::and_i(0, value, *enc::logical_immediate_field(0xffff'fffeU)));
  emit(enc::x::lsl_i(8, 0, 1));
  emit(enc::x::movz(1, 0xc000U, 3)); // 3 << 62
  emit(enc::x::orr(8, 8, 1));
}

void A32Compiler::write_pc(unsigned value, bool interwork) {
  if (interwork)
    build_interwork_key(value);
  else
    build_thumb_key(value);
  exit_indirect(8);
}

// ---------------------------------------------------------------------------
// Branches.

std::uint32_t A32Compiler::link_value() const noexcept {
  const auto &d = cur();
  return d.pc + d.inst.length + (thumb_ ? 1U : 0U);
}

A32Compiler::Flow A32Compiler::emit_branch(const OpInfo &info) {
  const auto &inst = cur().inst;
  const auto &o = inst.operands;
  const auto target_key_of = [&](std::uint32_t target, bool thumb_target) {
    return thumb_target ? aarch32::t32_key(target & ~1U)
                        : aarch32::a32_key(target & ~3U);
  };
  switch (info.op) {
  case Op::b: {
    const std::uint32_t target = pc_read() + static_cast<std::uint32_t>(o.imm32);
    exit_direct(target_key_of(target, thumb_));
    return Flow::end;
  }
  case Op::bl:
  case Op::blx:
    if (info.shape == Shape::immediate || info.op == Op::bl) {
      const std::uint32_t base =
          info.op == Op::blx ? pc_align_read() : pc_read();
      const std::uint32_t target = base + static_cast<std::uint32_t>(o.imm32);
      const bool to_thumb = o.targetInstrSet == InstrSet::T32;
      const auto link = def(14, false, fallback_n);
      load_u32(link.native.host, link_value());
      done(link);
      exit_direct(target_key_of(target, to_thumb));
      return Flow::end;
    } else {
      const unsigned m = use(static_cast<unsigned>(o.m), fallback_m);
      build_interwork_key(m);
      const auto link = def(14, false, fallback_n);
      load_u32(link.native.host, link_value());
      done(link);
      exit_indirect(8);
      return Flow::end;
    }
  case Op::bx: {
    const unsigned m = use(static_cast<unsigned>(o.m), fallback_m);
    build_interwork_key(m);
    exit_indirect(8);
    return Flow::end;
  }
  case Op::cbz:
  case Op::cbnz: {
    const std::uint32_t target = pc_read() + static_cast<std::uint32_t>(o.imm32);
    const unsigned n = use(static_cast<unsigned>(o.n), fallback_n);
    /* CBZ does not touch NZCV, but both successors' link checks do. */
    synchronize_flags();
    const auto not_taken = make_label();
    compare_zero(not_taken, n, false, info.op == Op::cbz);
    exit_direct(aarch32::t32_key(target));
    bind(not_taken);
    exit_direct(next_key(), inst.next_it.bits(), count_ + 1U);
    return Flow::end;
  }
  default:
    break;
  }
  exit_illegal();
  return Flow::end;
}

// ---------------------------------------------------------------------------
// The APSR.

A32Compiler::Flow A32Compiler::emit_status_register(const OpInfo &info) {
  const auto &inst = cur().inst;
  const auto &o = inst.operands;
  if (info.op == Op::mrs) {
    if (o.read_spsr) {
      exit_illegal();
      return Flow::end;
    }
    const auto dest = def(static_cast<unsigned>(o.d), false, fallback_n);
    const unsigned d = dest.native.host;
    // NZCV, then Q at bit 27, then GE[3:0] at bits 19:16.
    read_flags(d);
    emitter_.load_b(0, 15, offsetof(CpuState, a32_q));
    emit(enc::lsl_i(0, 0, 27));
    emit(enc::orr(d, d, 0));
    emitter_.load_w(0, 15, offsetof(CpuState, a32_ge_mask));
    emit(enc::and_i(0, 0, *enc::logical_immediate_field(0x8080'8080U)));
    emit(enc::lsr_i(0, 0, 7));
    emit(enc::movz(1, 0x0408U));
    emit(enc::movk(1, 0x0102U, 1));
    emit(enc::mul(0, 0, 1));
    emit(enc::lsr_i(0, 0, 8));
    emit(enc::and_i(0, 0, *enc::logical_immediate_field(0x000f'0000U)));
    emit(enc::orr(d, d, 0));
    // A read of the status register at EL0 shows User mode (M = 0b10000).
    emit(enc::orr_i(d, d, *enc::logical_immediate_field(0x10U)));
    done(dest);
    return Flow::next;
  }
  // MSR to the APSR: mask bit 3 writes N, Z, C, V and Q, bit 2 writes GE.
  if (o.write_spsr) {
    exit_illegal();
    return Flow::end;
  }
  const std::uint32_t word = inst.word;
  const unsigned mask = thumb_ ? (word >> 8) & 15U : (word >> 16) & 15U;
  unsigned value;
  if (info.shape == Shape::immediate) {
    load_u32(fallback_n, static_cast<std::uint32_t>(o.imm32));
    value = fallback_n;
  } else {
    value = use(static_cast<unsigned>(o.n), fallback_n);
  }
  if ((mask & 8U) != 0U) {
    emit(enc::msr_nzcv(value));
    store_flags();
    emit(enc::ubfx(0, value, 27, 1));
    emitter_.store_b(0, 15, offsetof(CpuState, a32_q));
  }
  if ((mask & 4U) != 0U) {
    // GE[3:0] = value<19:16>, widened to a byte each.
    emit(enc::ubfx(0, value, 16, 4));
    emit(enc::movz(1, 0x4081U));
    emit(enc::movk(1, 0x0020U, 1));
    emit(enc::mul(0, 0, 1));
    emit(enc::and_i(0, 0, *enc::logical_immediate_field(0x0101'0101U)));
    emit(enc::movz(1, 0xffU));
    emit(enc::mul(0, 0, 1));
    emitter_.store_w(0, 15, offsetof(CpuState, a32_ge_mask));
  }
  return Flow::next;
}

// ---------------------------------------------------------------------------
// The coprocessor accesses the profile allows.

A32Compiler::Flow A32Compiler::emit_coprocessor() {
  const auto &inst = cur().inst;
  const auto &o = inst.operands;
  using aarch32::SysReg;
  switch (inst.sysreg) {
  case SysReg::tpidruro:
  case SysReg::tpidrurw_read:
  case SysReg::cntfrq: {
    const unsigned t = static_cast<unsigned>(o.t);
    if (t == 15U) {
      // MRC to the PC transfers the flags: APSR_nzcv takes bits 31:28.
      if (inst.sysreg == SysReg::tpidruro)
        emitter_.load_w(0, 15, offsetof(CpuState, a32_tpidruro));
      else if (inst.sysreg == SysReg::tpidrurw_read)
        emitter_.load_w(0, 15, offsetof(CpuState, tls));
      else
        emit(0xd53b'e000U); // mrs x0, cntfrq_el0
      emit(enc::msr_nzcv(0));
      store_flags();
      return Flow::next;
    }
    const auto dest = def(t, false, fallback_n);
    if (inst.sysreg == SysReg::tpidruro)
      emitter_.load_w(dest.native.host, 15, offsetof(CpuState, a32_tpidruro));
    else if (inst.sysreg == SysReg::tpidrurw_read)
      emitter_.load_w(dest.native.host, 15, offsetof(CpuState, tls));
    else
      emit(0xd53b'e000U | dest.native.host); // mrs xN, cntfrq_el0
    done(dest);
    return Flow::next;
  }
  case SysReg::tpidrurw_write: {
    const unsigned source = use(static_cast<unsigned>(o.t), fallback_n);
    emitter_.store_x(source, 15, offsetof(CpuState, tls));
    return Flow::next;
  }
  case SysReg::cntvct: {
    // MRRC p15, 1, Rt, Rt2, c14: the 64-bit virtual count.
    const unsigned t = static_cast<unsigned>(o.t);
    const unsigned t2 = static_cast<unsigned>(o.t2);
    emit(0xd53b'e040U); // mrs x0, cntvct_el0
    const auto low = def(t, false, fallback_n);
    emit(enc::mov(low.native.host, 0));
    done(low);
    const auto high = def(t2, false, fallback_n);
    emit(enc::x::lsr_i(high.native.host, 0, 32));
    done(high);
    return Flow::next;
  }
  case SysReg::cp15_dsb:
  case SysReg::cp15_dmb:
  case SysReg::cp15_isb: {
    // The CP15 barriers are UNDEFINED while SCTLR.CP15BEN is clear.
    const auto undefined = cold_exit(ColdPath::Kind::illegal);
    emitter_.load_b(0, 15, offsetof(CpuState, a32_cp15ben));
    compare_zero(undefined, 0, false, false);
    if (inst.sysreg == SysReg::cp15_isb) {
      emit(enc::isb());
      emit_code_change_check();
    } else if (inst.sysreg == SysReg::cp15_dmb)
      emit(enc::dmb_ish());
    else
      emit(enc::dsb_sy());
    return Flow::next;
  }
  case SysReg::none:
    break;
  }
  exit_illegal();
  return Flow::end;
}

A32Compiler::Flow A32Compiler::emit_other(const OpInfo &) {
  exit_illegal();
  return Flow::end;
}

} // namespace uml::cpu::arm64::jit::a32
