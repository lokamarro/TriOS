// SPDX-License-Identifier: GPL-2.0-or-later
#include "backing_slots.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <sys/stat.h>
#include <vector>

namespace {

using uml::memory::BackingSlots;
using uml::memory::MappedBacking;
using uml::testing::ScratchBacking;
using uml::testing::require;

constexpr std::string_view backing_name = "uml-backing-slots-XXXXXX";
constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;

/* A backing of its own: a scratch file the test keeps open. */
class Backing final {
public:
	Backing() : file_{backing_name, page_size}
	{
		struct stat status {};
		require(fstat(file_.descriptor(), &status) == 0);
		backing_ = MappedBacking::create(file_.descriptor(), status);
		require(backing_ != nullptr);
	}

	[[nodiscard]] const std::shared_ptr<MappedBacking> &backing() const
	{
		return backing_;
	}
	[[nodiscard]] int fd() const { return file_.descriptor(); }

private:
	ScratchBacking file_;
	std::shared_ptr<MappedBacking> backing_;
};

void a_pair_keeps_its_slot_and_a_slot_follows_its_pages()
{
	Backing first;
	Backing second;
	BackingSlots slots;
	require(slots.slots_in_use() == 0);

	const auto one = slots.claim(first.backing(), first.fd());
	const auto two = slots.claim(second.backing(), second.fd());
	require(one == 0 && two == 1);
	/* The same pair is the same slot, and the same backing under another
	 * descriptor is another pair. */
	require(slots.claim(first.backing(), first.fd()) == one);
	const auto alias = slots.claim(first.backing(), first.fd() + 1000);
	require(alias == 2);
	require(slots.slots_in_use() == 3);

	const auto entry = slots.entry(one, 0);
	require(entry.backing == first.backing().get() &&
		entry.fd == first.fd());
	require(slots.entry(alias, 0).backing == first.backing().get() &&
		slots.entry(alias, 0).fd == first.fd() + 1000);

	slots.add(one, 0x1000, first.backing(), first.fd());
	slots.add(one, 0x2000, first.backing(), first.fd());
	slots.add(two, 0x3000, second.backing(), second.fd());
	require(slots.pages_in(one) == 2 && slots.pages_in(two) == 1);
	/* Settling releases the claim nothing used. */
	slots.settle();
	require(slots.slots_in_use() == 2);
	require(slots.entry(alias, 0).backing == nullptr);

	/* A slot is released when its last page goes, and is the first one the
	 * next pair takes. */
	slots.remove(one, 0x1000);
	slots.settle();
	require(slots.slots_in_use() == 2 && slots.pages_in(one) == 1);
	slots.remove(one, 0x2000);
	slots.settle();
	require(slots.slots_in_use() == 1);
	require(slots.entry(one, 0).backing == nullptr);
	Backing third;
	require(slots.claim(third.backing(), third.fd()) == one);
	require(slots.entry(two, 0).backing == second.backing().get());
	slots.settle();
	slots.clear();
	require(slots.slots_in_use() == 0);
}

void a_change_that_stops_after_its_claims_leaves_nothing_behind()
{
	Backing kept;
	Backing claimed;
	BackingSlots slots;
	require(slots.claim(kept.backing(), kept.fd()) == 0);
	slots.add(0, 0x1000, kept.backing(), kept.fd());
	slots.settle();

	/* A change claims a slot and room for an overflow page, then fails
	 * before it counts anything in. */
	require(slots.claim(claimed.backing(), claimed.fd()) == 1);
	slots.reserve_overflow(0x5000);
	require(slots.overflow_pages() == 1);
	slots.settle();
	require(slots.slots_in_use() == 1);
	require(slots.overflow_pages() == 0);
	require(slots.pages_in(0) == 1);
	require(slots.entry(0, 0).backing == kept.backing().get());
}

void more_backings_than_slots_fall_back_to_the_overflow_table()
{
	constexpr unsigned count = 70;
	std::vector<std::unique_ptr<Backing>> backings;
	for (unsigned index = 0; index < count; ++index)
		backings.push_back(std::make_unique<Backing>());

	BackingSlots slots;
	std::vector<unsigned> claimed;
	for (unsigned index = 0; index < count; ++index) {
		const auto &backing = *backings[index];
		const auto slot = slots.claim(backing.backing(), backing.fd());
		claimed.push_back(slot);
		const std::uint64_t page = (index + 1) * page_size;
		if (slot == BackingSlots::overflow)
			slots.reserve_overflow(page);
		slots.add(slot, page, backing.backing(), backing.fd());
	}
	slots.settle();
	for (unsigned index = 0; index < BackingSlots::slot_count; ++index)
		require(claimed[index] == index);
	for (unsigned index = BackingSlots::slot_count; index < count; ++index)
		require(claimed[index] == BackingSlots::overflow);
	require(slots.slots_in_use() == BackingSlots::slot_count);
	require(slots.overflow_pages() == count - BackingSlots::slot_count);

	/* Every page resolves to its own backing and descriptor, in a slot or
	 * in the table. */
	for (unsigned index = 0; index < count; ++index) {
		const auto &backing = *backings[index];
		const auto entry = slots.entry(
			claimed[index], (index + 1) * page_size);
		require(entry.backing == backing.backing().get());
		require(entry.fd == backing.fd());
	}
	/* The table answers for a page it holds and for no other. */
	require(slots.entry(BackingSlots::overflow, 0).backing == nullptr);
	require(slots.entry(BackingSlots::overflow, 1000 * page_size).backing ==
		nullptr);

	/* A page can move from one overflow backing to another in place. */
	const std::uint64_t moved = (BackingSlots::slot_count + 1) * page_size;
	slots.add(BackingSlots::overflow, moved, backings[0]->backing(),
		backings[0]->fd());
	require(slots.entry(BackingSlots::overflow, moved).backing ==
		backings[0]->backing().get());
	slots.settle();
	require(slots.overflow_pages() == count - BackingSlots::slot_count);

	/* Freeing a slot lets the next pair back in; the overflow table keeps
	 * what it holds until those pages go. */
	slots.remove(0, page_size);
	slots.settle();
	require(slots.slots_in_use() == BackingSlots::slot_count - 1);
	const auto &late = *backings[count - 1];
	require(slots.claim(late.backing(), late.fd()) == 0);
	slots.remove(BackingSlots::overflow, moved);
	slots.settle();
	require(slots.overflow_pages() ==
		count - BackingSlots::slot_count - 1);
	require(slots.entry(BackingSlots::overflow, moved).backing == nullptr);
	slots.settle();
	slots.clear();
	require(slots.slots_in_use() == 0 && slots.overflow_pages() == 0);
}

} // namespace

int main()
{
	const uml::testing::NamedTest tests[] = {
		{"a pair keeps its slot and a slot follows its pages",
			a_pair_keeps_its_slot_and_a_slot_follows_its_pages},
		{"a change that stops after its claims leaves nothing behind",
			a_change_that_stops_after_its_claims_leaves_nothing_behind},
		{"more backings than slots fall back to the overflow table",
			more_backings_than_slots_fall_back_to_the_overflow_table},
	};
	return uml::testing::runNamedTests(tests);
}
