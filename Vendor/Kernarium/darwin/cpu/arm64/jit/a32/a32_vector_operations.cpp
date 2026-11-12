// SPDX-License-Identifier: GPL-2.0-or-later

#include "a32_vector_operations.hpp"

#include <array>
#include <string_view>

namespace uml::cpu::arm64::jit::a32 {

namespace {

constexpr std::string_view mnemonics[] = {
  "fldmdbx", "fldmiax", "fstmdbx", "fstmiax", "vaba", "vabal", "vabd",
  "vabdl", "vabs", "vacge", "vacgt", "vadd", "vaddhn", "vaddl", "vaddw",
  "vand", "vbic", "vbif", "vbit", "vbsl", "vceq", "vcge", "vcgt", "vcle",
  "vcls", "vclt", "vclz", "vcmp", "vcmpe", "vcnt", "vcvt", "vcvtb", "vcvtr",
  "vcvtt", "vdiv", "vdup", "veor", "vext", "vfma", "vfms", "vfnma", "vfnms",
  "vhadd", "vhsub", "vld1", "vld2", "vld3", "vld4", "vldmdb", "vldm", "vldr",
  "vmax", "vmin", "vmla", "vmlal", "vmls", "vmlsl", "vmov", "vmovl", "vmovn",
  "vmrs", "vmsr", "vmul", "vmull", "vmvn", "vneg", "vnmla", "vnmls", "vnmul",
  "vorn", "vorr", "vpadal", "vpadd", "vpaddl", "vpmax", "vpmin", "vqabs",
  "vqadd", "vqdmlal", "vqdmlsl", "vqdmulh", "vqdmull", "vqmovn", "vqmovun",
  "vqneg", "vqrdmulh", "vqrshl", "vqrshrn", "vqrshrun", "vqshl", "vqshlu",
  "vqshrn", "vqshrun", "vqsub", "vraddhn", "vrecpe", "vrecps", "vrev16",
  "vrev32", "vrev64", "vrhadd", "vrshl", "vrshr", "vrshrn", "vrsqrte",
  "vrsqrts", "vrsra", "vrsubhn", "vshl", "vshll", "vshr", "vshrn", "vsli",
  "vsqrt", "vsra", "vsri", "vst1", "vst2", "vst3", "vst4", "vstmdb", "vstm",
  "vstr", "vsub", "vsubhn", "vsubl", "vsubw", "vswp", "vtbl", "vtbx", "vtrn",
  "vtst", "vuzp", "vzip",
};
static_assert(sizeof mnemonics / sizeof mnemonics[0] ==
              static_cast<std::size_t>(VM::vzip));

struct Spelled final {
  std::string_view text;
  Variant variant;
};
constexpr Spelled variants[] = {
    {"a", Variant::a},     {"d", Variant::d},   {"ds", Variant::ds},
    {"f", Variant::f},     {"hs", Variant::hs}, {"i", Variant::i},
    {"is", Variant::is},   {"l", Variant::l},   {"m", Variant::m},
    {"1", Variant::one},   {"r", Variant::r},   {"rs", Variant::rs},
    {"s", Variant::s},     {"sd", Variant::sd}, {"sh", Variant::sh},
    {"siv", Variant::siv}, {"sr", Variant::sr}, {"ss", Variant::ss},
    {"tod", Variant::tod}, {"tos", Variant::tos}, {"toss", Variant::toss},
    {"toxv", Variant::toxv}, {"uiv", Variant::uiv}, {"vi", Variant::vi},
    {"xs", Variant::xs},   {"xv", Variant::xv},
};

/* Whether `text` is `lower` with its letters in upper case. */
[[nodiscard]] bool equal_lower(std::string_view lower,
                               std::string_view text) noexcept {
  if (lower.size() != text.size())
    return false;
  for (std::size_t at = 0; at < text.size(); ++at)
    if ((text[at] | 0x20) != lower[at])
      return false;
  return true;
}

/* Mnemonics that are only VFP. */
[[nodiscard]] bool vfp_only(VM mnemonic) noexcept {
  switch (mnemonic) {
  case VM::vcmp: case VM::vcmpe: case VM::vcvtb: case VM::vcvtt:
  case VM::vcvtr: case VM::vdiv: case VM::vfnma: case VM::vfnms:
  case VM::vldr: case VM::vstr: case VM::vldm: case VM::vldmdb:
  case VM::vstm: case VM::vstmdb: case VM::vmrs: case VM::vmsr:
  case VM::vnmla: case VM::vnmls: case VM::vnmul: case VM::vsqrt:
  case VM::fldmdbx: case VM::fldmiax: case VM::fstmdbx: case VM::fstmiax:
    return true;
  default:
    return false;
  }
}

/* Mnemonics whose A2 encodings are NEON: the by-scalar forms, the 16-bit
 * modified immediates and the floating-point compares. */
[[nodiscard]] bool neon_scalar_only(VM mnemonic) noexcept {
  switch (mnemonic) {
  case VM::vqdmulh: case VM::vqrdmulh: case VM::vqdmull: case VM::vqdmlal:
  case VM::vqdmlsl: case VM::vbic: case VM::vorr: case VM::vmvn:
  case VM::vceq: case VM::vcge: case VM::vcgt:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] VInfo build(std::string_view name) noexcept {
  VInfo info;
  const auto first = name.find('_');
  const auto stem = name.substr(0, first);
  for (std::size_t index = 0; index < std::size(mnemonics); ++index)
    if (equal_lower(mnemonics[index], stem))
      info.mnemonic = static_cast<VM>(index + 1U);
  if (info.mnemonic == VM::none || first == std::string_view::npos)
    return info;
  std::string_view rest = name.substr(first + 1U);
  std::string_view last;
  while (!rest.empty()) {
    const auto cut = rest.find('_');
    const auto token = rest.substr(0, cut);
    rest = cut == std::string_view::npos ? std::string_view{}
                                         : rest.substr(cut + 1U);
    last = token;
    if ((token[0] == 'A' || token[0] == 'T') && token.size() == 2U &&
        token[1] >= '1' && token[1] <= '9') {
      info.encoding = static_cast<std::uint8_t>(token[1] - '0');
      continue;
    }
    if (token == "nowb") {
      info.writeback = Writeback::no_writeback;
      continue;
    }
    if (token == "posti") {
      info.writeback = Writeback::immediate;
      continue;
    }
    if (token == "postr") {
      info.writeback = Writeback::reg;
      continue;
    }
    if (token == "D" || token == "S" || token == "Q" || token == "SH" ||
        token == "HS" || token == "AS")
      continue;
    for (const auto &spelled : variants)
      if (spelled.text == token)
        info.variant = spelled.variant;
  }
  info.quad = last == "Q";
  if (vfp_only(info.mnemonic)) {
    info.vfp = true;
  } else if (info.mnemonic == VM::vcvt) {
    switch (info.variant) {
    case Variant::ds: case Variant::sd: case Variant::toxv: case Variant::xv:
    case Variant::uiv: case Variant::siv: case Variant::vi:
      info.vfp = true;
      break;
    default:
      break;
    }
  } else if (info.mnemonic == VM::vmov) {
    switch (info.variant) {
    case Variant::tos: case Variant::s: case Variant::tod: case Variant::d:
    case Variant::toss: case Variant::ss: case Variant::rs: case Variant::sr:
      info.vfp = true;
      break;
    default:
      info.vfp = info.encoding == 2U;
      break;
    }
  } else if (neon_scalar_only(info.mnemonic)) {
    info.vfp = false;
  } else {
    // The A2 encodings of the arithmetic mnemonics are the VFP ones, whose
    // names end in the precision (S or D); the NEON A2 forms of the
    // saturating multiplies are by scalar.
    info.vfp = info.encoding == 2U && (last == "S" || last == "D");
  }
  info.dp = info.vfp && last == "D";
  return info;
}

[[nodiscard]] const std::array<VInfo, aarch32::kFormCount> &table() {
  static const auto built = [] {
    std::array<VInfo, aarch32::kFormCount> result{};
    for (std::size_t index = 0; index < aarch32::kFormCount; ++index)
      if (aarch32::kForms[index].level == aarch32::Level::simd ||
          aarch32::kForms[index].level == aarch32::Level::complete)
        result[index] = build(aarch32::kForms[index].name);
    return result;
  }();
  return built;
}

} // namespace

const VInfo &vector_info(aarch32::Form form) noexcept {
  return table()[static_cast<std::size_t>(form)];
}

bool vector_memory(VM mnemonic) noexcept {
  switch (mnemonic) {
  case VM::vldr: case VM::vstr: case VM::vldm: case VM::vldmdb:
  case VM::vstm: case VM::vstmdb: case VM::fldmiax: case VM::fldmdbx:
  case VM::fstmiax: case VM::fstmdbx: case VM::vld1: case VM::vld2:
  case VM::vld3: case VM::vld4: case VM::vst1: case VM::vst2: case VM::vst3:
  case VM::vst4:
    return true;
  default:
    return false;
  }
}

bool vector_store(VM mnemonic) noexcept {
  switch (mnemonic) {
  case VM::vstr: case VM::vstm: case VM::vstmdb: case VM::fstmiax:
  case VM::fstmdbx: case VM::vst1: case VM::vst2: case VM::vst3:
  case VM::vst4:
    return true;
  default:
    return false;
  }
}

} // namespace uml::cpu::arm64::jit::a32
