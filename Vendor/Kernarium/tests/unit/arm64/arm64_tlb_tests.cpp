// SPDX-License-Identifier: GPL-2.0-or-later
#include "memory/tlb.hpp"
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"
#include "static_mmu_test_support.hpp"
#include "scratch_backing.hpp"
#include "require.hpp"
#include "runtime_test_route.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sys/mman.h>

namespace {

using uml::memory::Access;
using uml::memory::Tlb;
using uml::memory::tlb_empty_key;

constexpr std::uint64_t page = DARWIN_UML_GUEST_PAGE_SIZE;

using uml::testing::require;

class Mapping final {
public:
	Mapping(unsigned long generation, std::uint64_t base, std::size_t pages,
		int protection = PROT_READ | PROT_WRITE)
		: generation_{generation}
		, backing_{"uml-tlb.XXXXXX", pages * page}
	{
		require(darwin_uml_static_vm_map_generation(generation,
			reinterpret_cast<void *>(base), backing_.descriptor(),
			0, pages * page, protection, 0, 1) == 0);
	}

	Mapping(const Mapping &) = delete;
	Mapping &operator=(const Mapping &) = delete;

	~Mapping()
	{
		require(darwin_uml_static_vm_destroy(generation_) == 0);
	}

private:
	unsigned long generation_;
	uml::testing::ScratchBacking backing_;
};

[[nodiscard]] std::uint64_t changes(unsigned long generation)
{
	return darwin_uml_static_vm_sequences(generation).mapping_changes;
}

[[nodiscard]] bool empty(const Tlb &tlb)
{
	return std::ranges::all_of(tlb.entries, [](const auto &entry) {
		return entry.read_page == tlb_empty_key &&
			entry.write_page == tlb_empty_key;
	});
}

/* Whether any entry holds the page, as presented or with its tag stripped. */
[[nodiscard]] bool cached(const Tlb &tlb, std::uint64_t address)
{
	constexpr std::uint64_t untagged =
		0x00ff'ffff'ffff'ffffULL >> DARWIN_UML_GUEST_PAGE_SHIFT;
	const auto key = (address >> DARWIN_UML_GUEST_PAGE_SHIFT) & untagged;
	return std::ranges::any_of(tlb.entries, [&](const auto &entry) {
		return (entry.read_page != tlb_empty_key &&
				(entry.read_page & untagged) == key) ||
			(entry.write_page != tlb_empty_key &&
				(entry.write_page & untagged) == key);
	});
}

void fill(Tlb &tlb, std::uint64_t base, std::size_t pages)
{
	for (std::size_t index = 0; index < pages; ++index) {
		const auto address = base + index * page;
		require(tlb.translate(address, Access::read) != nullptr);
		if (index % 2 == 0)
			require(tlb.translate(address, Access::write) != nullptr);
	}
}

/* A flush clears only the sets filled since the previous one until more sets
 * were filled than it tracks; both must leave nothing a lookup could hit. */
void generation_switch_clears_every_filled_set()
{
	constexpr unsigned long first = 220;
	constexpr unsigned long second = 221;
	constexpr std::uint64_t base = 0x80'0000;
	constexpr std::size_t pages = 3 * uml::memory::tlb_dirty_capacity;
	Mapping mapping{first, base, pages};
	auto tlb = std::make_unique<Tlb>();
	require(empty(*tlb));

	for (const std::size_t filled : {std::size_t{5},
		     uml::memory::tlb_dirty_capacity, pages}) {
		tlb->prepare(first, changes(first));
		fill(*tlb, base, filled);
		require(cached(*tlb, base) &&
			cached(*tlb, base + (filled - 1) * page));
		tlb->prepare(second, 0);
		require(empty(*tlb));
	}
	/* The table refills correctly after a partial flush. */
	tlb->prepare(first, changes(first));
	fill(*tlb, base, 3);
	tlb->prepare(second, 0);
	tlb->prepare(first, changes(first));
	require(empty(*tlb));
	fill(*tlb, base, pages);
	require(cached(*tlb, base + (pages - 1) * page));
}

/* An unmap wider than the table invalidates by key: every entry in the range,
 * tagged or not, goes, and entries on either side stay. */
void wide_unmap_invalidates_only_its_range()
{
	constexpr unsigned long generation = 222;
	/* The five pages probed below occupy distinct entries, so no fill
	 * evicts another. */
	constexpr std::uint64_t base = 0x1600'0000;
	constexpr std::size_t removed = uml::memory::tlb_entry_count + 2;
	constexpr std::size_t pages = removed + 2;
	constexpr std::uint64_t tag = 0x5aULL << 56;
	Mapping mapping{generation, base, pages};
	auto tlb = std::make_unique<Tlb>();
	tlb->tagged_addresses = true;
	tlb->prepare(generation, changes(generation));

	const std::uint64_t below = base;
	const std::uint64_t inside = base + page;
	const std::uint64_t tagged = base + 7 * page;
	const std::uint64_t last_inside = base + removed * page;
	const std::uint64_t above = base + (removed + 1) * page;
	for (const auto address : {below, inside, last_inside, above}) {
		require(tlb->translate(address, Access::read) != nullptr);
		require(tlb->translate(address, Access::write) != nullptr);
	}
	require(tlb->translate_tagged(tagged | tag, Access::read) != nullptr);
	require(tlb->translate(tagged | tag, Access::read) != nullptr);
	for (const auto address : {below, inside, tagged, last_inside, above})
		require(cached(*tlb, address));

	require(darwin_uml_static_vm_unmap_generation(generation,
		reinterpret_cast<void *>(inside), removed * page) == 0);
	tlb->prepare(generation, changes(generation));
	require(!cached(*tlb, inside));
	require(!cached(*tlb, tagged));
	require(!cached(*tlb, last_inside));
	require(cached(*tlb, below));
	require(cached(*tlb, above));
	require(tlb->translate(inside, Access::read) == nullptr);
	require(tlb->translate(below, Access::write) != nullptr);
}

/* A narrow unmap still invalidates page by page. */
void narrow_unmap_invalidates_its_pages()
{
	constexpr unsigned long generation = 223;
	constexpr std::uint64_t base = 0x4000'0000;
	Mapping mapping{generation, base, 4};
	auto tlb = std::make_unique<Tlb>();
	tlb->prepare(generation, changes(generation));
	fill(*tlb, base, 4);
	require(darwin_uml_static_vm_unmap_generation(generation,
		reinterpret_cast<void *>(base + page), 2 * page) == 0);
	tlb->prepare(generation, changes(generation));
	require(cached(*tlb, base));
	require(!cached(*tlb, base + page));
	require(!cached(*tlb, base + 2 * page));
	require(cached(*tlb, base + 3 * page));
}

/* Whether the page's read and write entries are present. */
[[nodiscard]] bool has_entry(const Tlb &tlb, std::uint64_t address,
	Access access)
{
	const auto key = address >> DARWIN_UML_GUEST_PAGE_SHIFT;
	return std::ranges::any_of(tlb.entries, [&](const auto &entry) {
		return (access == Access::write ? entry.write_page :
			entry.read_page) == key;
	});
}

/* An exclusive load's translation fills the write entry of a writable page,
 * keeps to the read entry of one that is not, and faults only as a read. */
void exclusive_load_fills_the_write_entry()
{
	constexpr unsigned long generation = 224;
	constexpr std::uint64_t base = 0x2400'0000;
	constexpr std::uint64_t sentinel = 0x5eed'0000;
	Mapping mapping{generation, base, 2};
	auto tlb = std::make_unique<Tlb>();
	tlb->prepare(generation, changes(generation));
	tlb->fault_address = sentinel;
	tlb->fault_access = Access::execute;

	auto *host = tlb->translate_exclusive_load(base + 0x40);
	require(host != nullptr);
	require(has_entry(*tlb, base, Access::write));
	require(has_entry(*tlb, base, Access::read));
	/* The same bytes a later store reaches, and no fault recorded. */
	require(host == tlb->translate(base + 0x40, Access::write));
	require(tlb->fault_address == sentinel &&
		tlb->fault_access == Access::execute);
	/* A hit on the write entry returns the same translation. */
	require(tlb->translate_exclusive_load(base + 0x40) == host);

	require(darwin_uml_static_vm_unmap_generation(generation,
		reinterpret_cast<void *>(base), page) == 0);
	tlb->prepare(generation, changes(generation));
	require(!has_entry(*tlb, base, Access::write));
	/* An unmapped page faults as a read. */
	require(tlb->translate_exclusive_load(base) == nullptr);
	require(tlb->fault_address == base && tlb->fault_access == Access::read);
	require(!has_entry(*tlb, base, Access::read) &&
		!has_entry(*tlb, base, Access::write));
	/* The other page of the mapping is unaffected. */
	require(tlb->translate_exclusive_load(base + page) != nullptr);
	require(has_entry(*tlb, base + page, Access::write));
}

void exclusive_load_of_a_read_only_page()
{
	constexpr unsigned long generation = 225;
	constexpr std::uint64_t base = 0x2500'0000;
	constexpr std::uint64_t sentinel = 0x5eed'0000;
	Mapping mapping{generation, base, 1, PROT_READ};
	auto tlb = std::make_unique<Tlb>();
	tlb->prepare(generation, changes(generation));
	tlb->fault_address = sentinel;
	tlb->fault_access = Access::execute;

	auto *host = tlb->translate_exclusive_load(base + 8);
	require(host != nullptr);
	require(host == tlb->translate(base + 8, Access::read));
	require(has_entry(*tlb, base, Access::read));
	require(!has_entry(*tlb, base, Access::write));
	/* The failed write lookup left no fault behind. */
	require(tlb->fault_address == sentinel &&
		tlb->fault_access == Access::execute);
	/* The store still faults as a write when it comes. */
	require(tlb->translate(base + 8, Access::write) == nullptr);
	require(tlb->fault_address == base + 8 &&
		tlb->fault_access == Access::write);
}

/* A tagged lookup keys its entry by the page as presented, so the read and
 * write halves of one page can hold it under different tags at once, and an
 * invalidation finds both only by comparing the page number of the key. Every
 * way the mapping of the untagged page changes must remove them, and no tagged
 * lookup may then reach the old translation. */
constexpr std::uint64_t tag_a = 0xb4ULL << 56;
constexpr std::uint64_t tag_b = 0x2cULL << 56;

/* Holds `address` under tag A in the read half and tag B in the write half,
 * and returns the host pages those aliases translate to. */
struct Alias final {
	std::byte *read;
	std::byte *write;
};

[[nodiscard]] Alias hold_aliased(Tlb &tlb, std::uint64_t address)
{
	Alias alias{tlb.translate_tagged(address | tag_a, Access::read),
		tlb.translate_tagged(address | tag_b, Access::write)};
	require(alias.read != nullptr && alias.write != nullptr);
	return alias;
}

/* Whether a half holds the page under exactly this key. */
[[nodiscard]] bool holds(const Tlb &tlb, std::uint64_t address,
	Access access)
{
	const auto key = address >> DARWIN_UML_GUEST_PAGE_SHIFT;
	return std::ranges::any_of(tlb.entries, [&](const auto &entry) {
		return (access == Access::write ? entry.write_page :
			entry.read_page) == key;
	});
}

[[nodiscard]] bool faults_untagged(Tlb &tlb, std::uint64_t address,
	Access access)
{
	const auto tag = access == Access::write ? tag_b : tag_a;
	return tlb.translate_tagged(address | tag, access) == nullptr &&
		tlb.fault_address == address && tlb.fault_access == access;
}

void tagged_aliases_follow_a_narrow_unmap()
{
	constexpr unsigned long generation = 226;
	constexpr std::uint64_t base = 0x2600'0000;
	Mapping mapping{generation, base, 4};
	auto tlb = std::make_unique<Tlb>();
	tlb->tagged_addresses = true;
	tlb->prepare(generation, changes(generation));
	for (std::size_t index = 0; index < 4; ++index)
		(void)hold_aliased(*tlb, base + index * page);
	require(holds(*tlb, (base + page) | tag_a, Access::read) &&
		holds(*tlb, (base + page) | tag_b, Access::write));

	require(darwin_uml_static_vm_unmap_generation(generation,
		reinterpret_cast<void *>(base + page), 2 * page) == 0);
	tlb->prepare(generation, changes(generation));
	for (const auto index : {std::size_t{1}, std::size_t{2}}) {
		const auto address = base + index * page;
		require(!cached(*tlb, address));
		require(faults_untagged(*tlb, address, Access::read));
		require(faults_untagged(*tlb, address, Access::write));
		/* A fault leaves no entry behind. */
		require(!cached(*tlb, address));
	}
	for (const auto index : {std::size_t{0}, std::size_t{3}}) {
		const auto address = base + index * page;
		require(holds(*tlb, address | tag_a, Access::read));
		require(holds(*tlb, address | tag_b, Access::write));
	}
}

void tagged_aliases_follow_a_wide_unmap()
{
	constexpr unsigned long generation = 227;
	constexpr std::uint64_t base = 0x1a00'0000;
	constexpr std::size_t removed = uml::memory::tlb_entry_count + 2;
	constexpr std::size_t pages = removed + 2;
	Mapping mapping{generation, base, pages};
	auto tlb = std::make_unique<Tlb>();
	tlb->tagged_addresses = true;
	tlb->prepare(generation, changes(generation));
	const std::uint64_t kept_low = base;
	const std::uint64_t first = base + page;
	const std::uint64_t last = base + removed * page;
	const std::uint64_t kept_high = base + (removed + 1) * page;
	for (const auto address : {kept_low, first, last, kept_high})
		(void)hold_aliased(*tlb, address);

	require(darwin_uml_static_vm_unmap_generation(generation,
		reinterpret_cast<void *>(first), removed * page) == 0);
	tlb->prepare(generation, changes(generation));
	for (const auto address : {first, last}) {
		require(!cached(*tlb, address));
		require(faults_untagged(*tlb, address, Access::read));
		require(faults_untagged(*tlb, address, Access::write));
	}
	for (const auto address : {kept_low, kept_high}) {
		require(holds(*tlb, address | tag_a, Access::read));
		require(holds(*tlb, address | tag_b, Access::write));
	}
}

/* The same address mapped again onto other backing is a new translation: a
 * tagged lookup of it must reach the new bytes, never the old ones. */
void tagged_aliases_do_not_outlive_a_remap()
{
	constexpr unsigned long generation = 228;
	constexpr std::uint64_t base = 0x2700'0000;
	Mapping mapping{generation, base, 2};
	auto tlb = std::make_unique<Tlb>();
	tlb->tagged_addresses = true;
	tlb->prepare(generation, changes(generation));
	const auto before = hold_aliased(*tlb, base);

	require(darwin_uml_static_vm_unmap_generation(generation,
		reinterpret_cast<void *>(base), page) == 0);
	uml::testing::ScratchBacking replacement{"uml-tlb-remap.XXXXXX", page};
	replacement.put_byte(0x10, std::byte{0x5a});
	require(darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(base), replacement.descriptor(), 0,
		page, PROT_READ | PROT_WRITE, 0, 1) == 0);
	tlb->prepare(generation, changes(generation));
	require(!cached(*tlb, base));

	const auto after = hold_aliased(*tlb, base);
	require(after.read != before.read && after.write != before.write);
	require(after.read[0x10] == std::byte{0x5a});
	require(after.write[0x10] == std::byte{0x5a});
	/* The write alias reaches the page the read alias does. */
	after.write[0x11] = std::byte{0x77};
	require(after.read[0x11] == std::byte{0x77});
}

/* A protection change drops the aliases of the page: a write through the
 * write alias must fault after the page becomes read-only. */
void tagged_aliases_follow_a_protection_change()
{
	constexpr unsigned long generation = 229;
	constexpr std::uint64_t base = 0x2800'0000;
	Mapping mapping{generation, base, 2};
	require(darwin_uml_static_vm_activate(generation) == 0);
	auto tlb = std::make_unique<Tlb>();
	tlb->tagged_addresses = true;
	tlb->prepare(generation, changes(generation));
	(void)hold_aliased(*tlb, base);
	(void)hold_aliased(*tlb, base + page);

	require(darwin_uml_static_vm_protect(reinterpret_cast<void *>(base),
		page, PROT_READ) == 0);
	tlb->prepare(generation, changes(generation));
	require(!cached(*tlb, base));
	require(faults_untagged(*tlb, base, Access::write));
	require(tlb->translate_tagged(base | tag_a, Access::read) != nullptr);
	/* The neighbour keeps both aliases and its permission. */
	require(holds(*tlb, (base + page) | tag_a, Access::read));
	require(holds(*tlb, (base + page) | tag_b, Access::write));
	require(tlb->translate_tagged((base + page) | tag_b,
		Access::write) != nullptr);
}

/* An address-space switch and the overflow of the mapping history flush every
 * filled set, whatever tag keyed it. */
void tagged_aliases_follow_a_generation_switch()
{
	constexpr unsigned long first = 230;
	constexpr unsigned long second = 231;
	constexpr std::uint64_t base = 0x2900'0000;
	constexpr std::size_t pages = 3 * uml::memory::tlb_dirty_capacity;
	Mapping mapping{first, base, pages};
	auto tlb = std::make_unique<Tlb>();
	tlb->tagged_addresses = true;
	for (const std::size_t filled : {std::size_t{5},
		     uml::memory::tlb_dirty_capacity, pages}) {
		tlb->prepare(first, changes(first));
		for (std::size_t index = 0; index < filled; ++index)
			(void)hold_aliased(*tlb, base + index * page);
		require(holds(*tlb, (base + (filled - 1) * page) | tag_a,
			Access::read));
		tlb->prepare(second, 0);
		require(empty(*tlb));
	}
	/* Back in the first space, nothing of the earlier visit remains. */
	tlb->prepare(first, changes(first));
	(void)hold_aliased(*tlb, base);
	tlb->prepare(second, 0);
	tlb->prepare(first, changes(first));
	require(empty(*tlb));
}

void tagged_aliases_follow_a_history_overflow()
{
	constexpr unsigned long generation = 232;
	constexpr std::uint64_t base = 0x2a00'0000;
	/* More single-page changes than one reconcile can list. */
	constexpr std::size_t churn = 300;
	Mapping mapping{generation, base, 8 + churn};
	auto tlb = std::make_unique<Tlb>();
	tlb->tagged_addresses = true;
	tlb->prepare(generation, changes(generation));
	for (std::size_t index = 0; index < 8; ++index)
		(void)hold_aliased(*tlb, base + index * page);
	for (std::size_t index = 0; index < churn; ++index)
		require(darwin_uml_static_vm_unmap_generation(generation,
			reinterpret_cast<void *>(base + (8 + index) * page),
			page) == 0);
	tlb->prepare(generation, changes(generation));
	require(empty(*tlb));
	require(tlb->translate_tagged(base | tag_a, Access::read) != nullptr);
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	generation_switch_clears_every_filled_set();
	wide_unmap_invalidates_only_its_range();
	narrow_unmap_invalidates_its_pages();
	exclusive_load_fills_the_write_entry();
	exclusive_load_of_a_read_only_page();
	tagged_aliases_follow_a_narrow_unmap();
	tagged_aliases_follow_a_wide_unmap();
	tagged_aliases_do_not_outlive_a_remap();
	tagged_aliases_follow_a_protection_change();
	tagged_aliases_follow_a_generation_switch();
	tagged_aliases_follow_a_history_overflow();
}
