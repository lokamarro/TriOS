// SPDX-License-Identifier: GPL-2.0-or-later
/* The tagged translation key every cache of both engines is indexed by, and
 * the key-aware validation of a translation's saved guest units. Keys: that
 * A64, A32 and T32 never collide with one another, with the ~0 of an empty
 * cell or with a fault position, that interworking selects the state, and
 * that nothing a cache hashes by loses the instruction address. Validation:
 * that a word and a halfword span match, mismatch and fault in the right
 * order across page boundaries of the guest page. Fetch: that a window is
 * read one page-bounded run at a time and ends at the first run that cannot
 * be read. */
#include "cpu/arm64/aarch32/guest_fetch.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/guest_validation.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <unordered_set>
#include <vector>

namespace {

using ::uml::testing::require;
namespace aarch32 = ::uml::cpu::arm64::aarch32;
using aarch32::GuestIsa;
using aarch32::GuestValidation;

/* The guest page every caller passes the validation. */
constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;

/* A small deterministic generator, so a failure repeats. */
class Lcg final {
public:
	[[nodiscard]] std::uint64_t next() noexcept
	{
		state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
		return state_ >> 11 ^ state_ << 13;
	}

private:
	std::uint64_t state_{0x32a3'2a32'9e37'79b9ULL};
};

[[nodiscard]] std::vector<std::uint32_t> sample_pcs()
{
	std::vector<std::uint32_t> pcs{0, 2, 4, 6, 0x1000, 0x1002, 0x1ffc,
		0x1ffe, 0x3ffe, 0x4000, 0x7fff'fffc, 0x8000'0000U,
		0x8000'0002U, 0xfffe'fffcU, 0xffff'fff8U, 0xffff'fffcU,
		0xffff'fffeU};
	Lcg random;
	for (unsigned count = 0; count < 4096; ++count)
		pcs.push_back(static_cast<std::uint32_t>(random.next()));
	return pcs;
}

void keys_round_trip()
{
	for (const auto raw : sample_pcs()) {
		const auto word_pc = raw & ~3U;
		const auto half_pc = raw & ~1U;

		const auto a = aarch32::a32_key(word_pc);
		require(aarch32::key_is_aarch32(a) && !aarch32::key_is_thumb(a));
		require(aarch32::key_isa(a) == GuestIsa::a32);
		require(aarch32::key_pc(a) == word_pc);
		require(aarch32::key_target_value(a) == word_pc);
		require(aarch32::key_is_canonical(a));
		require(aarch32::key_from_target(word_pc) == a);

		const auto t = aarch32::t32_key(half_pc);
		require(aarch32::key_is_aarch32(t) && aarch32::key_is_thumb(t));
		require(aarch32::key_isa(t) == GuestIsa::t32);
		require(aarch32::key_pc(t) == half_pc);
		require(aarch32::key_target_value(t) == (half_pc | 1U));
		require(aarch32::key_is_canonical(t));
		require(aarch32::key_from_target(half_pc | 1U) == t);

		/* BXWritePC: the bit selects the state and is not the address. */
		require(aarch32::key_pc(aarch32::key_from_target(raw)) ==
			((raw & 1U) != 0U ? raw & ~1U : raw));
		require(aarch32::key_is_thumb(aarch32::key_from_target(raw)) ==
			((raw & 1U) != 0U));
	}
}

/* An AArch32 key is not an A64 key, whatever the SME mode bits say, and a
 * valid one has both low bits clear: the JIT's translation_key_mode() is 0
 * for it, so the PC-alignment test every entry passes accepts it. */
void keys_never_collide()
{
	constexpr auto mode_mask = std::uint64_t{::uml::cpu::arm64::svcr_mask};
	std::unordered_set<std::uint64_t> a64_keys;
	std::unordered_set<std::uint64_t> aarch32_keys;
	Lcg random;
	for (unsigned count = 0; count < 4096; ++count) {
		const auto pc48 = random.next() & 0x0000'ffff'ffff'fffcULL;
		const auto raw = static_cast<std::uint32_t>(random.next());
		for (std::uint64_t mode = 0; mode <= mode_mask; ++mode) {
			require(!aarch32::key_is_aarch32(pc48 | mode));
			require(aarch32::key_isa(pc48 | mode) == GuestIsa::a64);
			a64_keys.insert(pc48 | mode);
		}
		const auto a = aarch32::a32_key(raw & ~3U);
		const auto t = aarch32::t32_key(raw & ~1U);
		require((a & mode_mask) == 0 && (t & mode_mask) == 0,
			"an AArch32 key carries SME mode bits");
		require((a & 1U) == 0 && (t & 1U) == 0,
			"an AArch32 key can be a fault position");
		require(a != t);
		aarch32_keys.insert(a);
		aarch32_keys.insert(t);
	}
	for (const auto key : aarch32_keys)
		require(!a64_keys.contains(key),
			"an AArch32 key equals an A64 key");
	for (const auto raw : sample_pcs()) {
		require(aarch32::a32_key(raw & ~3U) != aarch32::t32_key(raw & ~1U));
		require(aarch32::a32_key(raw & ~3U) != (raw & ~3U),
			"an A32 key equals the A64 key of its address");
		require(aarch32::t32_key(raw & ~1U) != (raw & ~1U));
	}
	/* The empty cell and a fault position are never a key. */
	constexpr auto empty = std::numeric_limits<std::uint64_t>::max();
	require(!aarch32::key_is_canonical(empty));
	require(aarch32::key_isa(empty) == GuestIsa::t32 &&
		!aarch32::key_is_canonical(empty));
	for (const auto raw : sample_pcs()) {
		require(!aarch32::key_is_canonical(aarch32::a32_key(raw & ~3U) | 1U));
		require(!aarch32::key_is_canonical(aarch32::t32_key(raw & ~1U) | 1U));
		require(!aarch32::key_is_canonical(aarch32::a32_key(raw & ~3U) | 2U));
	}
	/* A canonical key has nothing outside its address. */
	require(!aarch32::key_is_canonical(aarch32::aarch32_key_bit |
		(std::uint64_t{1} << 40)));
	require(!aarch32::key_is_canonical(aarch32::aarch32_key_bit |
		aarch32::thumb_key_bit | (std::uint64_t{1} << 40)));
	require(!aarch32::key_is_canonical(aarch32::aarch32_key_bit |
		(std::uint64_t{1} << 32)));
}

/* An A32 target with bit 1 set is a misaligned PC: its key is not canonical,
 * so no cache stores it, and the PC-alignment exit comes first. */
void a32_target_with_bit_one_is_never_a_key()
{
	for (const auto value : {0x1002U, 0x2'0006U, 0x8000'0002U, 0xffff'fffeU,
		     0x1001U, 0x1003U, 0xffff'ffffU}) {
		const auto key = aarch32::key_from_target(value);
		if ((value & 1U) == 0U) {
			require(!aarch32::key_is_thumb(key));
			require(!aarch32::key_is_canonical(key),
				"a misaligned A32 target made a storable key");
		} else {
			require(aarch32::key_is_thumb(key));
			require(aarch32::key_is_canonical(key));
		}
	}
	/* A Thumb target keeps its halfword, so two Thumb targets one
	 * halfword apart are two keys. */
	require(aarch32::key_from_target(0x1003U) !=
		aarch32::key_from_target(0x1005U));
}

/* What the caches hash by starts at bit 2 of the key. Consecutive
 * instructions must differ there in every ISA, or a straight-line run of
 * Thumb code would land in one bucket. */
void adjacent_instructions_differ_above_bit_one()
{
	for (const auto raw : sample_pcs()) {
		const auto word_pc = raw & ~3U;
		const auto half_pc = raw & ~1U;
		if (word_pc != 0xffff'fffcU)
			require(aarch32::a32_key(word_pc + 4U) -
				aarch32::a32_key(word_pc) == 4U);
		if (half_pc != 0xffff'fffeU) {
			/* The shift moves the halfword bit to bit 2. */
			require(aarch32::t32_key(half_pc + 2U) -
				aarch32::t32_key(half_pc) == 4U);
			require((aarch32::t32_key(half_pc + 2U) >> 2) ==
				(aarch32::t32_key(half_pc) >> 2) + 1U);
		}
	}
}

void unit_sizes_and_fetch_addresses()
{
	require(aarch32::isa_unit_bytes(GuestIsa::a64) == 4);
	require(aarch32::isa_unit_bytes(GuestIsa::a32) == 4);
	require(aarch32::isa_unit_bytes(GuestIsa::t32) == 2);
	require(aarch32::key_fetch_address(0x4000'1003U) == 0x4000'1000U);
	require(aarch32::key_fetch_address(0x7fff'ffff'f001ULL) ==
		0x7fff'ffff'f000ULL);
	for (const auto raw : sample_pcs()) {
		require(aarch32::key_fetch_address(aarch32::a32_key(raw & ~3U)) ==
			(raw & ~3U));
		require(aarch32::key_fetch_address(aarch32::t32_key(raw & ~1U)) ==
			(raw & ~1U));
	}
}

/* A fake guest memory: a byte image at `base`, a page size, optional
 * unreadable bytes, and a count of fetches. */
class FakeMemory final {
public:
	FakeMemory(std::uint64_t base, std::vector<std::byte> bytes)
		: base_{base}, bytes_{std::move(bytes)}
	{
	}

	void poison(std::uint64_t address) { poison_ = address; }

	[[nodiscard]] bool fetch(std::uint64_t address, std::span<std::byte> out)
	{
		++fetches;
		if (address < base_ || address + out.size() > base_ + bytes_.size())
			return false;
		if (poison_ >= address && poison_ < address + out.size())
			return false;
		std::memcpy(out.data(), bytes_.data() + (address - base_), out.size());
		return true;
	}

	std::size_t fetches{};

private:
	std::uint64_t base_;
	std::vector<std::byte> bytes_;
	std::uint64_t poison_{std::numeric_limits<std::uint64_t>::max()};
};

[[nodiscard]] std::vector<std::byte> pattern(std::size_t bytes,
	std::uint8_t seed)
{
	std::vector<std::byte> out(bytes);
	for (std::size_t index = 0; index < bytes; ++index)
		out[index] = static_cast<std::byte>(seed + index * 7U + index / 3U);
	return out;
}

[[nodiscard]] GuestValidation validate(FakeMemory &memory,
	std::uint64_t key, std::size_t units, std::span<const std::byte> saved,
	std::uint64_t page_size, bool per_unit)
{
	std::array<std::byte, 256> scratch{};
	require(units * aarch32::isa_unit_bytes(aarch32::key_isa(key)) <=
		scratch.size());
	return aarch32::validate_guest_units(key, units, saved.data(), scratch,
		page_size,
		[&memory](std::uint64_t address, std::span<std::byte> out) {
			return memory.fetch(address, out);
		},
		per_unit);
}

/* Match, mismatch and fault, for each ISA, starting before a page boundary so
 * the span splits there. */
void validation_reads_page_bounded_runs()
{
	for (const auto isa : {GuestIsa::a64, GuestIsa::a32, GuestIsa::t32}) {
		const std::size_t unit = aarch32::isa_unit_bytes(isa);
		/* A span of 24 units whose 5th unit from the end sits on the
		 * boundary: the run splits in two. */
		const std::size_t units = 24;
		const std::uint64_t boundary = 0x4'0000 + page_size;
		const std::uint64_t start = boundary - (units - 5) * unit;
		const std::uint64_t key = isa == GuestIsa::a64 ? start :
			isa == GuestIsa::a32 ?
			aarch32::a32_key(static_cast<std::uint32_t>(start)) :
			aarch32::t32_key(static_cast<std::uint32_t>(start));
		const auto saved = pattern(units * unit, 0x40);

		std::vector<std::byte> image(2 * page_size, std::byte{0xee});
		std::memcpy(image.data() + (start - 0x4'0000), saved.data(),
			saved.size());
		FakeMemory memory{0x4'0000, image};
		for (const bool per_unit : {false, true}) {
			memory.fetches = 0;
			require(validate(memory, key, units, saved, page_size,
				per_unit) == GuestValidation::match,
				"identical words did not validate");
			require(memory.fetches == 2,
				"a span did not read one run per page");
		}
		require(validate(memory, key, 0, saved, page_size, false) ==
			GuestValidation::match);

		/* A difference in each place: first and last unit, either
		 * side of the page boundary, and the last byte of the span. */
		for (const std::size_t unit_index : {std::size_t{0},
			     units - 6, units - 5, units - 1}) {
			for (const std::size_t byte : {std::size_t{0}, unit - 1}) {
				auto changed = saved;
				changed[unit_index * unit + byte] ^= std::byte{0x10};
				for (const bool per_unit : {false, true})
					require(validate(memory, key, units, changed,
						page_size, per_unit) ==
						GuestValidation::mismatch,
						"a changed byte validated");
			}
		}

		/* Unreadable memory is a fault, from the page it lies in; with
		 * the per-unit retry a difference before it still wins. */
		FakeMemory partial{0x4'0000, image};
		partial.poison(boundary + unit);
		require(validate(partial, key, units, saved, page_size, false) ==
			GuestValidation::fault);
		require(validate(partial, key, units, saved, page_size, true) ==
			GuestValidation::fault);
		auto early = saved;
		early[(units - 5) * unit] ^= std::byte{1};
		require(validate(partial, key, units, early, page_size, false) ==
			GuestValidation::fault, "no retry: the fault wins");
		require(validate(partial, key, units, early, page_size, true) ==
			GuestValidation::mismatch,
			"a difference before the unreadable unit was lost");
		/* A difference in the first page is found before the second
		 * page is read, with or without the retry. */
		auto first_page = saved;
		first_page[0] ^= std::byte{1};
		require(validate(partial, key, units, first_page, page_size,
			false) == GuestValidation::mismatch);
		require(validate(partial, key, units, first_page, page_size,
			true) == GuestValidation::mismatch);

		/* Poison inside one page's run: the retry reports a mismatch
		 * that precedes it, a fault only without one. */
		FakeMemory inner{0x4'0000, image};
		inner.poison(start + 10 * unit);
		auto before = saved;
		before[2 * unit] ^= std::byte{0x80};
		require(validate(inner, key, units, saved, page_size, false) ==
			GuestValidation::fault);
		require(validate(inner, key, units, before, page_size, false) ==
			GuestValidation::fault);
		require(validate(inner, key, units, before, page_size, true) ==
			GuestValidation::mismatch,
			"a difference before an unreadable unit was lost");
		require(validate(inner, key, units, saved, page_size, true) ==
			GuestValidation::fault);
		auto after = saved;
		after[12 * unit] ^= std::byte{0x80};
		require(validate(inner, key, units, after, page_size, true) ==
			GuestValidation::fault,
			"a difference after an unreadable unit was reported");

		/* Not mapped at all. */
		FakeMemory none{0x9'0000, pattern(64, 1)};
		require(validate(none, key, units, saved, page_size, false) ==
			GuestValidation::fault);
		require(validate(none, key, units, saved, page_size, true) ==
			GuestValidation::fault);
	}
}

/* A Thumb 32-bit instruction can straddle a page boundary: the units are
 * halfwords, so the span splits between its halves and each is read in its
 * own page. An odd count of halfwords is the normal case. */
void thumb_spans_split_between_halfwords()
{
	const std::uint64_t boundary = 0x8'0000 + page_size;
	for (const std::size_t units : {std::size_t{1}, std::size_t{3},
		     std::size_t{7}, std::size_t{33}}) {
		for (const std::size_t before : {std::size_t{1}, units - 1U,
			     std::size_t{0}}) {
			if (before >= units)
				continue;
			const std::uint64_t start = boundary - before * 2;
			const auto saved = pattern(units * 2, 0x5a);
			std::vector<std::byte> image(2 * page_size, std::byte{0});
			std::memcpy(image.data() + (start - 0x8'0000),
				saved.data(), saved.size());
			FakeMemory memory{0x8'0000, image};
			const auto key = aarch32::t32_key(
				static_cast<std::uint32_t>(start));
			require(validate(memory, key, units, saved, page_size,
				false) == GuestValidation::match);
			require(memory.fetches == (before == 0 ? 1U : 2U));
			auto changed = saved;
			changed.back() ^= std::byte{1};
			require(validate(memory, key, units, changed, page_size,
				true) == GuestValidation::mismatch);
		}
	}
}

/* A window is fetched one page-bounded run at a time, in units of a byte, a
 * halfword or a word, and ends at the first run that cannot be read. */
void fetch_reads_a_page_bounded_window()
{
	for (const std::size_t unit : {std::size_t{1}, std::size_t{2},
		     std::size_t{4}}) {
		const std::size_t units = 24;
		const std::uint64_t boundary = 0x6'0000 + page_size;
		const std::uint64_t start = boundary - 5 * unit;
		const auto expected = pattern(units * unit, 0x21);
		std::vector<std::byte> image(2 * page_size, std::byte{0xee});
		std::memcpy(image.data() + (start - 0x6'0000), expected.data(),
			expected.size());
		const auto read = [&](FakeMemory &memory, std::uint64_t address,
				      std::span<std::byte> out) {
			return aarch32::fetch_guest_units(address, unit, out,
				page_size,
				[&memory](std::uint64_t at,
					  std::span<std::byte> bytes) {
					return memory.fetch(at, bytes);
				});
		};

		/* Across the boundary: two runs, every byte placed. */
		FakeMemory memory{0x6'0000, image};
		std::vector<std::byte> out(units * unit, std::byte{0});
		require(read(memory, start, out) == units,
			"a window across a page was cut short");
		require(memory.fetches == 2, "one run per page was not read");
		require(out == expected, "the window holds the wrong bytes");

		/* Inside one page: a single run. */
		memory.fetches = 0;
		require(read(memory, start - 8 * unit,
			std::span{out}.first(4 * unit)) == 4);
		require(memory.fetches == 1);

		/* Nothing asked for, nothing read. A byte tail short of a unit
		 * is not a unit. */
		memory.fetches = 0;
		require(read(memory, start, std::span{out}.first(0)) == 0);
		require(read(memory, start,
			std::span{out}.first(unit - 1)) == 0);
		require(memory.fetches == 0);

		/* An unreadable second page keeps the first run's units. */
		FakeMemory second{0x6'0000, image};
		second.poison(boundary + unit);
		require(read(second, start, out) == 5,
			"the units before an unreadable run were lost");
		/* An unreadable first page is an empty window. */
		FakeMemory first{0x6'0000, image};
		first.poison(start);
		require(read(first, start, out) == 0);
		/* A window with no mapping at all. */
		FakeMemory none{0x9'0000, pattern(64, 1)};
		require(read(none, start, out) == 0);
	}
}

/* A PC the keys cannot name (an odd Thumb address at the last byte of a
 * page) still terminates, with a deterministic answer. */
void malformed_pc_terminates()
{
	FakeMemory memory{0x1'0000, std::vector<std::byte>(2 * page_size)};
	const auto saved = pattern(8, 3);
	std::array<std::byte, 16> scratch{};
	const std::uint64_t key = aarch32::aarch32_key_bit |
		aarch32::thumb_key_bit | (std::uint64_t{0x1'0fff} << 1 | 0U);
	/* pc 0x10fff has bit 0 set and is the last byte of its page. */
	require(aarch32::key_pc(key) == 0x1'0fffU);
	const auto result = aarch32::validate_guest_units(key, 2, saved.data(),
		scratch, page_size,
		[&memory](std::uint64_t address, std::span<std::byte> out) {
			return memory.fetch(address, out);
		},
		true);
	require(result == GuestValidation::match ||
		result == GuestValidation::mismatch ||
		result == GuestValidation::fault);

	/* The same address as the start of a window: a run of one halfword
	 * across the page end, then the aligned remainder. */
	std::array<std::byte, 8> window{};
	std::vector<std::uint64_t> addresses;
	const auto count = aarch32::fetch_guest_units(0x1'0fffU, 2, window,
		page_size,
		[&](std::uint64_t address, std::span<std::byte> out) {
			addresses.push_back(address);
			return memory.fetch(address, out);
		});
	require(count == 4 && addresses.size() >= 2 &&
		addresses.front() == 0x1'0fffU,
		"a malformed window did not read its first unit in a run of one");
}

} // namespace

int main()
{
	keys_round_trip();
	keys_never_collide();
	a32_target_with_bit_one_is_never_a_key();
	adjacent_instructions_differ_above_bit_one();
	unit_sizes_and_fetch_addresses();
	validation_reads_page_bounded_runs();
	fetch_reads_a_page_bounded_window();
	thumb_spans_split_between_halfwords();
	malformed_pc_terminates();
	return 0;
}
