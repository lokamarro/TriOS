// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/a32_profile.hpp"
#include "cpu/arm64/aarch32/form_operation.hpp"

#include <cstdint>

namespace uml::cpu::arm64::jit::a32 {

/* The decoder's operations (aarch32/form_operation.hpp) as the compiler reads
 * them: the forms of the simd level are Op::vector, which a32_vector_operations
 * names by mnemonic, and the forms of the complete level (the Armv8 additions)
 * are Op::library, which the compiler lowers by extension (Ext) or leaves to
 * the semantic library. */
using aarch32::Op;
using aarch32::ParallelKind;
using aarch32::ParallelMode;
using aarch32::Shape;
using OpInfo = aarch32::FormOperation;

[[nodiscard]] OpInfo op_info(aarch32::Form form) noexcept;

/* The load-exclusive and store-exclusive operations, with the acquire and
 * release forms. */
[[nodiscard]] constexpr bool is_exclusive_op(Op op) noexcept {
  return op >= Op::ldrex && op <= Op::stlexd;
}

/* The Armv8 extensions the compiler lowers, by instruction group. The forms
 * of the complete level outside these (and the half-precision ones inside
 * them) run in the semantic library. */
enum class Ext : std::uint8_t {
  none, crc32, aes, sha, vsel, vmaxnm, vrint, vcvt_round, vjcvt, sb,
  dot, dot_element, rdm, vcadd, vcmla, vcmla_element,
};
[[nodiscard]] Ext extension_of(aarch32::Form form) noexcept;
/* Whether the compiler lowers the form. The decoder presents a form only
 * when the host has its extension (decode_profile), so a lowered form's A64
 * instruction is always available. */
[[nodiscard]] bool extension_lowered(aarch32::Form form) noexcept;

/* The forms the compiler sends to the AArch32 semantic library every time
 * rather than lowering: the signed saturating parallel add and subtract
 * forms, SSAT16, USAT16 and USAD8/USADA8, and the complete-level forms it
 * leaves to the library. */
[[nodiscard]] bool is_library_form(aarch32::Form form) noexcept;

/* Whether the instruction ends a block: it writes the PC, raises an event or
 * is not an instruction. */
[[nodiscard]] bool ends_block(const aarch32::Instruction &inst) noexcept;
/* Whether a block that reaches this instruction goes on past it: a
 * conditional forward branch (B with a condition, CBZ, CBNZ) whose fall-through
 * is the likelier path and whose taken path leaves by a side exit. */
[[nodiscard]] bool continues_past(const aarch32::Instruction &inst) noexcept;

/* The profile the compiler and the terminal helper decode against: the
 * optional extensions the host has, which are the ones the guest is shown
 * (a32_decode_profile). A form of an extension the host lacks is UNDEFINED,
 * not run in the semantic library. */
[[nodiscard]] aarch32::Profile decode_profile() noexcept;

} // namespace uml::cpu::arm64::jit::a32
