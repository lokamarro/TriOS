// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "address_space_types.hpp"

#include <cstdint>
#include <cstddef>
#include <memory>

namespace uml::memory {

[[nodiscard]] bool execution_reader_begin(
	GenerationState &state, std::uint64_t generation,
	std::uint64_t guest_pc) noexcept;
void execution_reader_end(GenerationState &state) noexcept;
[[nodiscard]] bool execution_context_publish_code_change(
	void *context, std::uint64_t address) noexcept;
/* The ranges an entered context's address space changed after `sequence`, or
 * -1 when the caller has to retire every translation it holds. */
[[nodiscard]] int execution_context_code_changes(void *context,
	std::uint64_t sequence, CodeChangeRange *output, std::size_t capacity,
	std::uint64_t *current_sequence) noexcept;

class ExecutionQuiesce final {
public:
	ExecutionQuiesce(std::shared_ptr<GenerationState> state,
		std::uint64_t generation);
	ExecutionQuiesce(const ExecutionQuiesce &) = delete;
	ExecutionQuiesce &operator=(const ExecutionQuiesce &) = delete;
	~ExecutionQuiesce();

private:
	std::shared_ptr<GenerationState> state_;
};

} // namespace uml::memory
