/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The code-change sequence an address space publishes and the ranges it
 * remembers, as the MMU does, for the tests that bind an execution context
 * of their own to an executor. */
#pragma once

#include "cpu/x86_64/gadgets/executor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::test::x86_64_code_change_history {

struct Sequences final {
	std::uint64_t mapping_changes{1};
	std::uint64_t code_changes{1};
	std::array<uml::cpu::x86_64::gadgets::ProgramCodeChange, 16> history{};
	std::size_t recorded{};

	void change(std::uint64_t address, std::uint64_t length) noexcept
	{
		if (recorded < history.size())
			history[recorded] = {address, length};
		++recorded;
		++code_changes;
	}
};

[[nodiscard]] inline uml::cpu::x86_64::gadgets::ProgramCacheSequences
read_sequences(void *context) noexcept
{
	const auto &state = *static_cast<const Sequences *>(context);
	return {state.mapping_changes, state.code_changes, false};
}

/* Stands in for the address space's own history: the ranges published after
 * `sequence`, or a refusal when it cannot describe the whole span. */
[[nodiscard]] inline int read_code_changes(void *context,
	std::uint64_t sequence,
	uml::cpu::x86_64::gadgets::ProgramCodeChange *changes,
	std::size_t capacity, std::uint64_t *current_sequence) noexcept
{
	const auto &state = *static_cast<const Sequences *>(context);
	*current_sequence = state.code_changes;
	if (sequence >= state.code_changes)
		return 0;
	if (state.recorded > state.history.size() ||
	    sequence + state.recorded < state.code_changes)
		return -1;
	const auto count = state.code_changes - sequence;
	if (count > capacity)
		return -1;
	for (std::uint64_t index = 0; index < count; ++index)
		changes[index] = state.history[static_cast<std::size_t>(
			state.recorded - count + index)];
	return static_cast<int>(count);
}

} // namespace uml::test::x86_64_code_change_history
