// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/gadget_generation_window.hpp"
#include "require.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cache = uml::cpu::gadget_cache;

namespace {

using uml::testing::require;

/* The translation space is no part of the window's bookkeeping. */
using State = cache::GenerationState<int, unsigned long>;

constexpr std::size_t limit = 3;
using Window = cache::GenerationWindow<unsigned long, State, limit>;

/* What a cache does on first sight of an address space, less its
 * translation space. */
State &admit(Window &window, unsigned long generation)
{
	auto [entry, inserted] = window.generations.try_emplace(generation);
	require(inserted);
	window.enroll(entry->second, generation);
	window.touch(generation);
	return entry->second;
}

/* And what it does to retire one, less its slots. */
void retire(Window &window, unsigned long generation, bool tombstone = false)
{
	const auto owner = window.generations.find(generation);
	if (owner != window.generations.end()) {
		window.reserve_id();
		window.leave(owner, owner->second.id);
	}
	if (!tombstone)
		window.forget_place(generation);
}

void ids_count_from_one_and_are_recycled_last_in_first_out()
{
	Window window;
	require(window.owners.size() == 1 && window.owners[0] == nullptr);
	auto &first = admit(window, 100);
	auto &second = admit(window, 200);
	auto &third = admit(window, 300);
	require(first.id == 1 && second.id == 2 && third.id == 3);
	require(first.value == 100 && third.value == 300);
	require(window.owners[1] == &first && window.owners[3] == &third);

	retire(window, 100);
	retire(window, 300);
	require(window.owners[1] == nullptr && window.owners[3] == nullptr);
	require(window.owners[2] == &second);
	/* The id retired last is the first handed out again. */
	require(admit(window, 400).id == 3);
	require(admit(window, 500).id == 1);
	require(admit(window, 600).id == 4);
}

void the_last_id_a_tag_can_carry_is_handed_out()
{
	Window window;
	std::vector<State *> states;
	for (unsigned long generation = 1; generation <= cache::max_owner_id;
	     ++generation) {
		auto [entry, inserted] = window.generations.try_emplace(generation);
		require(inserted);
		window.enroll(entry->second, generation);
		states.push_back(&entry->second);
	}
	require(states.back()->id == cache::max_owner_id);
	/* No owner id is one a tag cannot hold, and the unbound owner bits are
	 * none of them. */
	window.bind(*states.back());
	require((window.current_owner >> cache::tag_owner_shift) ==
		cache::max_owner_id);
	require(window.current_owner != cache::no_owner);
	require((cache::no_owner >> cache::tag_owner_shift) ==
		cache::max_owner_id + 1U);
}

void binding_names_the_owner_and_the_epoch_a_lookup_compares()
{
	Window window;
	require(window.current == nullptr && window.current_epoch == 0 &&
		window.current_owner == cache::no_owner);
	auto &first = admit(window, 7);
	auto &second = admit(window, 8);
	second.epoch = 5;

	window.current = &first;
	window.bind(first);
	require(window.current_owner ==
		std::uint64_t{first.id} << cache::tag_owner_shift);
	require(window.current_epoch == 1);
	window.current = &second;
	window.bind(second);
	require(window.current_owner ==
		std::uint64_t{second.id} << cache::tag_owner_shift);
	require(window.current_epoch == 5);

	window.unbind();
	require(window.current == nullptr && window.current_epoch == 0 &&
		window.current_owner == cache::no_owner);

	/* Retiring the running address space unbinds it; another's retirement
	 * leaves the binding alone. */
	window.current = &first;
	window.bind(first);
	retire(window, 8);
	require(window.current == &first &&
		window.current_owner ==
			std::uint64_t{first.id} << cache::tag_owner_shift);
	retire(window, 7);
	require(window.current == nullptr &&
		window.current_owner == cache::no_owner &&
		window.current_epoch == 0);
}

void an_epoch_wrap_clears_the_validations_it_would_repeat()
{
	Window window;
	auto &state = admit(window, 1);
	auto &other = admit(window, 2);
	window.current = &state;
	window.bind(state);

	unsigned forgotten = 0;
	const auto forget = [&](State &forgetting) noexcept {
		require(&forgetting == &state);
		++forgotten;
	};
	for (unsigned change = 2; change <= cache::max_epoch; ++change) {
		window.advance_epoch(state, forget);
		require(state.epoch == change && window.current_epoch == change);
	}
	require(state.epoch == cache::max_epoch && forgotten == 0);

	/* The change that would pass the maximum clears first and starts over
	 * from one, which is not the epoch of a slot that was cleared (zero). */
	window.advance_epoch(state, forget);
	require(forgotten == 1 && state.epoch == 1 && window.current_epoch == 1);

	/* An address space that is not running moves only its own count. */
	window.advance_epoch(state, forget);
	require(state.epoch == 2 && window.current_epoch == 2);
	other.epoch = cache::max_epoch;
	window.advance_epoch(other, [&](State &forgetting) noexcept {
		require(&forgetting == &other);
		++forgotten;
	});
	require(forgotten == 2 && other.epoch == 1 && state.epoch == 2 &&
		window.current_epoch == 2);
	window.advance_epoch(other, forget);
	require(other.epoch == 2 && window.current_epoch == 2);
}

void the_order_keeps_the_least_recently_current_first()
{
	Window window;
	std::vector<unsigned long> retired;
	const auto trim = [&] {
		window.trim([&](unsigned long generation) {
			retired.push_back(generation);
			retire(window, generation);
		});
	};
	for (unsigned long generation = 1; generation <= limit; ++generation)
		admit(window, generation);
	trim();
	require(retired.empty() && window.generations.size() == limit);

	/* Current again, 1 is the most recent and 2 the oldest. */
	window.touch(1);
	require((window.order == std::vector<unsigned long>{2, 3, 1}));
	admit(window, 4);
	trim();
	require((retired == std::vector<unsigned long>{2}));
	admit(window, 5);
	trim();
	require((retired == std::vector<unsigned long>{2, 3}));
	require((window.order == std::vector<unsigned long>{1, 4, 5}));
	require(window.generations.size() == limit);
	require(window.generations.count(2) == 0 &&
		window.generations.count(3) == 0);

	/* Touching what is already last, or what was never there, is not
	 * a duplicate. */
	window.touch(5);
	window.touch(9);
	require((window.order == std::vector<unsigned long>{1, 4, 5, 9}));
	window.forget_place(4);
	window.forget_place(77);
	require((window.order == std::vector<unsigned long>{1, 5, 9}));
}

void a_tombstone_holds_a_place_and_no_address_space()
{
	Window window;
	for (unsigned long generation = 1; generation <= limit; ++generation)
		admit(window, generation);
	const auto ids = window.owners.size();
	/* 1 is destroyed: it lets go of its id and keeps its place. */
	retire(window, 1, true);
	require(window.generations.count(1) == 0 &&
		(window.order == std::vector<unsigned long>{1, 2, 3}));
	require(window.free_ids.size() == 1);

	/* A fourth address space takes the freed id, and the window, now four
	 * long, retires the tombstone with no address space to let go of. */
	auto &fourth = admit(window, 4);
	require(fourth.id == 1 && window.owners.size() == ids);
	std::vector<unsigned long> retired;
	window.trim([&](unsigned long generation) {
		retired.push_back(generation);
		retire(window, generation);
	});
	require((retired == std::vector<unsigned long>{1}));
	require((window.order == std::vector<unsigned long>{2, 3, 4}));
	require(window.generations.size() == limit);
	/* Retiring the tombstone freed no id. */
	require(window.free_ids.empty());
}

void clearing_forgets_everything_but_the_id_zero_slot()
{
	Window window;
	auto &state = admit(window, 1);
	admit(window, 2);
	retire(window, 2);
	window.current = &state;
	window.bind(state);
	window.clear();
	require(window.generations.empty() && window.order.empty() &&
		window.free_ids.empty());
	require(window.owners.size() == 1 && window.owners[0] == nullptr);
	require(window.current == nullptr &&
		window.current_owner == cache::no_owner &&
		window.current_epoch == 0);
	require(admit(window, 3).id == 1);
}

} // namespace

int main()
{
	ids_count_from_one_and_are_recycled_last_in_first_out();
	the_last_id_a_tag_can_carry_is_handed_out();
	binding_names_the_owner_and_the_epoch_a_lookup_compares();
	an_epoch_wrap_clears_the_validations_it_would_repeat();
	the_order_keeps_the_least_recently_current_first();
	a_tombstone_holds_a_place_and_no_address_space();
	clearing_forgets_everything_but_the_id_zero_slot();
	return 0;
}
