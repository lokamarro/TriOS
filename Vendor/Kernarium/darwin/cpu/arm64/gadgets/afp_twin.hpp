// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <span>

namespace uml::cpu::arm64::gadgets {

/* One row of the FEAT_AFP twin table engine/afp_twin.inc emits: entries
 * [base_begin, base_end) run as the entries at the same offsets from
 * twin_begin wherever the virtual CPU implements FEAT_AFP. A row names one
 * entry or one equal-stride fixed-point table. */
struct AfpTwin final {
	std::uintptr_t base_begin;
	std::uintptr_t base_end;
	std::uintptr_t twin_begin;
	std::uintptr_t twin_end;
};
static_assert(sizeof(AfpTwin) == 32);

[[nodiscard]] std::span<const AfpTwin> afp_twins() noexcept;

/* The entry to emit in place of `entry` where the virtual CPU implements
 * FEAT_AFP: its twin, or `entry` itself when it needs none. */
[[nodiscard]] std::uintptr_t afp_twin(std::uintptr_t entry);

} // namespace uml::cpu::arm64::gadgets
