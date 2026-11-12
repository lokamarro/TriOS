// SPDX-License-Identifier: GPL-2.0-or-later
#include "execution_gate.hpp"

#include <atomic>
#include <utility>

namespace uml::memory {
namespace {

constexpr std::uint64_t execution_gate_closed = std::uint64_t{1} << 63;
constexpr std::uint64_t execution_reader_mask = ~execution_gate_closed;

} // namespace

bool execution_reader_begin(GenerationState &state, std::uint64_t generation,
	std::uint64_t guest_pc) noexcept
{
	auto value = state.execution_gate.load(std::memory_order_acquire);
	for (;;) {
		if (state.retired.load(std::memory_order_acquire))
			return false;
		while ((value & execution_gate_closed) != 0) {
			state.execution_gate.wait(value, std::memory_order_relaxed);
			value = state.execution_gate.load(std::memory_order_acquire);
			if (state.retired.load(std::memory_order_acquire))
				return false;
		}
		if ((value & execution_reader_mask) == execution_reader_mask)
			return false;
		if (state.execution_gate.compare_exchange_weak(
				value, value + 1, std::memory_order_acquire,
				std::memory_order_relaxed)) {
			(void)generation;
			(void)guest_pc;
			return true;
		}
	}
}

void execution_reader_end(GenerationState &state) noexcept
{
	const auto previous = state.execution_gate.fetch_sub(
		1, std::memory_order_release);
	if ((previous & execution_gate_closed) != 0 &&
	    (previous & execution_reader_mask) == 1)
		state.execution_gate.notify_all();
}

bool execution_context_publish_code_change(
	void *opaque, std::uint64_t address) noexcept
{
	auto *context = static_cast<ExecutionContext *>(opaque);
	if (context == nullptr || !context->entered || !context->state ||
	    context->state->retired.load(std::memory_order_acquire))
		return false;
	/* The entered execution context owns the generation state for the entire
	 * translated run, so publication needs no domain lookup or mapping lock. */
	publish_code_change(*context->state,
		address & ~std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE - 1U},
		DARWIN_UML_GUEST_PAGE_SIZE);
	return true;
}

int execution_context_code_changes(void *opaque, std::uint64_t sequence,
	CodeChangeRange *output, std::size_t capacity,
	std::uint64_t *current_sequence) noexcept
{
	auto *context = static_cast<ExecutionContext *>(opaque);
	if (context == nullptr || !context->state || current_sequence == nullptr ||
	    (capacity != 0 && output == nullptr) ||
	    context->state->retired.load(std::memory_order_acquire))
		return -1;
	return code_changes_since(*context->state, sequence, output, capacity,
		current_sequence);
}

ExecutionQuiesce::ExecutionQuiesce(std::shared_ptr<GenerationState> state,
	std::uint64_t generation)
	: state_{std::move(state)}
{
	(void)generation;
	auto value = state_->execution_gate.load(std::memory_order_acquire);
	for (;;) {
		if ((value & execution_gate_closed) != 0) {
			state_->execution_gate.wait(value, std::memory_order_relaxed);
			value = state_->execution_gate.load(std::memory_order_acquire);
			continue;
		}
		if (state_->execution_gate.compare_exchange_weak(
				value, value | execution_gate_closed,
				std::memory_order_acq_rel,
				std::memory_order_relaxed))
			break;
	}
	state_->execution_interrupts.fetch_add(1, std::memory_order_release);
	value = state_->execution_gate.load(std::memory_order_acquire);
	while ((value & execution_reader_mask) != 0) {
		state_->execution_gate.wait(value, std::memory_order_relaxed);
		value = state_->execution_gate.load(std::memory_order_acquire);
	}
}

ExecutionQuiesce::~ExecutionQuiesce()
{
	state_->execution_gate.store(0, std::memory_order_release);
	state_->execution_gate.notify_all();
}

} // namespace uml::memory
