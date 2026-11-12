// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "program.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace uml::cpu::x86_64::gadgets {

/* What a stream is. A block's own stream is the one its program holds and an
 * executor runs, and the runtime can name any of its instructions from a
 * stream word (`instruction_at_word`); a baked PUSH or POP in it therefore
 * carries no word of its own. A fragment is built for one run from some of a
 * block's instructions, a budget-limited replay or one checked instruction,
 * and its instructions all carry their position. */
enum class StreamKind : std::uint8_t { fragment, block };

/* Append the block's stream to `stream`, which the caller may hand back with
 * its capacity intact so a translation does not buy the storage again.
 * Returns the index of the first gadget the bank has no form for, leaving a
 * partial stream, or nothing when the whole block was appended.
 *
 * `program_begin` is the address of the block the stream runs as, which the
 * run's execution context names as its program_begin whatever part of the
 * block the stream holds: an instruction that can fault or leave carries its
 * position from it in its stream words, and the exits add it back. A stream
 * built from some of a block's instructions, a budget-limited replay or one
 * checked instruction, therefore takes the block's address and not its own
 * first instruction's. An instruction 1024 bytes or more past it has no
 * form, and ends the stream before it like any other the bank refuses. */
[[nodiscard]] std::optional<std::size_t> append_native_stream(
	std::vector<std::uintptr_t> &stream, std::span<const Gadget> gadgets,
	std::uint64_t final_rip, ExecutionMode mode,
	std::uint64_t program_begin, ProgramLinkResolver links = {},
	StreamKind kind = StreamKind::fragment);

/* The instruction of `gadgets` whose words in `stream`, a stream of `kind`
 * built from them, include word `word`, or nothing when the stream is not
 * theirs. */
[[nodiscard]] std::optional<std::size_t> instruction_at_word(
	std::span<const Gadget> gadgets, std::span<const std::uintptr_t> stream,
	std::size_t word, ExecutionMode mode, std::uint64_t program_begin,
	StreamKind kind = StreamKind::fragment);

[[nodiscard]] std::vector<std::uintptr_t> build_native_stream(
	std::span<const Gadget> gadgets, std::uint64_t final_rip,
	ExecutionMode mode, std::uint64_t program_begin,
	ProgramLinkResolver links = {});

/* `build_native_stream` into `stream`, whose storage it keeps, for a caller
 * that builds one stream after another. */
void build_native_stream_into(std::vector<std::uintptr_t> &stream,
	std::span<const Gadget> gadgets, std::uint64_t final_rip,
	ExecutionMode mode, std::uint64_t program_begin,
	ProgramLinkResolver links = {});

/* Whether the instruction is a string operation with a REP, REPE or REPNE
 * prefix. */
[[nodiscard]] bool repeated_string(const Instruction &instruction) noexcept;

/* One iteration of a repeated string instruction, as a single step runs
 * it: the stream leaves between elements with RIP naming the instruction,
 * or retires it when the iteration ends it. The instruction's position is
 * counted from `program_begin`, as in `append_native_stream`. */
[[nodiscard]] std::vector<std::uintptr_t> build_repeat_step_stream(
	const Gadget &gadget, ExecutionMode mode, std::uint64_t program_begin);

/* A stream that runs nothing and leaves with RIP at `rip`: a check head's,
 * whose instruction the runtime runs itself. */
void append_leave_stream(std::vector<std::uintptr_t> &stream,
	std::uint64_t rip);

} // namespace uml::cpu::x86_64::gadgets
