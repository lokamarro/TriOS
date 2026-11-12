// SPDX-License-Identifier: GPL-2.0-or-later
//
// The x86-64 gadget cache's set index spreads clustered guest code across the
// sets. Code is not random: function entries sit at alignments, a table's rows
// repeat page after page, a library takes a 64 KiB slot, and one binary runs
// in several address spaces and both execution modes. A set index that maps
// such a stream onto a few sets turns most of the cache into conflict
// evictions while other sets stay empty, so each stream here must load the
// sets as a uniform spread would, where a set that holds more than its ways
// is a translation evicted for a conflict.

#include "cpu/x86_64/gadgets/program_cache_index.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {

using ::uml::cpu::x86_64::ExecutionMode;
using ::uml::cpu::x86_64::gadgets::program_cache_set;

constexpr std::size_t ways = 8;
constexpr std::uint64_t code_base = 0x40'0000;

static_assert(program_cache_set(1, code_base, ExecutionMode::long64, 32768) <
	32768);
static_assert(program_cache_set(1, code_base, ExecutionMode::long64, 128) <
	128);
static_assert(program_cache_set(1, code_base, ExecutionMode::long64, 32768) !=
	program_cache_set(1, code_base, ExecutionMode::compat32, 32768));
static_assert(program_cache_set(1, code_base, ExecutionMode::long64, 32768) !=
	program_cache_set(2, code_base, ExecutionMode::long64, 32768));

struct Key final {
	std::uint64_t generation;
	std::uint64_t address;
	ExecutionMode mode;
};

/* The index this one replaced, which two xor folds of the key made. It stays
 * only to show each clustered stream below is one it loaded unevenly, so a
 * stream that stops being clustered fails here instead of passing for any
 * index. */
[[nodiscard]] std::size_t legacy_set(const Key &key, std::size_t sets) noexcept
{
	const auto block = key.address ^
		(key.generation * 0x9e37'79b9'7f4a'7c15ULL) ^
		(static_cast<std::uint64_t>(key.mode) << 32U);
	const auto mixed = block ^ (block >> 12U) ^ (block >> 24U);
	return static_cast<std::size_t>(mixed) & (sets - 1U);
}

struct Load final {
	std::size_t overflow;
	std::size_t occupied;
	std::size_t maximum;
};

[[nodiscard]] Load load_of(const std::vector<Key> &keys, std::size_t sets,
	const std::function<std::size_t(const Key &)> &index)
{
	std::vector<std::uint32_t> counts(sets);
	for (const auto &key : keys) {
		const auto set = index(key);
		if (set >= sets) {
			std::fprintf(stderr, "set %zu beyond %zu\n", set, sets);
			std::exit(1);
		}
		++counts[set];
	}
	Load load{};
	for (const auto count : counts) {
		if (count > ways)
			load.overflow += count - ways;
		if (count != 0)
			++load.occupied;
		load.maximum = std::max<std::size_t>(load.maximum, count);
	}
	return load;
}

/* What `keys` keys thrown uniformly into `sets` sets would leave: the keys
 * beyond the ways of a set, and the sets that hold any. */
struct Expected final {
	double overflow;
	double occupied;
};

[[nodiscard]] Expected expected_of(std::size_t keys, std::size_t sets)
{
	const auto lambda = static_cast<double>(keys) / static_cast<double>(sets);
	Expected expected{};
	for (std::size_t count = ways + 1; count < 200; ++count)
		expected.overflow += static_cast<double>(count - ways) *
			std::exp(-lambda + static_cast<double>(count) * std::log(lambda) -
				std::lgamma(static_cast<double>(count) + 1.0));
	expected.overflow *= static_cast<double>(sets);
	expected.occupied = static_cast<double>(sets) * (1.0 - std::exp(-lambda));
	return expected;
}

struct Stream final {
	std::string name;
	std::vector<Key> keys;
	/* A stream the two xor folds loaded unevenly. A dense run they loaded
	 * evenly stays in as the case any index must pass. */
	bool legacy_uneven;
};

/* `entries` per base, in 16-byte steps: the entry points of a function table
 * or a run of identical stubs. */
void add_entries(std::vector<Key> &keys, std::uint64_t generation,
	ExecutionMode mode, std::uint64_t stride, std::size_t bases,
	std::size_t entries)
{
	for (std::size_t base = 0; base < bases; ++base)
		for (std::size_t entry = 0; entry < entries; ++entry)
			keys.push_back({generation,
				code_base + base * stride + entry * 16U, mode});
}

[[nodiscard]] std::vector<Stream> streams_for(std::size_t sets)
{
	/* Two keys a set on average, the occupancy of a cache a workload leaves
	 * a third full. */
	const auto count = sets * 2U;
	std::vector<Stream> streams;

	{
		Stream stream{"dense run of byte-granular RIPs", {}, false};
		for (std::size_t i = 0; i < count; ++i)
			stream.keys.push_back(
				{1, code_base + i, ExecutionMode::long64});
		streams.push_back(std::move(stream));
	}
	for (const std::uint64_t stride :
		{std::uint64_t{0x1000}, std::uint64_t{0x1'0000},
			std::uint64_t{0x100'0000}}) {
		char name[96];
		std::snprintf(name, sizeof name,
			"16 entries per base, bases %#llx apart",
			static_cast<unsigned long long>(stride));
		Stream stream{name, {}, true};
		add_entries(stream.keys, 1, ExecutionMode::long64, stride,
			count / 16U, 16);
		streams.push_back(std::move(stream));
	}
	{
		Stream stream{"64 dense windows, 64 KiB apart", {}, true};
		for (std::size_t window = 0; window < 64; ++window)
			for (std::size_t i = 0; i < count / 64U; ++i)
				stream.keys.push_back({1,
					code_base + window * 0x1'0000U + i,
					ExecutionMode::long64});
		streams.push_back(std::move(stream));
	}
	{
		Stream stream{"entries 64 KiB apart, 8 generations, both modes", {},
			true};
		for (std::uint64_t generation = 1; generation <= 8; ++generation)
			for (const auto mode :
				{ExecutionMode::long64, ExecutionMode::compat32})
				add_entries(stream.keys, generation, mode, 0x1'0000,
					count / (16U * 16U), 16);
		streams.push_back(std::move(stream));
	}
	{
		Stream stream{"dense window, 8 generations, both modes", {}, false};
		for (std::uint64_t generation = 1; generation <= 8; ++generation)
			for (const auto mode :
				{ExecutionMode::long64, ExecutionMode::compat32})
				for (std::size_t i = 0; i < count / 16U; ++i)
					stream.keys.push_back(
						{generation, code_base + i, mode});
		streams.push_back(std::move(stream));
	}
	return streams;
}

} // namespace

int main()
{
	bool ok = true;
	/* 16384 sets are 131,072 slots and 32768 are 262,144; 65536 brackets
	 * them above. */
	for (const std::size_t sets : {std::size_t{16384}, std::size_t{32768},
		std::size_t{65536}}) {
		for (const auto &stream : streams_for(sets)) {
			const auto expected = expected_of(stream.keys.size(), sets);
			/* Overflow within twice a uniform spread's, with room for the
			 * spread's own variance, and no collapse onto a fraction of
			 * the sets. */
			const auto overflow_limit = 2.0 * expected.overflow + 64.0;
			const auto occupied_floor = 0.25 * expected.occupied;
			const auto production = load_of(stream.keys, sets,
				[sets](const Key &key) {
					return program_cache_set(key.generation, key.address,
						key.mode, sets);
				});
			const auto legacy = load_of(stream.keys, sets,
				[sets](const Key &key) { return legacy_set(key, sets); });
			const bool spread =
				static_cast<double>(production.overflow) <= overflow_limit &&
				static_cast<double>(production.occupied) >= occupied_floor;
			/* The stream earns its place only if the old index failed it
			 * the same way. */
			const bool discriminates = !stream.legacy_uneven ||
				static_cast<double>(legacy.overflow) > 4.0 * overflow_limit ||
				static_cast<double>(legacy.occupied) < 0.5 * occupied_floor;
			if (!spread || !discriminates) {
				ok = false;
				std::fprintf(stderr,
					"%s: sets %zu keys %zu: overflow %zu (limit %.0f, "
					"uniform %.1f), occupied %zu (floor %.0f, uniform %.0f), "
					"most in a set %zu%s; old index overflow %zu occupied "
					"%zu\n",
					stream.name.c_str(), sets, stream.keys.size(),
					production.overflow, overflow_limit, expected.overflow,
					production.occupied, occupied_floor, expected.occupied,
					production.maximum,
					spread ? "" : " -- NOT SPREAD",
					legacy.overflow, legacy.occupied);
				if (!discriminates)
					std::fprintf(stderr,
						"  (this stream no longer loads the old index "
						"unevenly)\n");
			}
		}
	}
	if (ok)
		std::puts("x86-64 gadget cache set index: ok");
	return ok ? 0 : 1;
}
