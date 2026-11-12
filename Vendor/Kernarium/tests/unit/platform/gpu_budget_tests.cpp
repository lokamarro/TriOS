// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_budget.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <cstdint>
#include <limits>

namespace {

using uml::testing::require;
namespace platform = uml::platform;

constexpr std::uint64_t mib = 1024ULL * 1024ULL;

platform::GpuBudget budgetOf(std::uint64_t limit, std::uint64_t guest,
	std::uint64_t application = 0, std::uint64_t engine = 0,
	std::uint64_t headroom = 0)
{
	return platform::compute_gpu_budget({
		.process_limit = limit * mib,
		.guest_memory = guest * mib,
		.application = application * mib,
		.engine = engine * mib,
		.headroom = headroom * mib,
	});
}

/* L - A - G - E - S between the floor and half the guest's RAM is the
 * budget itself. */
void theRemainderIsTheBudgetBetweenTheBounds()
{
	/* iPad mini 6: 2867 MiB, a 1536 MiB guest, a 358 MiB gadget cache. */
	const auto ipad = budgetOf(2867, 1536, 0, 358, 0);
	require(ipad.available == 973 * mib);
	require(ipad.bytes == 768 * mib, "half the guest is the most");
	require(!ipad.short_of_floor);

	const auto tight = budgetOf(2867, 1536, 300, 358, 400);
	require(tight.available == 273 * mib);
	require(tight.bytes == 384 * mib, "the floor is the least");
	require(tight.short_of_floor);

	const auto middle = budgetOf(2867, 1536, 300, 358, 100);
	require(middle.available == 573 * mib);
	require(middle.bytes == 573 * mib, "the remainder in between");
	require(!middle.short_of_floor);

	require(budgetOf(2867, 1536, 0, 358, 573).bytes == 400 * mib);
	require(budgetOf(2867, 1536, 0, 358, 589).bytes == 384 * mib);
	require(!budgetOf(2867, 1536, 0, 358, 589).short_of_floor,
		"exactly the floor is not short of it");
	require(budgetOf(2867, 1536, 0, 358, 590).short_of_floor);
}

/* Where half the guest's RAM is under the floor, the maximum wins. */
void halfTheGuestWinsOverTheFloor()
{
	const auto small = budgetOf(2867, 512);
	require(small.bytes == 256 * mib);
	require(!small.short_of_floor);
	require(budgetOf(2867, 704).bytes == 352 * mib);
	require(budgetOf(2867, 768).bytes == 384 * mib);
	/* The warning follows the sum alone, the budget follows the clamp. */
	const auto starved = budgetOf(1024, 512, 0, 400, 0);
	require(starved.available == 112 * mib);
	require(starved.short_of_floor);
	require(starved.bytes == 256 * mib);
}

/* Without a limit nothing is known to be short, and the guest's half is the
 * budget, whatever A, E and S state. */
void noLimitIsHalfTheGuest()
{
	const auto none = budgetOf(0, 2048, 100, 500, 200);
	require(none.bytes == 1024 * mib);
	require(!none.short_of_floor);
	require(budgetOf(0, 0).bytes == 0);
}

/* A, E and S at zero leave the limit, the guest and the cache in the sum,
 * except where the limit leaves the guest less than half of its RAM. */
void zeroParametersLeaveOnlyTheGuestAndTheCache()
{
	require(budgetOf(4096, 2048).bytes == 1024 * mib);
	require(budgetOf(2867, 2048).bytes == 819 * mib,
		"L - G is under G / 2");
	require(budgetOf(2867, 2048).available == 819 * mib);
	require(budgetOf(2867, 4096).bytes == 384 * mib,
		"the guest alone passes the limit");
	require(budgetOf(2867, 4096).short_of_floor);
	require(budgetOf(2867, 4096).available == 0);
}

/* The conservative launch set (runtime/guest_memory_budget.hpp) on an iPad
 * mini 6: 64 MiB for the application, 320 for the rest of the engine and 149
 * of headroom, with the default guest of 1536 MiB and six vCPUs' cache (240
 * MiB) and slot tags (24 MiB). The GPU gets what is left, 534 MiB, well under
 * the half of the guest that caps it at 768 MiB with no terms counted. */
void theConservativeSumLeavesTheGPUWhatIsLeft()
{
	const auto conservative = budgetOf(2867, 1536, 64, 240 + 24 + 320, 149);
	require(conservative.available == 534 * mib);
	require(conservative.bytes == 534 * mib);
	require(!conservative.short_of_floor);
	require(budgetOf(2867, 1536, 0, 240 + 24, 0).bytes == 768 * mib);
	/* A guest of 2048 MiB with the same terms is short of the floor. */
	const auto above = budgetOf(2867, 2048, 64, 240 + 24 + 320, 149);
	require(above.available == 22 * mib);
	require(above.bytes == 384 * mib);
	require(above.short_of_floor);
}

void theSumSaturates()
{
	constexpr auto top = std::numeric_limits<std::uint64_t>::max();
	const auto huge = platform::compute_gpu_budget({
		.process_limit = top,
		.guest_memory = 2048 * mib,
		.application = top,
		.engine = top,
		.headroom = top,
	});
	require(huge.available == 0);
	require(huge.bytes == 384 * mib);
	require(huge.short_of_floor);
}

} // namespace

int main()
{
	static constexpr uml::testing::NamedTest tests[] {
		{"the-remainder-is-the-budget-between-the-bounds",
			theRemainderIsTheBudgetBetweenTheBounds},
		{"half-the-guest-wins-over-the-floor", halfTheGuestWinsOverTheFloor},
		{"no-limit-is-half-the-guest", noLimitIsHalfTheGuest},
		{"zero-parameters-leave-only-the-guest-and-the-cache",
			zeroParametersLeaveOnlyTheGuestAndTheCache},
		{"the-conservative-sum-leaves-the-gpu-what-is-left",
			theConservativeSumLeavesTheGPUWhatIsLeft},
		{"the-sum-saturates", theSumSaturates},
	};
	return uml::testing::runNamedTests(tests);
}
