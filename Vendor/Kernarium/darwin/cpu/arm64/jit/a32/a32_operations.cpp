// SPDX-License-Identifier: GPL-2.0-or-later

#include "a32_operations.hpp"

#include "cpu/host_capabilities.hpp"

#include <array>
#include <string_view>

namespace uml::cpu::arm64::jit::a32 {

namespace {

[[nodiscard]] const std::array<OpInfo, aarch32::kFormCount> &table() {
  static const auto built = [] {
    std::array<OpInfo, aarch32::kFormCount> result{};
    for (std::size_t index = 0; index < aarch32::kFormCount; ++index) {
      OpInfo info = aarch32::form_operation(static_cast<aarch32::Form>(index));
      if (aarch32::kForms[index].level == aarch32::Level::simd)
        info.op = Op::vector;
      else if (aarch32::kForms[index].level == aarch32::Level::complete)
        info.op = Op::library;
      result[index] = info;
    }
    return result;
  }();
  return built;
}

} // namespace

OpInfo op_info(aarch32::Form form) noexcept {
  return table()[static_cast<std::size_t>(form)];
}

namespace {

struct ExtNamed final {
  std::string_view mnemonic;
  Ext ext;
};

constexpr ExtNamed extension_mnemonics[] = {
    {"CRC32B", Ext::crc32},   {"CRC32H", Ext::crc32},
    {"CRC32W", Ext::crc32},   {"CRC32CB", Ext::crc32},
    {"CRC32CH", Ext::crc32},  {"CRC32CW", Ext::crc32},
    {"AESE", Ext::aes},       {"AESD", Ext::aes},
    {"AESMC", Ext::aes},      {"AESIMC", Ext::aes},
    {"SHA1C", Ext::sha},      {"SHA1P", Ext::sha},
    {"SHA1M", Ext::sha},      {"SHA1H", Ext::sha},
    {"SHA1SU0", Ext::sha},    {"SHA1SU1", Ext::sha},
    {"SHA256H", Ext::sha},    {"SHA256H2", Ext::sha},
    {"SHA256SU0", Ext::sha},  {"SHA256SU1", Ext::sha},
    {"VSELEQ", Ext::vsel},    {"VSELGE", Ext::vsel},
    {"VSELGT", Ext::vsel},    {"VSELVS", Ext::vsel},
    {"VMAXNM", Ext::vmaxnm},  {"VMINNM", Ext::vmaxnm},
    {"VRINTA", Ext::vrint},   {"VRINTN", Ext::vrint},
    {"VRINTP", Ext::vrint},   {"VRINTM", Ext::vrint},
    {"VRINTX", Ext::vrint},   {"VRINTZ", Ext::vrint},
    {"VRINTR", Ext::vrint},   {"VCVTA", Ext::vcvt_round},
    {"VCVTN", Ext::vcvt_round}, {"VCVTP", Ext::vcvt_round},
    {"VCVTM", Ext::vcvt_round}, {"VJCVT", Ext::vjcvt},
    {"SB", Ext::sb},
};

} // namespace

namespace {

/* The extension a complete-level form belongs to, from its mnemonic and,
 * for the by-element forms that share one, its name. */
Ext classify_extension(const aarch32::FormInfo &info) noexcept {
  if (info.level != aarch32::Level::complete)
    return Ext::none;
  const std::string_view name = info.name;
  const bool by_element = name.find("_s_") != std::string_view::npos;
  const std::string_view mnemonic = info.mnemonic;
  if (mnemonic == "VSDOT" || mnemonic == "VUDOT")
    return by_element ? Ext::dot_element : Ext::dot;
  if (mnemonic == "VQRDMLAH" || mnemonic == "VQRDMLSH")
    return Ext::rdm;
  if (mnemonic == "VCADD")
    return Ext::vcadd;
  if (mnemonic == "VCMLA")
    return by_element ? Ext::vcmla_element : Ext::vcmla;
  for (const auto &named : extension_mnemonics)
    if (named.mnemonic == mnemonic)
      return named.ext;
  return Ext::none;
}

const std::array<Ext, aarch32::kFormCount> &extension_table() {
  static const auto table = [] {
    std::array<Ext, aarch32::kFormCount> result{};
    for (std::size_t index = 0; index < aarch32::kFormCount; ++index)
      result[index] = classify_extension(aarch32::kForms[index]);
    return result;
  }();
  return table;
}

} // namespace

Ext extension_of(aarch32::Form form) noexcept {
  return extension_table()[static_cast<std::size_t>(form)];
}

bool extension_lowered(aarch32::Form form) noexcept {
  if (extension_of(form) == Ext::none)
    return false;
  /* The half-precision scalar forms are the library's. */
  const std::string_view name =
      aarch32::kForms[static_cast<std::size_t>(form)].name;
  return !name.ends_with("_H");
}

aarch32::Profile decode_profile() noexcept {
  return a32_decode_profile(::uml::cpu::host_capabilities());
}

bool is_library_form(aarch32::Form form) noexcept {
  const auto info = op_info(form);
  switch (info.op) {
  case Op::parallel:
    // The signed saturating forms; the others run in the vector unit.
    return info.mode == ParallelMode::saturating;
  case Op::ssat16: case Op::usat16: case Op::usad8: case Op::usada8:
    return true;
  case Op::library:
    return !extension_lowered(form);
  default:
    return false;
  }
}

bool ends_block(const aarch32::Instruction &inst) noexcept {
  if (inst.status == aarch32::Status::undefined)
    return true;
  if (inst.status == aarch32::Status::nop)
    return false;
  const auto &o = inst.operands;
  switch (op_info(inst.form).op) {
  case Op::b: case Op::bl: case Op::blx: case Op::bx: case Op::cbz:
  case Op::cbnz: case Op::tb: case Op::svc: case Op::bkpt: case Op::udf:
    return true;
  case Op::ldm:
    return (o.registers & 0x8000U) != 0U;
  case Op::ldr: case Op::ldrt:
    return o.t == 15;
  case Op::adc: case Op::add: case Op::addw: case Op::adr: case Op::and_:
  case Op::bic: case Op::eor: case Op::mov: case Op::mvn: case Op::orn:
  case Op::orr: case Op::rsb: case Op::rsc: case Op::sbc: case Op::sub:
  case Op::subw: case Op::movw:
    return o.d == 15;
  default:
    return false;
  }
}

bool continues_past(const aarch32::Instruction &inst) noexcept {
  if (inst.status != aarch32::Status::ok || inst.next_it.bits() != 0U)
    return false;
  const auto &o = inst.operands;
  switch (op_info(inst.form).op) {
  case Op::b:
    // A condition other than always, to a target after the branch.
    return inst.cond < 14U && static_cast<std::int32_t>(o.imm32) >= 0;
  case Op::cbz: case Op::cbnz:
    return true;
  default:
    return false;
  }
}

} // namespace uml::cpu::arm64::jit::a32
