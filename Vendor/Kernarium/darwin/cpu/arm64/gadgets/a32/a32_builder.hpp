// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>

namespace uml::cpu::arm64::gadgets::a32 {

/* The AArch32 builder of the gadget engine: it translates A32 and T32 guest
 * code, decoded by the shared decoder (cpu/arm64/aarch32/decode.hpp), into
 * the same data-only streams the A64 Builder produces, so one executor, one
 * program cache and one execution boundary run both (executor.cpp).
 *
 * A translation is named by its key (aarch32/guest_key.hpp): the guest
 * address of a block is 1 << 63 | pc for A32 and 1 << 63 | 1 << 62 | pc << 1
 * for T32, and every instruction's key is the block's plus four for each
 * guest unit before it (a word for A32, a halfword for T32). Every PC a
 * stream stores, and every BlockLink it follows, is such a key.
 *
 * The PC of a program as the engine reports it on an exit is the key of the
 * instruction that did not retire, or of the next instruction for one that
 * did (SVC, BKPT excepted: aarch32/semantics.hpp). */

struct BuildInput final {
	/* The key the block starts at. */
	std::uint64_t key{};
	/* The guest bytes from the key's address: 32-bit words for A32 and
	 * 16-bit halfwords for T32, in guest (little-endian) order. A window
	 * that ends inside a T32 instruction stops the block before it. */
	std::span<const std::byte> code;
	/* ITSTATE at the first instruction: zero for any cached block. */
	aarch32::ItState it{};
	/* A single instruction, uncached: no links, and no IT block is
	 * required to complete. */
	bool single{};
};

struct BuildOutput final {
	BuildResult result;
	/* Guest units the translation read, the ones the cache validates it
	 * against: the instructions' and, for a block that stopped at an
	 * undefined instruction, that instruction's too. */
	std::size_t units{};
	/* ITSTATE after the last instruction of a single-instruction build. */
	aarch32::ItState next_it{};
};

/* The most instructions an AArch32 block holds, whatever window the executor
 * fetches. Gadget words that carry an instruction's position in its block may
 * rely on it. */
inline constexpr std::size_t block_limit = 64;

/* `block_instructions` is the block-wide limit on translated instructions,
 * at most block_limit. An A32 block holds that many; a T32 block stops five
 * short of it unless an IT block is open, so that an IT block that starts
 * inside the block always completes there. */
[[nodiscard]] BuildOutput build(const BuildInput &input,
	const Builder::LinkResolver &resolve_link,
	std::pmr::memory_resource *storage,
	const Builder::IndirectLinkResolver &resolve_indirect_link,
	const BuildOptions &options, const aarch32::Profile &profile,
	std::size_t block_instructions);

/* The profile the gadget engine decodes AArch32 against: the optional
 * extensions the host has (a32_features), which are the ones the guest is
 * shown. A form of any other extension is UNDEFINED. */
[[nodiscard]] aarch32::Profile decode_profile(const BuildOptions &options) noexcept;

/* Where each instruction of a T32 block starts, given the block's halfwords:
 * the number of instructions before the halfword `unit`, or nothing when
 * `unit` is not the start of an instruction. */
[[nodiscard]] std::optional<std::size_t> thumb_position(
	std::span<const std::byte> halfwords, std::size_t unit) noexcept;

/* The halfword where the instruction at `position` of a T32 block starts: the
 * inverse of thumb_position. A memory gadget reports the position, and the
 * key of its instruction is the block's key plus four per halfword. */
[[nodiscard]] std::optional<std::size_t> thumb_unit(
	std::span<const std::byte> halfwords, std::size_t position) noexcept;

/* ITSTATE at the instruction starting at `unit` of a T32 block that begins
 * with ITSTATE = 0, found by decoding the block's halfwords again. */
[[nodiscard]] std::uint8_t thumb_itstate(std::span<const std::byte> halfwords,
	std::size_t unit, const aarch32::Profile &profile) noexcept;

/* Whether the engine leaves a form to the AArch32 semantic library
 * (library_helpers.cpp): the rare forms it does not lower, and, for the
 * block transfers (LDM, STM, PUSH, POP, VLDM, VSTM) that cross a page and an
 * STM that stores the PC, the instruction that does. A form that does not may
 * never reach the library; the library-exit count gates hold the engine to
 * that. */
[[nodiscard]] bool routes_to_library(aarch32::Form form) noexcept;
[[nodiscard]] bool may_reach_library(aarch32::Form form) noexcept;

} // namespace uml::cpu::arm64::gadgets::a32
