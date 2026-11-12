// SPDX-License-Identifier: GPL-2.0-or-later
//
// Data processing: the sixteen ALU operations, ADR, ADDW, SUBW, MOVW and MOVT,
// with the A32 and T32 shifter, and the flags kept in the host NZCV as the A64
// compiler keeps them. Arithmetic maps one for one onto the host flags (the
// carry is "no borrow" on both). A logical operation sets N and Z from its
// result, C from the shifter's carry-out and leaves V; it reads the old flags
// into x11 first and restores what it keeps with RMIF (FEAT_FlagM, which
// every Apple Silicon host has).
//
// Registers: an operand lives in its resident register or a fallback (x9 for
// Rn and for a destination, x10 for Rm, x1 for Rs), x0, x1 and x8 are
// temporaries, x16 holds a register-shifted operand and x11 the NZCV image.

#include "a32_compiler.hpp"

#include "a32_encoders.hpp"
#include "a32_operations.hpp"

#include "cpu/arm64/aarch32/decode_support.hpp"

namespace uml::cpu::arm64::jit::a32 {

using aarch32::SRType;

namespace {

constexpr unsigned fallback_s = 1;
constexpr unsigned shifted = 16;
constexpr unsigned image = 11;
/* Where a predicated instruction builds its result. */
constexpr unsigned predicated_result = 2;

[[nodiscard]] bool is_logical(Op op) noexcept {
  switch (op) {
  case Op::and_: case Op::bic: case Op::eor: case Op::orr: case Op::orn:
  case Op::mov: case Op::mvn: case Op::tst: case Op::teq:
    return true;
  default:
    return false;
  }
}

/* An ADD or SUB immediate: the 12-bit field and whether it is shifted. */
[[nodiscard]] bool arithmetic_immediate(std::uint32_t value, unsigned &field,
                                        bool &lsl12) noexcept {
  if (value < 4096U) {
    field = value;
    lsl12 = false;
    return true;
  }
  if ((value & 0xfffU) == 0U && (value >> 12) < 4096U) {
    field = value >> 12;
    lsl12 = true;
    return true;
  }
  return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Registers, constants and flags.

std::uint32_t A32Compiler::pc_read() const noexcept {
  return cur().pc + (thumb_ ? 4U : 8U);
}

unsigned A32Compiler::use(unsigned r, unsigned fallback) {
  if (r == 15U) {
    load_u32(fallback, pc_read());
    return fallback;
  }
  if (probing_)
    ++uses_[r];
  return read_native_gpr(r, false, fallback).host;
}

A32Compiler::Dest A32Compiler::def(unsigned r, bool reads_old,
                                   unsigned fallback) {
  if (r == 15U)
    return {{fallback, false}, 15U};
  if (probing_)
    ++uses_[r];
  if (predicated_)
    return {{predicated_result, false}, r};
  return {write_native_gpr(r, false, reads_old, fallback), r};
}

void A32Compiler::done(const Dest &dest) {
  if (predicated_ && dest.guest != 15U) {
    const auto old = write_native_gpr(dest.guest, false, true, fallback_n);
    emit(enc::csel(old.host, predicated_result, old.host,
                   predicate_condition_));
    commit_native_gpr(old, dest.guest, false);
    return;
  }
  if (dest.guest != 15U)
    commit_native_gpr(dest.native, dest.guest, false);
}

void A32Compiler::load_u32(unsigned d, std::uint32_t value) {
  if ((value >> 16) == 0U) {
    emit(enc::movz(d, value));
  } else if ((value >> 16) == 0xffffU) {
    emit(enc::movn(d, ~value & 0xffffU));
  } else if ((value & 0xffffU) == 0U) {
    emit(enc::movz(d, value >> 16, 1));
  } else if ((value & 0xffffU) == 0xffffU) {
    emit(enc::movn(d, (~value >> 16) & 0xffffU, 1));
  } else {
    emit(enc::movz(d, value & 0xffffU));
    emit(enc::movk(d, value >> 16, 1));
  }
}

void A32Compiler::clobber_flags() { spill_flags_for_internal_clobber(); }

// ---------------------------------------------------------------------------
// Shifts.

/* The value of source shifted by an immediate, in `scratch` (or source itself
 * when nothing shifts). RRX reads the carry flag, so the host flags must hold
 * the guest's. */
unsigned A32Compiler::shift_value(unsigned source, SRType type,
                                  unsigned amount, unsigned scratch) {
  switch (type) {
  case SRType::LSL:
    if (amount == 0U)
      return source;
    emit(enc::lsl_i(scratch, source, amount));
    return scratch;
  case SRType::LSR:
    if (amount >= 32U)
      emit(enc::movz(scratch, 0));
    else
      emit(enc::lsr_i(scratch, source, amount));
    return scratch;
  case SRType::ASR:
    emit(enc::asr_i(scratch, source, amount >= 32U ? 31U : amount));
    return scratch;
  case SRType::ROR:
    emit(enc::ror_i(scratch, source, amount & 31U));
    return scratch;
  case SRType::RRX:
    load_flags();
    emit(enc::cset(temp2, enc::hs));
    emit(enc::extr(scratch, temp2, source, 1));
    return scratch;
  }
  return source;
}

/* out = rm shifted by the low byte of rs, for the register-controlled forms;
 * clobbers x0, x1 and the host flags. */
void A32Compiler::shift_by_register(unsigned out, unsigned rm, unsigned rs,
                                    SRType type) {
  clobber_flags();
  emit(enc::and_i(temp1, rs, *enc::logical_immediate_field(0xffU)));
  switch (type) {
  case SRType::LSL:
  case SRType::LSR:
    emit(type == SRType::LSL ? enc::lslv(temp0, rm, temp1)
                             : enc::lsrv(temp0, rm, temp1));
    emit(enc::cmp_i(temp1, 32));
    emit(enc::csel(out, temp0, 31, enc::lo));
    break;
  case SRType::ASR:
    emit(enc::movz(temp0, 31));
    emit(enc::cmp_i(temp1, 31));
    emit(enc::csel(temp1, temp1, temp0, enc::lo));
    emit(enc::asrv(out, rm, temp1));
    break;
  case SRType::ROR:
  case SRType::RRX:
    emit(enc::rorv(out, rm, temp1));
    break;
  }
}

/* As shift_by_register, and the new carry in bit 1 of x0 (the old one when the
 * amount is zero) taken from the NZCV image in x11. Clobbers x0, x1, x8. */
void A32Compiler::shift_by_register_carry(unsigned out, unsigned rm,
                                          unsigned rs, SRType type) {
  clobber_flags();
  emit(enc::and_i(temp1, rs, *enc::logical_immediate_field(0xffU)));
  switch (type) {
  case SRType::LSL:
    // X = rm | C << 32; carry is bit 32 of X << min(amount, 33).
    emit(enc::x::ubfx(temp0, image, 29, 1));
    emit(enc::x::orr(temp0, rm, temp0, enc::lsl, 32));
    emit(enc::movz(temp2, 33));
    emit(enc::cmp(temp1, temp2));
    emit(enc::csel(temp1, temp1, temp2, enc::lo));
    emit(enc::x::lslv(temp0, temp0, temp1));
    emit(enc::mov(out, temp0));
    emit(enc::x::lsr_i(temp0, temp0, 31));
    break;
  case SRType::LSR:
    // z = rm << 1 | C; carry is bit 0 of z >> min(amount, 33).
    emit(enc::x::ubfx(temp0, image, 29, 1));
    emit(enc::x::lsl_i(temp2, rm, 1));
    emit(enc::x::orr(temp0, temp2, temp0));
    emit(enc::movz(temp2, 33));
    emit(enc::cmp(temp1, temp2));
    emit(enc::csel(temp1, temp1, temp2, enc::lo));
    emit(enc::x::lsrv(temp0, temp0, temp1));
    emit(enc::x::lsr_i(temp2, temp0, 1));
    emit(enc::mov(out, temp2));
    emit(enc::x::lsl_i(temp0, temp0, 1));
    break;
  case SRType::ASR:
    emit(enc::x::sxtw(temp2, rm));
    emit(enc::x::lsl_i(temp2, temp2, 1));
    emit(enc::x::ubfx(temp0, image, 29, 1));
    emit(enc::x::orr(temp2, temp2, temp0));
    emit(enc::movz(temp0, 63));
    emit(enc::cmp(temp1, temp0));
    emit(enc::csel(temp1, temp1, temp0, enc::lo));
    emit(enc::x::asrv(temp2, temp2, temp1));
    emit(enc::x::asr_i(temp0, temp2, 1));
    emit(enc::mov(out, temp0));
    emit(enc::x::lsl_i(temp0, temp2, 1));
    break;
  case SRType::ROR:
  case SRType::RRX:
    emit(enc::rorv(out, rm, temp1));
    emit(enc::lsr_i(temp0, out, 30));
    emit(enc::x::lsr_i(temp2, image, 28));
    emit(enc::tst(temp1, temp1));
    emit(enc::csel(temp0, temp0, temp2, enc::ne));
    break;
  }
}

// ---------------------------------------------------------------------------
// The sixteen operations.

A32Compiler::Flow A32Compiler::emit_data_processing(const OpInfo &info) {
  const auto &inst = cur().inst;
  const auto &o = inst.operands;
  const Op op = info.op;
  const bool compare =
      op == Op::cmp || op == Op::cmn || op == Op::tst || op == Op::teq;
  const bool moves = op == Op::mov || op == Op::mvn;
  const bool logic = is_logical(op);
  const bool set = compare || o.setflags;
  const bool logic_flags = logic && set;
  const unsigned n_guest = info.sp_base ? 13U : static_cast<unsigned>(o.n);
  const unsigned d_guest = compare ? 31U : static_cast<unsigned>(o.d);
  const bool writes_pc = !compare && d_guest == 15U;

  // The immediate second operand and, for a logical operation, its carry.
  CarryKind carry = CarryKind::unchanged;
  unsigned carry_reg = 0;
  unsigned carry_rot = 0;
  bool is_imm = info.shape == Shape::immediate;
  std::uint32_t imm = 0;
  if (is_imm) {
    if (!logic) {
      imm = static_cast<std::uint32_t>(o.imm32);
    } else if (op == Op::mov) {
      imm = static_cast<std::uint32_t>(o.imm32);
      carry = o.carry_flags
                  ? CarryKind::unchanged
                  : (o.carry != 0 ? CarryKind::set : CarryKind::clear);
    } else {
      const auto expanded = o.a32 ? aarch32::detail::a32_expand_imm_c(o.imm)
                                  : aarch32::detail::t32_expand_imm_c(o.imm);
      imm = static_cast<std::uint32_t>(expanded.imm32);
      carry = expanded.carry_in
                  ? CarryKind::unchanged
                  : (expanded.carry != 0 ? CarryKind::set : CarryKind::clear);
    }
  }

  // A logical operation that sets the flags captures the old ones first.
  if (logic_flags)
    read_flags(image);

  unsigned n = 31;
  if (!moves)
    n = use(n_guest, fallback_n);

  // The register second operand.
  unsigned m = 0;
  bool fold = false;
  enc::Shift fold_shift = enc::lsl;
  unsigned fold_amount = 0;
  SRType shift_type = SRType::LSL;
  unsigned shift_amount = 0;
  bool register_shift = false;
  if (info.shape == Shape::reg) {
    m = use(o.m, fallback_m);
    shift_type = o.shift_t;
    shift_amount = static_cast<unsigned>(o.shift_n);
    switch (shift_type) {
    case SRType::LSL:
      if (shift_amount != 0U) {
        carry = CarryKind::bit;
        carry_reg = m;
        carry_rot = 31U - shift_amount;
      }
      fold = true;
      fold_shift = enc::lsl;
      fold_amount = shift_amount;
      break;
    case SRType::LSR:
      carry = CarryKind::bit;
      carry_reg = m;
      carry_rot = (shift_amount - 2U) & 63U;
      if (shift_amount >= 32U) {
        is_imm = true;
        imm = 0;
      } else {
        fold = true;
        fold_shift = enc::lsr;
        fold_amount = shift_amount;
      }
      break;
    case SRType::ASR:
      carry = CarryKind::bit;
      carry_reg = m;
      carry_rot = (shift_amount - 2U) & 63U;
      fold = true;
      fold_shift = enc::asr;
      fold_amount = shift_amount >= 32U ? 31U : shift_amount;
      break;
    case SRType::ROR:
      carry = CarryKind::bit;
      carry_reg = m;
      carry_rot = (shift_amount - 2U) & 63U;
      /* The arithmetic encodings have no rotate. */
      fold = logic;
      fold_shift = enc::ror;
      fold_amount = shift_amount;
      break;
    case SRType::RRX:
      carry = CarryKind::bit;
      carry_reg = m;
      carry_rot = 63U;
      break;
    }
  } else if (info.shape == Shape::reg_shifted) {
    register_shift = true;
    m = use(o.m, fallback_m);
    const unsigned s = use(o.s, fallback_s);
    if (logic_flags) {
      carry = CarryKind::computed;
      shift_by_register_carry(shifted, m, s, o.shift_t);
    } else {
      shift_by_register(shifted, m, s, o.shift_t);
    }
    m = shifted;
  }

  Dest dest{};
  if (!compare)
    dest = def(d_guest, false, fallback_n);
  unsigned result = compare ? 31U : dest.native.host;

  /* The value of the second operand as a register: shifted into x0 unless
   * the shift is folded into the instruction. */
  const auto operand_register = [&](bool allow_fold) -> unsigned {
    if (register_shift)
      return m;
    if (allow_fold && fold)
      return m;
    return shift_value(m, shift_type, shift_amount, temp0);
  };

  if (logic) {
    // A destination also used as the carry source is written last.
    const bool via_temporary = logic_flags && !compare &&
                               carry == CarryKind::bit && result == carry_reg;
    const unsigned out = via_temporary ? temp0 : result;
    const auto reg_op = [&](unsigned operand, enc::Shift sh, unsigned amt) {
      switch (op) {
      case Op::and_:
        emit(set ? enc::ands(out, n, operand, sh, amt)
                 : enc::and_(out, n, operand, sh, amt));
        break;
      case Op::tst:
        emit(enc::ands(31, n, operand, sh, amt));
        break;
      case Op::bic:
        emit(set ? enc::bics(out, n, operand, sh, amt)
                 : enc::bic(out, n, operand, sh, amt));
        break;
      case Op::orr:
        emit(enc::orr(out, n, operand, sh, amt));
        break;
      case Op::orn:
        emit(enc::orn(out, n, operand, sh, amt));
        break;
      case Op::eor:
        emit(enc::eor(out, n, operand, sh, amt));
        break;
      case Op::teq:
        emit(enc::eor(temp1, n, operand, sh, amt));
        break;
      case Op::mov:
        if (amt != 0U || sh != enc::lsl)
          emit(enc::orr(out, 31, operand, sh, amt));
        else if (out != operand)
          emit(enc::mov(out, operand));
        break;
      case Op::mvn:
        emit(enc::mvn(out, operand, sh, amt));
        break;
      default:
        break;
      }
    };
    if (is_imm) {
      const auto field = enc::logical_immediate_field(imm);
      const auto inverse = enc::logical_immediate_field(~imm);
      switch (op) {
      case Op::mov:
        load_u32(out, imm);
        break;
      case Op::mvn:
        load_u32(out, ~imm);
        break;
      case Op::and_:
      case Op::tst:
        if (field) {
          emit(op == Op::tst ? enc::ands_i(31, n, *field)
                             : (set ? enc::ands_i(out, n, *field)
                                    : enc::and_i(out, n, *field)));
        } else {
          load_u32(temp1, imm);
          reg_op(temp1, enc::lsl, 0);
        }
        break;
      case Op::bic:
        if (inverse) {
          emit(set ? enc::ands_i(out, n, *inverse)
                   : enc::and_i(out, n, *inverse));
        } else {
          load_u32(temp1, imm);
          reg_op(temp1, enc::lsl, 0);
        }
        break;
      case Op::orr:
      case Op::eor:
      case Op::orn: {
        const auto use_field = op == Op::orn ? inverse : field;
        if (use_field) {
          emit(op == Op::eor ? enc::eor_i(out, n, *use_field)
                             : enc::orr_i(out, n, *use_field));
        } else {
          load_u32(temp1, imm);
          reg_op(temp1, enc::lsl, 0);
        }
        break;
      }
      case Op::teq:
        if (field) {
          emit(enc::eor_i(temp1, n, *field));
        } else {
          load_u32(temp1, imm);
          emit(enc::eor(temp1, n, temp1));
        }
        break;
      default:
        break;
      }
    } else {
      const bool direct = fold && !register_shift;
      const unsigned operand = operand_register(true);
      reg_op(operand, direct ? fold_shift : enc::lsl, direct ? fold_amount : 0U);
    }
    if (logic_flags) {
      // Operations without a flag-setting form test their result.
      switch (op) {
      case Op::orr: case Op::orn: case Op::eor: case Op::mov: case Op::mvn:
        emit(enc::tst(out, out));
        break;
      case Op::teq:
        emit(enc::tst(temp1, temp1));
        break;
      default:
        break;
      }
      restore_logical_flags(carry, carry_reg, carry_rot);
      store_flags();
    }
    if (via_temporary)
      emit(enc::mov(result, temp0));
  } else {
    // Arithmetic.
    if (op == Op::adc || op == Op::sbc || op == Op::rsc)
      load_flags();
    const bool subtract = op == Op::sub || op == Op::cmp;
    const auto register_operand = [&](unsigned scratch) -> unsigned {
      if (is_imm) {
        load_u32(scratch, imm);
        return scratch;
      }
      return operand_register(false);
    };
    switch (op) {
    case Op::add: case Op::sub: case Op::cmp: case Op::cmn: {
      unsigned field = 0;
      bool lsl12 = false;
      if (is_imm && arithmetic_immediate(imm, field, lsl12)) {
        if (subtract)
          emit(set ? enc::subs_i(result, n, field, lsl12)
                   : enc::sub_i(result, n, field, lsl12));
        else
          emit(set ? enc::adds_i(result, n, field, lsl12)
                   : enc::add_i(result, n, field, lsl12));
      } else if (is_imm) {
        load_u32(temp1, imm);
        if (subtract)
          emit(set ? enc::subs(result, n, temp1) : enc::sub(result, n, temp1));
        else
          emit(set ? enc::adds(result, n, temp1) : enc::add(result, n, temp1));
      } else {
        const bool direct = fold && !register_shift;
        const unsigned operand = operand_register(true);
        const enc::Shift sh = direct ? fold_shift : enc::lsl;
        const unsigned amt = direct ? fold_amount : 0U;
        if (subtract)
          emit(set ? enc::subs(result, n, operand, sh, amt)
                   : enc::sub(result, n, operand, sh, amt));
        else
          emit(set ? enc::adds(result, n, operand, sh, amt)
                   : enc::add(result, n, operand, sh, amt));
      }
      break;
    }
    case Op::rsb:
      if (is_imm && imm == 0U) {
        emit(set ? enc::subs(result, 31, n) : enc::sub(result, 31, n));
      } else {
        const unsigned operand = register_operand(temp1);
        emit(set ? enc::subs(result, operand, n) : enc::sub(result, operand, n));
      }
      break;
    case Op::adc:
    case Op::sbc: {
      const unsigned operand = register_operand(temp1);
      if (op == Op::adc)
        emit(set ? enc::adcs(result, n, operand) : enc::adc(result, n, operand));
      else
        emit(set ? enc::sbcs(result, n, operand) : enc::sbc(result, n, operand));
      break;
    }
    case Op::rsc: {
      const unsigned operand = register_operand(temp1);
      emit(set ? enc::sbcs(result, operand, n) : enc::sbc(result, operand, n));
      break;
    }
    default:
      break;
    }
    if (set)
      store_flags();
  }

  if (writes_pc) {
    write_pc(dest.native.host);
    return Flow::end;
  }
  if (!compare)
    done(dest);
  return Flow::next;
}

/* After a logical operation set N and Z, with C and V clear: V keeps its old
 * value and C takes the shifter's carry-out (old image in x11). */
void A32Compiler::restore_logical_flags(CarryKind carry, unsigned carry_reg,
                                        unsigned carry_rot) {
  switch (carry) {
  case CarryKind::unchanged:
    emit(enc::rmif(image, 28, 3));
    return;
  case CarryKind::clear:
    emit(enc::rmif(image, 28, 1));
    return;
  case CarryKind::set:
    emit(enc::movz(temp1, 2));
    emit(enc::rmif(temp1, 0, 2));
    emit(enc::rmif(image, 28, 1));
    return;
  case CarryKind::bit:
    emit(enc::rmif(carry_reg, carry_rot, 2));
    emit(enc::rmif(image, 28, 1));
    return;
  case CarryKind::computed:
    emit(enc::rmif(temp0, 0, 2));
    emit(enc::rmif(image, 28, 1));
    return;
  }
}

/* ADR, ADDW, SUBW, MOVW and MOVT. */
A32Compiler::Flow A32Compiler::emit_address_forms(const OpInfo &info) {
  const auto &o = cur().inst.operands;
  const unsigned d_guest = static_cast<unsigned>(o.d);
  switch (info.op) {
  case Op::adr: {
    const std::uint32_t base = pc_align_read();
    const std::uint32_t value = o.add ? base + static_cast<std::uint32_t>(o.imm32)
                                      : base - static_cast<std::uint32_t>(o.imm32);
    const auto dest = def(d_guest, false, fallback_n);
    load_u32(dest.native.host, value);
    if (d_guest == 15U) {
      write_pc(dest.native.host);
      return Flow::end;
    }
    done(dest);
    return Flow::next;
  }
  case Op::addw:
  case Op::subw: {
    const unsigned n_guest = info.sp_base ? 13U : static_cast<unsigned>(o.n);
    const auto imm = static_cast<std::uint32_t>(o.imm32);
    const unsigned n = use(n_guest, fallback_n);
    const auto dest = def(d_guest, false, fallback_n);
    if (info.op == Op::addw)
      emit(enc::add_i(dest.native.host, n, imm));
    else
      emit(enc::sub_i(dest.native.host, n, imm));
    done(dest);
    return Flow::next;
  }
  case Op::movw: {
    const auto dest = def(d_guest, false, fallback_n);
    load_u32(dest.native.host, static_cast<std::uint32_t>(o.imm32));
    done(dest);
    return Flow::next;
  }
  case Op::movt: {
    const auto dest = def(d_guest, true, fallback_n);
    emit(enc::movk(dest.native.host, o.imm16, 1));
    done(dest);
    return Flow::next;
  }
  default:
    return Flow::next;
  }
}

} // namespace uml::cpu::arm64::jit::a32
