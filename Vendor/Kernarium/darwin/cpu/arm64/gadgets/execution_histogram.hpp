// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

/* GadgetExecutor's layout follows this switch, so a translation unit that
 * guessed it would disagree with the library about that layout. */
#ifndef UML_GADGET_EXECUTION_HISTOGRAM
#error "UML_GADGET_EXECUTION_HISTOGRAM must come from the build"
#endif

/*
 * Execution-weighted histogram of gadget entries (diagnostic builds only).
 *
 * A threaded counter gadget counts entered decode groups by the exact gadget
 * entry the translator selected, including forwarding variants, so a static
 * host-instruction cost per entry can be weighted by how often the entry
 * actually runs. Slot zero accumulates guest words in entered groups, slot one counts groups whose
 * entry could not be indexed, and slot 2 + i counts executions of the entry
 * with dense index i, where an index names one (predecessor, entry) pair so
 * that executed adjacent pairs are exact as well. Counting is per carrier and
 * merged at dump time.
 *
 * Set DARWIN_UML_GADGET_EXECUTION_HISTOGRAM to a path to have the merged
 * counts rewritten there periodically and at exit, most frequent first.
 */
namespace uml::cpu::arm64::gadgets::diagnostics {

inline constexpr std::size_t execution_histogram_capacity = std::size_t{1} << 18;

#if UML_GADGET_EXECUTION_HISTOGRAM
[[nodiscard]] void *execution_counters();
/* Byte offset of the counter slot for one adjacent entry pair. A null
 * predecessor names a block entry, so per-entry totals are pair sums. */
[[nodiscard]] std::size_t execution_histogram_slot(
	const void *previous, const void *gadget);
/* An executor's diagnostics block, summed into the dump. The carrier that
 * owns the block notes it after each block run and the histogram copies it
 * from time to time on that carrier, so the dump never reads a block another
 * thread is writing; `forget` hands over the final counts when the executor
 * is destroyed, after which the histogram holds no pointer to it. */
void execution_histogram_note_executor(const void *diagnostics);
void execution_histogram_forget_executor(const void *diagnostics);
extern "C" void uml_arm64_gadget_execution_count();
#endif

} // namespace uml::cpu::arm64::gadgets::diagnostics
