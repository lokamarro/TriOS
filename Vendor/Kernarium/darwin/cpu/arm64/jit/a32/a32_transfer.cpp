// SPDX-License-Identifier: GPL-2.0-or-later

#include "a32_transfer.hpp"

#include <bit>
#include <string_view>

namespace uml::cpu::arm64::jit::a32 {

using aarch32::Instruction;

namespace {

struct Size final {
  std::uint8_t bytes;
  bool sign;
  bool load;
};

[[nodiscard]] bool single_size(Op op, Size &size) noexcept {
  switch (op) {
  case Op::ldr: case Op::ldrt: size = {4, false, true}; return true;
  case Op::ldrb: case Op::ldrbt: size = {1, false, true}; return true;
  case Op::ldrh: case Op::ldrht: size = {2, false, true}; return true;
  case Op::ldrsb: case Op::ldrsbt: size = {1, true, true}; return true;
  case Op::ldrsh: case Op::ldrsht: size = {2, true, true}; return true;
  case Op::lda: size = {4, false, true}; return true;
  case Op::ldab: size = {1, false, true}; return true;
  case Op::ldah: size = {2, false, true}; return true;
  case Op::stl: size = {4, false, false}; return true;
  case Op::stlb: size = {1, false, false}; return true;
  case Op::stlh: size = {2, false, false}; return true;
  case Op::str: case Op::strt: size = {4, false, false}; return true;
  case Op::strb: case Op::strbt: size = {1, false, false}; return true;
  case Op::strh: case Op::strht: size = {2, false, false}; return true;
  default: return false;
  }
}

[[nodiscard]] bool ordered(Op op) noexcept {
  switch (op) {
  case Op::lda: case Op::ldab: case Op::ldah: case Op::stl: case Op::stlb:
  case Op::stlh:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool unprivileged(Op op) noexcept {
  switch (op) {
  case Op::ldrt: case Op::ldrbt: case Op::ldrht: case Op::ldrsbt:
  case Op::ldrsht: case Op::strt: case Op::strbt: case Op::strht:
    return true;
  default:
    return false;
  }
}

} // namespace

Transfer describe_transfer(const Instruction &inst) noexcept {
  Transfer transfer;
  if (inst.status != aarch32::Status::ok)
    return transfer;
  const auto &o = inst.operands;
  const auto info = op_info(inst.form);
  Size size{};
  if (single_size(info.op, size)) {
    transfer.kind = Transfer::Kind::single;
    transfer.bytes = size.bytes;
    transfer.sign_extend = size.sign;
    transfer.load = size.load;
    transfer.t = static_cast<std::uint8_t>(o.t);
    transfer.add = o.add;
    if (info.shape == Shape::literal) {
      transfer.literal = true;
      transfer.imm = static_cast<std::uint32_t>(o.imm32);
      return transfer;
    }
    transfer.n = static_cast<std::uint8_t>(o.n);
    if (ordered(info.op)) {
      /* [Rn] alone. */
      transfer.add = true;
      transfer.ordered = true;
      return transfer;
    }
    if (unprivileged(info.op)) {
      transfer.index = !o.postindex;
      transfer.wback = o.postindex;
      transfer.register_offset = o.register_form;
    } else {
      transfer.index = o.index;
      transfer.wback = o.wback;
      transfer.register_offset = info.shape == Shape::reg;
    }
    if (transfer.register_offset) {
      transfer.m = static_cast<std::uint8_t>(o.m);
      transfer.shift_type = o.shift_t;
      transfer.shift_amount = static_cast<std::uint8_t>(o.shift_n);
    } else {
      transfer.imm = static_cast<std::uint32_t>(o.imm32);
    }
    return transfer;
  }
  switch (info.op) {
  case Op::ldrd:
  case Op::strd:
    transfer.kind = Transfer::Kind::pair;
    transfer.load = info.op == Op::ldrd;
    transfer.bytes = 8;
    transfer.t = static_cast<std::uint8_t>(o.t);
    transfer.t2 = static_cast<std::uint8_t>(o.t2);
    transfer.add = o.add;
    if (info.shape == Shape::literal) {
      transfer.literal = true;
      transfer.imm = static_cast<std::uint32_t>(o.imm32);
      return transfer;
    }
    transfer.n = static_cast<std::uint8_t>(o.n);
    transfer.index = o.index;
    transfer.wback = o.wback;
    transfer.register_offset = info.shape == Shape::reg;
    if (transfer.register_offset)
      transfer.m = static_cast<std::uint8_t>(o.m);
    else
      transfer.imm = static_cast<std::uint32_t>(o.imm32);
    return transfer;
  case Op::ldm:
  case Op::stm: {
    transfer.kind = Transfer::Kind::multiple;
    transfer.load = info.op == Op::ldm;
    transfer.bytes = 4;
    transfer.registers = o.registers;
    const std::string_view name = inst.info().name;
    if (name == "PUSH_T1" || name == "POP_T1") {
      transfer.n = 13;
      transfer.wback = true;
      transfer.mode = name == "PUSH_T1" ? Transfer::Mode::db
                                        : Transfer::Mode::ia;
      return transfer;
    }
    transfer.n = static_cast<std::uint8_t>(o.n);
    transfer.wback = o.wback;
    if (name.find("DA_") != std::string_view::npos)
      transfer.mode = Transfer::Mode::da;
    else if (name.find("DB_") != std::string_view::npos)
      transfer.mode = Transfer::Mode::db;
    else if (name.find("IB_") != std::string_view::npos)
      transfer.mode = Transfer::Mode::ib;
    return transfer;
  }
  case Op::tb:
    transfer.kind = Transfer::Kind::table;
    transfer.load = true;
    transfer.n = static_cast<std::uint8_t>(o.n);
    transfer.m = static_cast<std::uint8_t>(o.m);
    transfer.halfword_table = o.is_tbh;
    transfer.bytes = o.is_tbh ? 2 : 1;
    return transfer;
  case Op::ldrex: case Op::ldrexb: case Op::ldrexh: case Op::ldrexd:
  case Op::strex: case Op::strexb: case Op::strexh: case Op::strexd:
  case Op::ldaex: case Op::ldaexb: case Op::ldaexh: case Op::ldaexd:
  case Op::stlex: case Op::stlexb: case Op::stlexh: case Op::stlexd: {
    transfer.kind = Transfer::Kind::exclusive;
    const bool load = info.op == Op::ldrex || info.op == Op::ldrexb ||
                      info.op == Op::ldrexh || info.op == Op::ldrexd ||
                      info.op == Op::ldaex || info.op == Op::ldaexb ||
                      info.op == Op::ldaexh || info.op == Op::ldaexd;
    transfer.load = load;
    transfer.acquire = info.op == Op::ldaex || info.op == Op::ldaexb ||
                       info.op == Op::ldaexh || info.op == Op::ldaexd;
    transfer.release = info.op == Op::stlex || info.op == Op::stlexb ||
                       info.op == Op::stlexh || info.op == Op::stlexd;
    switch (info.op) {
    case Op::ldrexb: case Op::strexb: case Op::ldaexb: case Op::stlexb:
      transfer.bytes = 1;
      break;
    case Op::ldrexh: case Op::strexh: case Op::ldaexh: case Op::stlexh:
      transfer.bytes = 2;
      break;
    case Op::ldrexd: case Op::strexd: case Op::ldaexd: case Op::stlexd:
      transfer.bytes = 8;
      break;
    default:
      transfer.bytes = 4;
      break;
    }
    transfer.t = static_cast<std::uint8_t>(o.t);
    transfer.t2 = static_cast<std::uint8_t>(o.t2);
    transfer.n = static_cast<std::uint8_t>(o.n);
    transfer.d = static_cast<std::uint8_t>(o.d);
    transfer.imm = static_cast<std::uint32_t>(o.imm32);
    return transfer;
  }
  case Op::prefetch:
    transfer.kind = Transfer::Kind::prefetch;
    return transfer;
  default:
    return transfer;
  }
}

} // namespace uml::cpu::arm64::jit::a32
