// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace uml::cpu::gadget_cache {

/* A translation cache slot's tag holds the translation's key in its low 48
 * bits, under the id of the address space that owns it. */
inline constexpr unsigned tag_owner_shift = 48;
/* The ids tags can carry, and the owner bits that match none of them while no
 * address space is running. */
inline constexpr std::size_t max_owner_id = 0xfffeU;
inline constexpr std::uint64_t no_owner = std::uint64_t{0xffffU}
	<< tag_owner_shift;
/* The most code changes a slot's epoch can count. */
inline constexpr std::uint16_t max_epoch = 0xffffU;

/* What a cache keeps about one address space it holds: its translation
 * `Space`, which the cache shares with the other executors running the
 * address space or keeps to itself, and the rest of what the window and
 * the cache's slots need of it. An engine derives from this to add what is
 * its own. */
template <typename Space, typename Number>
struct GenerationState {
	GenerationState() = default;
	GenerationState(const GenerationState &) = delete;
	GenerationState &operator=(const GenerationState &) = delete;

	Space *space{};
	/* The code-change sequence the cache last answered for the address
	 * space. */
	std::uint64_t changes{};
	/* The guest's number for the address space, and the cache's own for
	 * it: the id a slot's tag carries, from one up, recycled once the
	 * address space is retired and its slots released. */
	Number value{};
	std::uint16_t id{};
	/* How many times the address space's code is known to have changed, as
	 * far as a slot's validation can tell: a slot whose epoch equals this
	 * one has matched guest memory since the last change. It runs from one
	 * to its maximum and starts over after a sweep of the slots that carry
	 * it, so a slot's epoch is never mistaken for the current one. */
	std::uint16_t epoch{1};
	/* Slots this address space has stored into, as flat slot indices. It
	 * may name a slot another address space has since taken, and may name
	 * one twice, so retirement checks every entry; it can only *miss* a
	 * slot by having overflowed, which `slots_complete` records and
	 * retirement answers with the whole sweep. */
	std::vector<std::uint32_t> slots;
	bool slots_complete{true};
};

/* The address spaces ("generations") a gadget cache holds translations for,
 * which one is running, and the order that bounds them.
 *
 * Translations for several address spaces share one cache, so each slot's
 * tag names its owner by an id (`id`), from one up, which the cache
 * hands out when it first sees the address space and takes back when it
 * retires it. `owners` finds the state of an id, with id zero never used, so
 * a slot with no owner, and a tag made while no address space is running,
 * match no lookup. `free_ids` holds the ids of the retired.
 *
 * What a lookup reads of the running address space sits here by value,
 * `current_owner` and `current_epoch`, rather than behind `current`: the
 * owner bits a tag is made with, and the epoch a slot must carry to be
 * current. An address space's epoch counts the code changes seen in it as
 * far as a slot's validation can tell, from one to `max_epoch`.
 *
 * `order` lists the address spaces least recently current first. Only
 * `Limit` of them stay resident: a switch to a new one retires the least
 * recently current beyond that (trim()). An address space the MMU destroyed
 * may keep its place in the order without being held, a tombstone that
 * retiring only drops.
 *
 * `State` is the engine's record of one address space, a GenerationState or
 * a type derived from it, kept in `generations` and so at a stable address.
 * Retiring an address space, which also releases its slots and its
 * translation space, is the engine's: it calls reserve_id() before it
 * changes anything, and leave() once the rest is done.
 */
template <typename Number, typename State, std::size_t Limit>
struct GenerationWindow final {
	static_assert(Limit != 0, "a window holds at least one address space");
	static_assert(Limit + 2U < max_owner_id,
		"every resident address space has an id, and so does the one "
		"being retired");

	std::unordered_map<Number, State> generations;
	/* The running address space's state, null until it is first looked up
	 * and again after it has been retired. */
	State *current{};
	std::vector<Number> order;
	Number current_generation{};
	std::uint64_t current_owner{no_owner};
	std::uint16_t current_epoch{};
	std::vector<State *> owners{nullptr};
	std::vector<std::uint16_t> free_ids;

	/* Give an address space the id its slots' tags will carry. */
	void enroll(State &state, Number generation)
	{
		state.value = generation;
		if (!free_ids.empty()) {
			state.id = free_ids.back();
			free_ids.pop_back();
		} else {
			assert(owners.size() <= max_owner_id);
			state.id = static_cast<std::uint16_t>(owners.size());
			owners.push_back(nullptr);
		}
		owners[state.id] = &state;
	}

	/* Make an enrolled address space the one lookups are made in. */
	void bind(const State &state) noexcept
	{
		current_owner = std::uint64_t{state.id} << tag_owner_shift;
		current_epoch = state.epoch;
	}

	/* No address space is running: the owner bits name none, so no lookup
	 * matches. */
	void unbind() noexcept
	{
		current = nullptr;
		current_owner = no_owner;
		current_epoch = 0;
	}

	/* A code change was seen in `state`'s address space: every validation
	 * its slots carry is out of date. The epoch advances, and when it would
	 * pass what a slot can hold `forget_epochs(state)` first clears the
	 * epoch of each slot that carries one, so that no old validation is ever
	 * equal to a new epoch. It cannot fail. */
	template <typename ForgetEpochs>
	void advance_epoch(State &state, ForgetEpochs &&forget_epochs) noexcept
	{
		if (state.epoch == max_epoch) {
			forget_epochs(state);
			state.epoch = 0;
		}
		++state.epoch;
		if (&state == current)
			current_epoch = state.epoch;
	}

	/* Make `generation` the most recently current. Inlined into the
	 * engine's own touch(), which is the function it was. */
	[[gnu::always_inline]] void touch(Number generation)
	{
		forget_place(generation);
		order.push_back(generation);
	}

	/* Drop a generation's place in the order. */
	void forget_place(Number generation)
	{
		const auto found = std::find(order.begin(), order.end(), generation);
		if (found != order.end())
			order.erase(found);
	}

	/* Retire, with `retire(generation)`, the least recently current beyond
	 * the limit. `retire` takes the number by value, and drops its place in
	 * the order. */
	template <typename Retire>
	void trim(Retire &&retire)
	{
		while (order.size() > Limit)
			retire(order.front());
	}

	/* Make room for retiring an address space, which is the one step of it
	 * that can fail: do this before anything is changed, so that a failure
	 * leaves the address space held and retirable again. */
	void reserve_id()
	{
		free_ids.reserve(free_ids.size() + 1);
	}

	/* Take a retired address space out: its id is free again and, if it was
	 * the running one, nothing is. Its place in the order is the caller's to
	 * drop (forget_place()), or to keep as a tombstone. After reserve_id().
	 * Inlined into the engine's retirement, which has read the id already. */
	[[gnu::always_inline]] void leave(
		typename std::unordered_map<Number, State>::iterator owner,
		const std::uint16_t &id)
	{
		assert(owner->second.id == id);
		if (current == &owner->second)
			unbind();
		owners[id] = nullptr;
		free_ids.push_back(id);
		generations.erase(owner);
	}

	/* Forget every address space, once the caller has let go of them. */
	void clear() noexcept
	{
		order.clear();
		generations.clear();
		owners.assign(1, nullptr);
		free_ids.clear();
		unbind();
	}
};

} // namespace uml::cpu::gadget_cache
