// SPDX-License-Identifier: GPL-2.0-or-later
#include "require.hpp"

#include <uml/tlb_sync_ranges.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <vector>

namespace {

using Range = uml_tlb_sync_range;

struct RangeSet final {
	std::array<Range, UML_TLB_SYNC_MAX_RANGES> ranges{};
	unsigned int count{};

	void add(unsigned long from, unsigned long to)
	{
		count = uml_tlb_sync_ranges_add(
			ranges.data(), count, from, to);
	}
};

using uml::testing::require;

void require_range(const RangeSet &set, std::size_t index,
		   unsigned long from, unsigned long to)
{
	require(index < set.count);
	require(set.ranges[index].from == from);
	require(set.ranges[index].to == to);
}

void test_sorted_insert_and_multi_range_merge()
{
	RangeSet set;

	set.add(0x4000, 0x5000);
	set.add(0x1000, 0x2000);
	set.add(0x7000, 0x8000);
	require(set.count == 3);
	require_range(set, 0, 0x1000, 0x2000);
	require_range(set, 1, 0x4000, 0x5000);
	require_range(set, 2, 0x7000, 0x8000);

	set.add(0x2000, 0x4000);
	require(set.count == 2);
	require_range(set, 0, 0x1000, 0x5000);
	require_range(set, 1, 0x7000, 0x8000);

	set.add(0x5000, 0x7000);
	require(set.count == 1);
	require_range(set, 0, 0x1000, 0x8000);

	set.add(0x3000, 0x4000);
	require(set.count == 1);
	require_range(set, 0, 0x1000, 0x8000);
}

RangeSet full_set()
{
	RangeSet set;

	for (unsigned long index = 0;
	     index < UML_TLB_SYNC_MAX_RANGES; ++index)
		set.add(index * 100 + 20, index * 100 + 30);
	require(set.count == UML_TLB_SYNC_MAX_RANGES);
	return set;
}

void test_bounded_overflow_uses_nearest_gap()
{
	{
		auto set = full_set();
		set.add(0, 10);
		require(set.count == UML_TLB_SYNC_MAX_RANGES);
		require_range(set, 0, 0, 30);
	}
	{
		auto set = full_set();
		set.add(1600, 1610);
		require(set.count == UML_TLB_SYNC_MAX_RANGES);
		require_range(set, UML_TLB_SYNC_MAX_RANGES - 1, 1520, 1610);
	}
	{
		auto set = full_set();
		set.add(735, 740);
		require_range(set, 7, 720, 740);
		require_range(set, 8, 820, 830);
	}
	{
		auto set = full_set();
		set.add(810, 815);
		require_range(set, 7, 720, 730);
		require_range(set, 8, 810, 830);
	}
	{
		auto set = full_set();
		set.add(774, 776);
		require_range(set, 7, 720, 776);
		require_range(set, 8, 820, 830);
	}
}

void test_randomized_coverage_and_invariants()
{
	RangeSet set;
	std::vector<Range> requested;
	unsigned long state = 0x9e37'79b9UL;

	for (unsigned int iteration = 0; iteration < 4000; ++iteration) {
		state = state * 1664525UL + 1013904223UL;
		const auto from = (state >> 8) & 0xffffUL;
		state = state * 1664525UL + 1013904223UL;
		const auto to = from + 1 + ((state >> 16) & 0xffUL);
		requested.push_back({from, to});
		set.add(from, to);

		require(set.count > 0);
		require(set.count <= UML_TLB_SYNC_MAX_RANGES);
		for (unsigned int index = 0; index < set.count; ++index) {
			require(set.ranges[index].from < set.ranges[index].to);
			if (index != 0)
				require(set.ranges[index - 1].to <
					set.ranges[index].from);
		}
	}

	for (const auto &range : requested) {
		bool covered = false;

		for (unsigned int index = 0; index < set.count; ++index) {
			if (set.ranges[index].from <= range.from &&
			    set.ranges[index].to >= range.to) {
				covered = true;
				break;
			}
		}
		require(covered);
	}
}

} // namespace

int main()
{
	test_sorted_insert_and_multi_range_merge();
	test_bounded_overflow_uses_nearest_gap();
	test_randomized_coverage_and_invariants();
	std::puts("TLB sync range tests passed");
	return 0;
}
