// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/sme_profile.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64::aarch32 {

/* The key of a translation, and of CpuState::pc for the whole entry of an
 * AArch32 task, so that one gadget ProgramCache and one JIT arena, block
 * cache, link table and shared tier serve A64, A32 and T32 code with no
 * second instance of any of them.
 *
 *   A64  pc | svcr_mode    a user PC lies below 2^48; the two low bits are
 *        | tbi << 61       the SME mode, and bit 61 is set for a translation
 *                          that strips the top byte of its data addresses
 *   A32  1<<63 | pc        pc is a 32-bit word-aligned address
 *   T32  1<<63 | 1<<62 | pc << 1
 *                          pc is a 32-bit halfword-aligned address; the
 *                          shift moves the halfword bit up to bit 2, where
 *                          every cache hash starts
 *
 * An AArch32 key never equals an A64 key, because bit 63 is set, or the
 * ~0 an empty cell or ring record holds, because bit 0 is clear. A valid
 * key (key_is_canonical) has both low bits clear, so the translation_key_mode
 * the JIT reads from it is 0 and the PC-alignment test that guards every
 * block entry passes. The one AArch32 PC that fails it is an A32 target with
 * bit 1 set, which is a PC-alignment exit before it is ever a key. A fault
 * position (program.hpp) has bit 0 set and so is never a key either.
 *
 * Interworking is the key: bit 0 of a BX, BLX, LDR-to-PC or POP-to-PC target
 * selects T32, so the key carries PSTATE.T and no stub switches the state.
 * The IT state is not part of the key: a block never ends inside an IT
 * block, and an entry in the middle of one runs a single instruction at a
 * time. CPSR.E is not part of it because SETEND is UNDEFINED. */

/* How a translation's guest words are laid out and fetched. */
enum class GuestIsa : std::uint8_t {
	a64,
	a32,
	t32,
};

inline constexpr std::uint64_t aarch32_key_bit = std::uint64_t{1} << 63;
inline constexpr std::uint64_t thumb_key_bit = std::uint64_t{1} << 62;
/* An A64 key's top-byte-ignore bit: the translation was compiled for a guest
 * whose data addresses ignore their top byte. */
inline constexpr std::uint64_t a64_tbi_key_bit = std::uint64_t{1} << 61;

/* `pc` is word aligned. */
[[nodiscard]] constexpr std::uint64_t a32_key(std::uint32_t pc) noexcept
{
	return aarch32_key_bit | pc;
}

/* `pc` is halfword aligned (bit 0 clear). */
[[nodiscard]] constexpr std::uint64_t t32_key(std::uint32_t pc) noexcept
{
	return aarch32_key_bit | thumb_key_bit | std::uint64_t{pc} << 1;
}

/* BXWritePC(): bit 0 of an interworking branch target selects T32 and is
 * not part of the address. An A32 target keeps bit 1, so a target with it
 * set stays a misaligned PC and not a different one. */
[[nodiscard]] constexpr std::uint64_t key_from_target(
	std::uint32_t value) noexcept
{
	return (value & 1U) != 0U ? t32_key(value & ~1U) : a32_key(value);
}

[[nodiscard]] constexpr bool key_is_aarch32(std::uint64_t key) noexcept
{
	return (key & aarch32_key_bit) != 0U;
}

[[nodiscard]] constexpr bool key_is_thumb(std::uint64_t key) noexcept
{
	return key_is_aarch32(key) && (key & thumb_key_bit) != 0U;
}

[[nodiscard]] constexpr GuestIsa key_isa(std::uint64_t key) noexcept
{
	return !key_is_aarch32(key) ? GuestIsa::a64 :
		(key & thumb_key_bit) != 0U ? GuestIsa::t32 : GuestIsa::a32;
}

/* The AArch32 PC a key names (bit 0 clear; PSTATE.T is key_is_thumb()). */
[[nodiscard]] constexpr std::uint32_t key_pc(std::uint64_t key) noexcept
{
	return static_cast<std::uint32_t>(key_is_thumb(key) ? key >> 1 : key);
}

/* The interworking value of a key, as BL, BLX and the exception model put
 * it in LR or a branch target register: the PC with the T bit in bit 0. */
[[nodiscard]] constexpr std::uint32_t key_target_value(
	std::uint64_t key) noexcept
{
	return key_pc(key) | (key_is_thumb(key) ? 1U : 0U);
}

/* Whether `key` is one the engines ever store a translation under: an A64
 * key has no AArch32 bit and an AArch32 key has nothing outside its 32-bit
 * address, the thumb bit and both low bits clear. */
[[nodiscard]] constexpr bool key_is_canonical(std::uint64_t key) noexcept
{
	if (!key_is_aarch32(key))
		return true;
	if ((key & 3U) != 0U)
		return false;
	return key_is_thumb(key) ?
		(key & ~(aarch32_key_bit | thumb_key_bit)) <=
			(std::uint64_t{0xffff'ffffU} << 1) :
		(key & ~aarch32_key_bit) <= 0xffff'ffffU;
}

/* The bytes a guest instruction word of the ISA occupies in a validation
 * span: a unit is a 32-bit word for A64 and A32 and a 16-bit halfword for
 * T32, in which an instruction takes one or two. */
[[nodiscard]] constexpr std::size_t isa_unit_bytes(GuestIsa isa) noexcept
{
	return isa == GuestIsa::t32 ? 2U : 4U;
}

/* Where the guest bytes of a translation start. An A64 key may carry the
 * SVCR mode in its two low bits and the top-byte-ignore bit; the gadget
 * engine's keys carry neither. */
[[nodiscard]] constexpr std::uint64_t key_fetch_address(
	std::uint64_t key) noexcept
{
	return key_is_aarch32(key) ? key_pc(key) :
		key & ~(std::uint64_t{svcr_mask} | a64_tbi_key_bit);
}

static_assert(a32_key(0) == aarch32_key_bit);
static_assert(t32_key(0) == (aarch32_key_bit | thumb_key_bit));
static_assert(key_isa(0x0000'7fff'ffff'f000U) == GuestIsa::a64);
static_assert(key_isa(a32_key(0x1000)) == GuestIsa::a32);
static_assert(key_isa(t32_key(0x1002)) == GuestIsa::t32);
static_assert(key_pc(a32_key(0xffff'fffcU)) == 0xffff'fffcU);
static_assert(key_pc(t32_key(0xffff'fffeU)) == 0xffff'fffeU);
static_assert(key_from_target(0x1001U) == t32_key(0x1000U));
static_assert(key_from_target(0x1000U) == a32_key(0x1000U));
static_assert(key_from_target(0x1002U) == a32_key(0x1002U));
static_assert(key_target_value(key_from_target(0x1001U)) == 0x1001U);
static_assert(key_target_value(key_from_target(0x1000U)) == 0x1000U);
/* No valid AArch32 key reaches the JIT's translation_key_mode(), the SME
 * mode in the two low bits of an A64 key, and none can be the ~0 of an empty
 * cell or ring record or an A64 fault position (bit 0 set). */
static_assert((a32_key(0xffff'fffcU) & svcr_mask) == 0U);
static_assert((t32_key(0xffff'fffeU) & svcr_mask) == 0U);
static_assert(!key_is_canonical(~std::uint64_t{0}));
static_assert(key_is_canonical(a32_key(0xffff'fffcU)) &&
	key_is_canonical(t32_key(0xffff'fffeU)));
static_assert(!key_is_canonical(a32_key(2U)));
static_assert(key_fetch_address(0x4000'1003U) == 0x4000'1000U);
static_assert(key_fetch_address(a64_tbi_key_bit | 0x4000'1001U) ==
	0x4000'1000U);
static_assert(key_isa(a64_tbi_key_bit | 0x4000'1000U) == GuestIsa::a64);
static_assert(key_fetch_address(a32_key(0x4000'1000U)) == 0x4000'1000U);
static_assert(key_fetch_address(t32_key(0x4000'1002U)) == 0x4000'1002U);

} // namespace uml::cpu::arm64::aarch32
