// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace uml::platform {

/* The smallest budget the host GPU is given: 384 MiB, which every application
 * of the launch set fits in. */
inline constexpr std::uint64_t gpu_budget_floor_bytes = 384ULL << 20;

/* What the fixed GPU budget derives from, all in bytes. A, E and S are the
 * launch parameters (runtime/guest_memory_budget.hpp states the sets a launch
 * takes them from); a caller that states none of them leaves only the guest's
 * own memory and the gadget cache in the sum. */
struct GpuBudgetInputs final {
	/* L: the memory the host lets the process reach before it ends it
	 * (process_memory_limit_bytes), zero for none. */
	std::uint64_t process_limit{};
	/* G: the guest's RAM. */
	std::uint64_t guest_memory{};
	/* A: the application's own expected ceiling. */
	std::uint64_t application{};
	/* E: the other memory the guest side of the process uses at its
	 * ceiling, the gadget cache in effect included. */
	std::uint64_t engine{};
	/* S: the headroom left unused. */
	std::uint64_t headroom{};
};

struct GpuBudget final {
	/* B: what the host GPU may hold for the guest, in bytes. */
	std::uint64_t bytes{};
	/* L - A - G - E - S, zero when the sum passes L, and when L is not known
	 * the guest's maximum. */
	std::uint64_t available{};
	/* Whether L is known and L - A - G - E - S is below the floor: the sum
	 * of what the process uses leaves the GPU less than it needs, so the
	 * process is likely to be ended. The budget is the floor (or half the
	 * guest's RAM when that is less); the launch goes ahead. */
	bool short_of_floor{};
};

/* The host GPU's budget, evaluated once at launch:
 *
 *     B = clamp(L - A - G - E - S, 384 MiB, G / 2)
 *
 * Half the guest's RAM is the most the GPU may take, so where that is under
 * the floor it wins and B is G / 2. Without a limit the budget is G / 2 and
 * nothing is short. The sum saturates, so no input overflows it. */
[[nodiscard]] constexpr GpuBudget compute_gpu_budget(
	const GpuBudgetInputs &inputs) noexcept
{
	const std::uint64_t maximum = inputs.guest_memory / 2U;
	if (inputs.process_limit == 0)
		return {.bytes = maximum, .available = maximum,
			.short_of_floor = false};

	constexpr std::uint64_t unlimited =
		std::numeric_limits<std::uint64_t>::max();
	const auto sum = [](std::uint64_t first, std::uint64_t second) {
		return first > unlimited - second ? unlimited : first + second;
	};
	const std::uint64_t committed = sum(sum(inputs.application,
							inputs.guest_memory),
		sum(inputs.engine, inputs.headroom));
	const std::uint64_t available = inputs.process_limit > committed ?
		inputs.process_limit - committed : 0;
	return {
		.bytes = std::min(std::max(available, gpu_budget_floor_bytes),
			maximum),
		.available = available,
		.short_of_floor = available < gpu_budget_floor_bytes,
	};
}

} // namespace uml::platform
