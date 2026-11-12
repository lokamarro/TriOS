// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "program.hpp"

#include "cpu/gadget_body_arena.hpp"
#include "cpu/gadget_shared_space.hpp"
#include "memory/guest_address_space.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

namespace uml::cpu::x86_64::gadgets {

/* The page stamps a translation was last validated against, as a validation
 * compares them. */
struct ProgramCodeStamps final {
	std::array<::uml::memory::CodePageStamp, 2> pages{};
	std::uint8_t count{};
	bool complete{};

	friend bool operator==(
		const ProgramCodeStamps &, const ProgramCodeStamps &) = default;
};

/* One cached translation, allocated in its address space's arena apart from
 * the slots that index it.
 *
 * A way exists whether or not it holds a translation, so what belongs to one
 * translation is kept here and the slot holds three words and a few flags.
 * The body is the arena allocation [Body][guest bounds][stream words]: the
 * stream is the one the engine runs, and the two words before it are the
 * guest range its code was read from, which a chained block reads at
 * [stream, #-16] (program.hpp). The saved source is a second, cold
 * allocation in the address space's source arena, so that streams stay dense.
 * A body's program is a view of those, and the body is the only owner of
 * what a replay decoded.
 *
 * Every field below is written before the body is published, except the ones
 * that say so: they move with validation, with the lists the body is in and
 * with the slots that hold it. Executors that share an address space share its
 * bodies, so those are atomic, or read and written as a pair.
 */
struct alignas(16) Body final {
	explicit Body(Program &&view) noexcept : program{std::move(view)} {}

	/* The words a body takes in the arena for a stream of `words`. */
	[[nodiscard]] static std::size_t allocation_bytes(
		std::size_t words) noexcept
	{
		return sizeof(Body) + Program::stream_bytes(words);
	}

	[[nodiscard]] std::size_t allocation_bytes() const noexcept
	{
		return allocation_bytes(program.native_stream().size());
	}

	/* The stream and its header sit right behind the body. */
	[[nodiscard]] std::uintptr_t *stream_allocation() noexcept
	{
		return reinterpret_cast<std::uintptr_t *>(this + 1);
	}

	/* The stamps a validation reads and records. Each is one aligned pair,
	 * so a reader sees a stamp some validation recorded, never half of two;
	 * a stamp that is older than the bytes only costs a comparison of them
	 * later. */
	[[nodiscard]] ProgramCodeStamps stamps() const noexcept
	{
		ProgramCodeStamps result;
		for (std::size_t index = 0; index < code_pages.size(); ++index)
			::uml::cpu::gadget_cache::load_pair(&code_pages[index],
				result.pages[index].identity,
				result.pages[index].version);
		const auto state = std::atomic_ref<std::uint8_t>{
			const_cast<std::uint8_t &>(stamp_state)}.load(
				std::memory_order_relaxed);
		result.count = state & (stamps_complete_bit - 1U);
		result.complete = (state & stamps_complete_bit) != 0U;
		return result;
	}
	void set_stamps(const ProgramCodeStamps &stamps) noexcept
	{
		for (std::size_t index = 0; index < code_pages.size(); ++index)
			::uml::cpu::gadget_cache::store_pair(&code_pages[index],
				stamps.pages[index].identity,
				stamps.pages[index].version);
		std::atomic_ref<std::uint8_t>{stamp_state}.store(
			static_cast<std::uint8_t>(stamps.count |
				(stamps.complete ? stamps_complete_bit : 0U)),
			std::memory_order_relaxed);
	}

	Program program;
	/* The first and last page of the guest range, as the lists of bodies
	 * per page thread them: entry 0 is the page the first byte is on and
	 * entry 1 the page the last byte is on. The lists belong to the address
	 * space, which guards them. */
	std::array<Body *, 2> page_previous{};
	std::array<Body *, 2> page_next{};
	/* The page stamps the translation was last validated against, as two
	 * pairs (stamps()). */
	alignas(16) std::array<::uml::memory::CodePageStamp, 2> code_pages{};
	/* How many slots hold the body, counted under the lock of its link
	 * record. */
	std::atomic<std::uint32_t> holders{};
	/* Set once a store has landed over the code the body was read from, so
	 * that no link is armed to it. */
	std::atomic<bool> stale{};
	/* The instruction limit it was built under, as slots compare it
	 * (kept_instruction_limit). */
	std::uint8_t instruction_limit{};
	/* Which of `page_previous` and `page_next` are in a page's list; never
	 * set before the body is published. */
	std::uint8_t linked_pages{};
	/* How many of `code_pages` are stamps, in two bits, and above them
	 * whether they cover the whole block; zero while there are none. */
	std::uint8_t stamp_state{};

	static constexpr std::uint8_t stamps_complete_bit = 4;
};
static_assert(sizeof(Body) % 16 == 0 && alignof(Body) == 16,
	"the guest bounds that follow a body start on a sixteen-byte boundary");
static_assert(sizeof(Body) == 128);
static_assert(offsetof(Body, code_pages) % 16 == 0,
	"a stamp is one aligned pair");

} // namespace uml::cpu::x86_64::gadgets
