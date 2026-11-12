// SPDX-License-Identifier: GPL-2.0-or-later
#include "executor.hpp"

#include "cpu/gadget_cache_budget.hpp"
#include "cpu/gadget_cache_geometry.hpp"
#include "cpu/gadget_generation_window.hpp"
#if UML_GADGET_CACHE_CENSUS
#include "cpu/gadget_cache_census.hpp"
#endif
#include "cpu/x86_64/code_page_registry.hpp"

#include "native_entry.hpp"
#include "native_symbols.hpp"
#include "program_cache_index.hpp"
#include "translation_body.hpp"
#include "translation_space.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#if UML_GADGET_CACHE_CENSUS
#include <cstdio>
#endif
#include <cstring>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <malloc/malloc.h>

namespace uml::cpu::x86_64::gadgets {
#if UML_GADGET_CACHE_CENSUS
namespace census = ::uml::cpu::gadget_cache::census;
#endif
namespace {

static_assert(DARWIN_UML_GUEST_PAGE_SIZE == 4096,
	"x86-64 gadgets are a 4 KiB guest product");
inline constexpr std::size_t tlb_entry_count = native_tlb_entry_count;
inline constexpr std::size_t program_cache_ways = gadget_cache::ways;
inline constexpr std::size_t program_cache_slots =
	gadget_cache::slots;
inline constexpr std::size_t program_cache_sets =
	program_cache_slots / program_cache_ways;
inline constexpr std::size_t program_cache_slab_slots = gadget_cache::slab_slots;
inline constexpr std::size_t program_cache_slab_count =
	program_cache_slots / program_cache_slab_slots;
inline constexpr std::size_t resident_generation_limit =
	gadget_cache::resident_generations;
inline constexpr std::size_t maximum_cached_source_bytes = 64U * 15U;
static_assert(maximum_cached_source_bytes <= DARWIN_UML_GUEST_PAGE_SIZE,
	"one cached x86 program may span at most two code pages");
static_assert((program_cache_slots & (program_cache_slots - 1U)) == 0,
	"x86-64 gadget cache slots must be a power of two");
static_assert(program_cache_slots >= program_cache_slab_slots * 4U &&
	program_cache_slots % program_cache_slab_slots == 0,
	"x86-64 gadget cache must contain at least four complete slabs");
static_assert(program_cache_sets * program_cache_ways == program_cache_slots);
static_assert(resident_generation_limit != 0,
	"x86-64 gadget cache must retain at least one address space");
static_assert(std::is_nothrow_move_constructible_v<Program>);

using gadget_cache::acquire_announcement;
using gadget_cache::announce_entry;
using gadget_cache::announce_exit;
using gadget_cache::release_announcement;
using Announcement = gadget_cache::ExecutorAnnouncement;

/* Executors running one address space share its translations unless
 * DARWIN_UML_GADGET_SHARED_BODIES=0 keeps every executor's to itself: the
 * bodies, link records, indirect sites and target tables are the address
 * space's, held once, and each executor keeps only its own slots. A shared
 * space bounds the freed arena it accumulates under process churn by renewal
 * (Space::renewal_due), which is what lets sharing be the default. Read as
 * each executor is made. */
[[nodiscard]] bool shared_bodies_enabled() noexcept
{
	const char *value = std::getenv("DARWIN_UML_GADGET_SHARED_BODIES");
	return value == nullptr || std::strcmp(value, "0") != 0;
}

struct ProgramCacheLocation final {
	std::size_t set;
	std::size_t slab;
	std::size_t offset;
};

[[nodiscard]] ProgramCacheLocation locate_program(
	std::uint64_t generation, std::uint64_t address,
	ExecutionMode mode) noexcept
{
	const auto set = program_cache_set(
		generation, address, mode, program_cache_sets);
	const auto first = set * program_cache_ways;
	return {
		.set = set,
		.slab = first / program_cache_slab_slots,
		.offset = first % program_cache_slab_slots,
	};
}

/* A slot's tag holds the translation's guest address and execution mode in its
 * low 48 bits, under the id of the address space that owns it. A long64
 * address is below the guest address limit (2^47) and a compat32 address
 * below 2^32, and the mode takes the bit above the first. An address with no
 * such form names no fetchable code, so it tags to a value no slot holds. */
inline constexpr std::uint64_t tag_address_mask =
	(std::uint64_t{1} << 47) - 1U;
inline constexpr std::uint64_t compat32_flag = std::uint64_t{1} << 47;
inline constexpr std::uint64_t unmatchable_tag = ~std::uint64_t{};

[[nodiscard]] constexpr std::uint64_t compact_tag_address(
	std::uint64_t address, ExecutionMode mode) noexcept
{
	if (mode == ExecutionMode::compat32)
		return address > 0xffff'ffffU ? unmatchable_tag :
			address | compat32_flag;
	return address > tag_address_mask ? unmatchable_tag : address;
}

static_assert(compact_tag_address(0x1000, ExecutionMode::long64) !=
	compact_tag_address(0x1000, ExecutionMode::compat32));
static_assert(compact_tag_address(tag_address_mask, ExecutionMode::long64) ==
	tag_address_mask);
static_assert(compact_tag_address(tag_address_mask + 1U,
	ExecutionMode::long64) == unmatchable_tag);
static_assert(compact_tag_address(0x1'0000'0000ULL,
	ExecutionMode::compat32) == unmatchable_tag);

enum class ProgramSourceComparison {
	match,
	mismatch,
	fault,
};

/* A page stamp certifies only the bytes read after it was taken: a store that
 * lands later moves the stamp, and one that landed earlier is already visible
 * in the bytes. Every caller therefore takes the stamp before the source it
 * stands for, never after. */
[[nodiscard]] ProgramCodeStamps capture_program_code_stamps(
	::uml::memory::GuestAddressSpace &memory, const Program &program,
	void *execution_context) noexcept
{
	ProgramCodeStamps result;
	auto address = program.address();
	auto remaining = program.byte_count();
	auto previous_page = ~std::uint64_t{};
	while (remaining != 0) {
		const auto page = address &
			~std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE - 1U};
		if (page != previous_page) {
			if (result.count == result.pages.size() ||
			    !memory.code_page_stamp(address, execution_context,
				    result.pages[result.count]))
				return {};
			++result.count;
			previous_page = page;
		}
		const auto count = std::min<std::size_t>(remaining,
			DARWIN_UML_GUEST_PAGE_SIZE -
				(address & (DARWIN_UML_GUEST_PAGE_SIZE - 1U)));
		remaining -= count;
		address = advance_instruction_pointer(
			program.execution_mode(), address, count);
	}
	result.complete = result.count != 0;
	return result;
}

/* Boot profiling shows that more than 90 percent of stale-block validation
 * spans are at most 32 bytes. Keep those comparisons in the caller instead of
 * paying the platform memcmp entry cost for one to four ordinary word loads. */
[[nodiscard]] bool equal_small_source(const std::byte *current,
	const std::byte *expected, std::size_t size) noexcept
{
	while (size >= sizeof(std::uint64_t)) {
		std::uint64_t current_word{};
		std::uint64_t expected_word{};
		std::memcpy(&current_word, current, sizeof(current_word));
		std::memcpy(&expected_word, expected, sizeof(expected_word));
		if (current_word != expected_word)
			return false;
		current += sizeof(current_word);
		expected += sizeof(expected_word);
		size -= sizeof(current_word);
	}
	if (size >= sizeof(std::uint32_t)) {
		std::uint32_t current_word{};
		std::uint32_t expected_word{};
		std::memcpy(&current_word, current, sizeof(current_word));
		std::memcpy(&expected_word, expected, sizeof(expected_word));
		if (current_word != expected_word)
			return false;
		current += sizeof(current_word);
		expected += sizeof(expected_word);
		size -= sizeof(current_word);
	}
	if (size >= sizeof(std::uint16_t)) {
		std::uint16_t current_word{};
		std::uint16_t expected_word{};
		std::memcpy(&current_word, current, sizeof(current_word));
		std::memcpy(&expected_word, expected, sizeof(expected_word));
		if (current_word != expected_word)
			return false;
		current += sizeof(current_word);
		expected += sizeof(expected_word);
		size -= sizeof(current_word);
	}
	return size == 0 || *current == *expected;
}

[[nodiscard]] bool equal_source(const std::byte *current,
	const std::byte *expected, std::size_t size) noexcept
{
	return size <= 32 ? equal_small_source(current, expected, size) :
		std::memcmp(current, expected, size) == 0;
}

[[nodiscard]] ProgramSourceComparison compare_program_source(
	::uml::memory::GuestAddressSpace &memory, const Program &program,
	std::span<const std::byte> expected)
{
	if (expected.size() != program.byte_count())
		return ProgramSourceComparison::mismatch;

	std::array<std::byte, maximum_cached_source_bytes> fetched;
	auto address = program.address();
	std::size_t completed = 0;
	while (completed < expected.size()) {
		auto count = expected.size() - completed;
		const auto last = program.execution_mode() == ExecutionMode::compat32 ?
			std::uint64_t{0xffff'ffffU} : ~std::uint64_t{};
		const auto before_wrap = last - address;
		if (count - 1U > before_wrap)
			count = static_cast<std::size_t>(before_wrap) + 1U;
		count = std::min<std::size_t>(count,
			DARWIN_UML_GUEST_PAGE_SIZE -
				(address & (DARWIN_UML_GUEST_PAGE_SIZE - 1U)));

		const auto saved = expected.subspan(completed, count);
		if (const auto *mapped = memory.atomic_pointer(address, count,
				::uml::memory::MemoryAccess::execute);
			mapped != nullptr) {
			if (!equal_source(mapped, saved.data(), count))
				return ProgramSourceComparison::mismatch;
		} else {
			auto copied = std::span{fetched}.first(count);
			if (!memory.fetch(address, copied))
				return ProgramSourceComparison::fault;
			if (!equal_source(copied.data(), saved.data(), count))
				return ProgramSourceComparison::mismatch;
		}

		completed += count;
		address = advance_instruction_pointer(
			program.execution_mode(), address, count);
	}
	return ProgramSourceComparison::match;
}

/* The registries of every address-space generation this executor holds
 * translations for, and its participation in the one it is running. A page
 * is counted once per executor that caches code on it, so a translation
 * retired here is subtracted here.
 */
struct CodePageRegistries final {
	std::unordered_map<std::uint64_t, std::shared_ptr<CodePageRegistry>> held;
	std::shared_ptr<CodePageRegistry> current;
	std::uint64_t current_generation{};
	CodePageRegistry::Participant participant;

	~CodePageRegistries() { unbind(); }

	[[nodiscard]] CodePageRegistry &of(std::uint64_t generation)
	{
		auto [entry, inserted] = held.try_emplace(generation);
		if (inserted)
			entry->second = CodePageRegistry::acquire(generation);
		return *entry->second;
	}

	void bind(std::uint64_t generation)
	{
		if (current && current_generation == generation)
			return;
		unbind();
		auto &registry = of(generation);
		registry.join(participant);
		current = held.find(generation)->second;
		current_generation = generation;
	}

	void unbind() noexcept
	{
		if (current)
			current->leave(participant);
		current.reset();
	}

	void release(std::uint64_t generation) noexcept
	{
		if (current && current_generation == generation)
			return;
		held.erase(generation);
	}

	void drop_all() noexcept
	{
		unbind();
		held.clear();
	}
};

/* Holds a translation's possible pages in the registry while their bytes are
 * read, so any store another executor has already translated for them lands
 * first. A cached program is at most maximum_cached_source_bytes long, so a
 * block beginning at one address can span that page and the page its last
 * possible byte falls in, and no other. The cache's own index keeps whichever
 * pages the program turns out to cover. */
class CodePageHold final {
public:
	CodePageHold(CodePageRegistries *registries, std::uint64_t address,
		ExecutionMode mode) noexcept
	{
		pages_[count_++] = address &
			~std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE - 1U};
		/* Advancing in the block's own mode keeps the bound right where a
		 * compat32 block reaches its last byte past the 4 GiB wrap. */
		const auto last = advance_instruction_pointer(mode, address,
			maximum_cached_source_bytes - 1U) &
			~std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE - 1U};
		if (last != pages_[0])
			pages_[count_++] = last;
		if (registries == nullptr || !registries->current)
			return;
		registry_ = registries->current.get();
		for (std::size_t index = 0; index < count_; ++index)
			registry_->add(pages_[index], &registries->participant);
	}
	~CodePageHold()
	{
		if (registry_ == nullptr)
			return;
		for (std::size_t index = 0; index < count_; ++index)
			registry_->remove(pages_[index]);
	}
	CodePageHold(const CodePageHold &) = delete;
	CodePageHold &operator=(const CodePageHold &) = delete;

	[[nodiscard]] std::span<const std::uint64_t> pages() const noexcept
	{
		return {pages_.data(), count_};
	}

private:
	std::array<std::uint64_t, 2> pages_{};
	std::size_t count_{};
	CodePageRegistry *registry_{};
};

/* The stamp of every page a block can span, read before the decoder reads a
 * byte of any of them. A page whose stamp the address space cannot produce is
 * recorded as absent rather than failing the whole capture, because a block
 * near the end of a text page holds the page after it without ever reaching
 * it, and that page need not be code at all. */
struct HeldCodeStamps final {
	std::array<::uml::memory::CodePageStamp, 2> stamps{};
	std::array<bool, 2> present{};
};

[[nodiscard]] HeldCodeStamps capture_held_code_stamps(
	::uml::memory::GuestAddressSpace &memory,
	std::span<const std::uint64_t> pages, void *execution_context) noexcept
{
	HeldCodeStamps result;
	for (std::size_t index = 0; index < pages.size(); ++index)
		result.present[index] = memory.code_page_stamp(
			pages[index], execution_context, result.stamps[index]);
	return result;
}

/* The stamps a fresh translation records, taken from the read made before the
 * decoder ran, for the pages the program turned out to cover. They certify the
 * program because the source the slot saves is the decoder's own bytes, read
 * after them: one observation of guest memory ("Keeping a cached translation"
 * in README.md).
 *
 * `nullopt` means a page this program covers was not held while its bytes were
 * read, which the block's byte bound makes impossible and which no later check
 * could recover; such a translation is used once and not cached. An address
 * space that cannot stamp a page gives no stamps at all, and the program keeps
 * the exact byte validation that address space already relies on.
 */
[[nodiscard]] std::optional<ProgramCodeStamps> program_code_stamps_from_hold(
	const Program &program, std::span<const std::uint64_t> held_pages,
	const HeldCodeStamps &held) noexcept
{
	ProgramCodeStamps result;
	auto address = program.address();
	auto remaining = program.byte_count();
	auto previous_page = ~std::uint64_t{};
	bool stamped = true;
	while (remaining != 0) {
		const auto page = address &
			~std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE - 1U};
		if (page != previous_page) {
			std::size_t held_index = held_pages.size();
			for (std::size_t index = 0; index < held_pages.size(); ++index)
				if (held_pages[index] == page)
					held_index = index;
			if (held_index == held_pages.size() ||
			    result.count == result.pages.size())
				return std::nullopt;
			if (held.present[held_index])
				result.pages[result.count] =
					held.stamps[held_index];
			else
				stamped = false;
			++result.count;
			previous_page = page;
		}
		const auto count = std::min<std::size_t>(remaining,
			DARWIN_UML_GUEST_PAGE_SIZE -
				(address & (DARWIN_UML_GUEST_PAGE_SIZE - 1U)));
		remaining -= count;
		address = advance_instruction_pointer(
			program.execution_mode(), address, count);
	}
	if (!stamped)
		return ProgramCodeStamps{};
	result.complete = result.count != 0;
	return result;
}

class ProgramCache final {
private:
	struct Slot;

	/* One way of a set. A way exists whether or not it holds a translation,
	 * so every byte here is paid for by the whole cache: at 262,144 slots
	 * every eight bytes is two mebibytes per executor, which is why a slot
	 * is four words and the rest of what a translation needs is in its
	 * Body. */
	struct Slot final {
		/* What a lookup compares, so rejecting a way is one load and one
		 * comparison: the translation's address and execution mode
		 * (compact_tag_address) under the id of the address space that
		 * owns it. Zero while the slot is empty: ids start at one, so no
		 * occupied slot's tag is zero and no lookup's is, either. It is
		 * written after the rest of the slot, so a slot is never seen
		 * occupied half made. */
		std::uint64_t tag{};
		/* The translation this slot indexes, in its address space's arena.
		 * A slot is occupied exactly while this is non-null. */
		Body *body{};
		/* Two fields sharing a word, a pointer below the guest's 48-bit
		 * address-space limit and sixteen bits above it: the link record
		 * the slot's block publishes through, memoised so reactivation
		 * need not look it up again, and the epoch of the owner's code
		 * (Generation::epoch) when the block was last known to match
		 * guest memory, zero for never. */
		std::uint64_t memo{};
		/* The slot's flat index, which names it in successor sets; fixed
		 * when its slab is made. */
		std::uint32_t index{};
		std::uint8_t instruction_limit{};
		bool terminal{};

		static constexpr unsigned epoch_shift = 48;
		static constexpr std::uint64_t link_mask =
			(std::uint64_t{1} << epoch_shift) - 1U;

		[[nodiscard]] bool resident() const noexcept { return tag != 0; }
		[[nodiscard]] std::uint16_t owner() const noexcept
		{
			return static_cast<std::uint16_t>(
				tag >> gadget_cache::tag_owner_shift);
		}
		[[nodiscard]] ExecutionMode mode() const noexcept
		{
			return (tag & compat32_flag) != 0U ?
				ExecutionMode::compat32 : ExecutionMode::long64;
		}
		[[nodiscard]] std::uint64_t address() const noexcept
		{
			return tag & (compat32_flag - 1U);
		}
		[[nodiscard]] LinkRecord *link() const noexcept
		{
			return reinterpret_cast<LinkRecord *>(
				static_cast<std::uintptr_t>(memo & link_mask));
		}
		void set_link(LinkRecord *record) noexcept
		{
			const auto address =
				reinterpret_cast<std::uintptr_t>(record);
			assert((address & ~link_mask) == 0U);
			memo = (memo & ~link_mask) | address;
		}
		[[nodiscard]] std::uint16_t epoch() const noexcept
		{
			return static_cast<std::uint16_t>(memo >> epoch_shift);
		}
		void set_epoch(std::uint16_t value) noexcept
		{
			memo = (memo & link_mask) |
				(std::uint64_t{value} << epoch_shift);
		}
	};
	static_assert(ProgramCodeStamps{}.pages.size() <
			Body::stamps_complete_bit,
		"a stamp count fits the bits below the completeness bit");
	static_assert(sizeof(Slot) == 32,
		"a way costs its slot whether or not it holds a translation");
	static_assert(program_cache_slots < (std::size_t{1} << 31),
		"a slot index fits 32 bits");

	/* The common monomorphic and bimorphic successors bypass the main
	 * set probe. A successor is named by its slot's index plus one, so
	 * zero is empty, and a slot's own set sits at that same index; bit 31 of
	 * the first entry is the way the next successor replaces. Slots and
	 * their companion sets live in lazy slabs for the executor's lifetime;
	 * tag checks make stale entries inert. */
	struct SuccessorSet final {
		static constexpr std::uint32_t replacement_bit = 1U << 31;

		std::array<std::uint32_t, 2> entries{};

		[[nodiscard]] std::uint32_t entry(std::size_t way) const noexcept
		{
			return entries[way] & ~replacement_bit;
		}
		[[nodiscard]] std::size_t next_replacement() const noexcept
		{
			return (entries[0] & replacement_bit) != 0U ? 1U : 0U;
		}
		void set(std::size_t way, std::uint32_t id,
			std::size_t next) noexcept
		{
			entries[way] = (entries[way] & replacement_bit) | id;
			entries[0] = (entries[0] & ~replacement_bit) |
				(next != 0U ? replacement_bit : 0U);
		}
	};
	static_assert(sizeof(SuccessorSet) == 8);

	struct Slab final {
		std::array<Slot, program_cache_slab_slots> slots;
		std::array<SuccessorSet, program_cache_slab_slots> successors;
	};

	/* As in the arm64 cache, each address space owns the list of slots it has
	 * populated. Retiring the least-recently-current generation therefore
	 * scales with that generation's footprint rather than the whole cache.
	 * Its translations, link records and target link tables are its space's
	 * (translation_space.hpp), which this cache shares with the other
	 * executors running the address space, or keeps to itself
	 * (DARWIN_UML_GADGET_SHARED_BODIES). The space charges the account it
	 * keeps in the process's gadget-cache budget (gadget_cache_budget.hpp),
	 * which may ask the cache to let go of it (leave_shedding()).
	 */
	struct Generation final
		: gadget_cache::GenerationState<Space, std::uint64_t> {
#if UML_GADGET_CACHE_CENSUS
		/* When the cache last left this address space, in switches and
		 * in seconds. */
		std::uint64_t census_switch{};
		double census_seconds{};
#endif
	};

	using Window = gadget_cache::GenerationWindow<std::uint64_t, Generation,
		resident_generation_limit>;

public:
	/* `announcement` is what this cache's executor says about itself for the
	 * bodies other executors free, and `share` whether the cache shares its
	 * address spaces with the other executors running them. */
	explicit ProgramCache(
		const Announcement *announcement = nullptr, bool share = false)
		: announcement_{announcement},
		  share_{share && announcement != nullptr}
	{}

	ProgramCache(const ProgramCache &) = delete;
	ProgramCache &operator=(const ProgramCache &) = delete;

	/* The exceptions this cache's blocks are built for: the part of an
	 * address space's key that is not the address space. Set before the
	 * first prepare. */
	void set_checked(unsigned checked) noexcept
	{
		checked_ = static_cast<std::uint8_t>(checked);
	}

	void invalidate() noexcept
	{
#if UML_GADGET_CACHE_CENSUS
		for (auto &slab : slabs_)
			if (slab)
				for (auto &slot : slab->slots)
					if (slot.resident())
						census_depart(slot,
							census::Departure::retired);
#endif
		/* Every slot lets go of its body, whose address space may be
		 * shared with executors still running it. */
		for (auto &slab : slabs_)
			if (slab)
				for (auto &slot : slab->slots) {
					if (slot.body != nullptr)
						release_body(slot, false);
					slot.tag = 0;
					slot.memo = 0;
					slot.body = nullptr;
				}
		for (auto &[generation, state] : window_.generations) {
			renew_if_due(*state.space);
			state.space->reclaim(announcement_);
			SpaceRegistry::instance().detach(state.space,
				announcement_);
			if (registries_ != nullptr)
				registries_->release(generation);
		}
		window_.clear();
		preceding_ = nullptr;
	}

	void prepare(std::uint64_t generation, std::uint64_t changes)
	{
		if (window_.current_generation != generation)
			preceding_ = nullptr;
#if UML_GADGET_CACHE_CENSUS
		/* The address space being left was in use until now. */
		if (generation != window_.current_generation &&
		    window_.current != nullptr) {
			++switches_;
			window_.current->census_switch = switches_;
			window_.current->census_seconds =
				census::elapsed_seconds();
		}
#endif
		window_.current_generation = generation;
		bool inserted = false;
		auto &state = generation_for(generation, changes, inserted);
		window_.current = &state;
		window_.bind(state);
		touch(generation);
		if (inserted) {
			state.changes = changes;
			/* A space another executor made may be behind the code
			 * this one is at. */
			state.space->observe(changes, code_change_reader_,
				code_change_context_);
			window_.trim([this](std::uint64_t retiring) {
#if UML_GADGET_CACHE_CENSUS
				if (!window_.generations.contains(retiring))
					++lru_tomb_;
#endif
				retire(retiring);
			});
			return;
		}
		if (state.changes == changes)
			return;
		preceding_ = nullptr;
		state.space->observe(changes, code_change_reader_,
			code_change_context_);
		state.changes = changes;
		advance_epoch(state);
	}

	/* Let go of every address space held here whose generation `live`
	 * reports destroyed, and say whether the one last prepared was among
	 * them. The generation's number keeps its place in the order the
	 * window retires in, a tombstone that holds nothing and that the
	 * least-recently-used rule drops when it reaches it, so the window
	 * keeps the live address spaces it would have kept. An answer `live`
	 * cannot give (negative) keeps the address space. Out of memory part
	 * way, what was not let go of is still held, and the caller asks
	 * again. */
	[[nodiscard]] bool leave_gone(GenerationLiveness live)
	{
		std::array<std::uint64_t, resident_generation_limit + 1U> gone{};
		std::size_t count = 0;
		for (const auto &[generation, state] : window_.generations) {
			if (count == gone.size())
				break;
			if (live(static_cast<unsigned long>(generation)) == 0)
				gone[count++] = generation;
		}
		bool left_current = false;
		for (std::size_t index = 0; index < count; ++index) {
			left_current |=
				gone[index] == window_.current_generation;
#if UML_GADGET_CACHE_CENSUS
			++dead_left_;
#endif
			retire(gone[index], true);
		}
		return left_current;
	}

	/* Let go of every address space held here that the gadget-cache budget
	 * asked to shed, or that another executor closed, as of a destroyed one
	 * but with its place in the window's order, and say whether the one last
	 * prepared was among them. Only when the budget or an executor has
	 * asked since the last call, which two loads answer. Out of memory part
	 * way, what was not let go of is still held and asked for again. */
	[[nodiscard]] bool leave_shedding()
	{
		const bool closed = announcement_ != nullptr &&
			announcement_->renew.load(std::memory_order_relaxed);
		if (!closed && !shed_requested_.load(std::memory_order_relaxed))
			return false;
		shed_requested_.store(false, std::memory_order_relaxed);
		if (closed)
			announcement_->renew.store(false, std::memory_order_relaxed);
		std::array<std::uint64_t, resident_generation_limit + 1U> gone{};
		std::size_t count = 0;
		for (const auto &[generation, state] : window_.generations) {
			if (count == gone.size())
				break;
			if (state.space->closed() || state.space->shedding())
				gone[count++] = generation;
		}
		bool left_current = false;
		try {
			for (std::size_t index = 0; index < count; ++index) {
				left_current |= window_.current_generation ==
					gone[index];
				/* A space the budget asked to shed is closed, for
				 * every other executor holding it to let go of
				 * too. */
				if (const auto held = window_.generations.find(
					    gone[index]);
				    held != window_.generations.end() &&
				    held->second.space->shedding() &&
				    held->second.space->shared()) {
					try {
						SpaceRegistry::instance().close(
							held->second.space);
					} catch (...) {
						/* Still shedding: no executor takes it
						 * up again. */
					}
				}
				retire(gone[index]);
			}
		} catch (...) {
			shed_requested_.store(true, std::memory_order_relaxed);
			throw;
		}
		return left_current;
	}

	void set_code_change_reader(
		ProgramCacheCodeChangeReader reader, void *context) noexcept
	{
		code_change_reader_ = reader;
		code_change_context_ = context;
	}

	/* The write translations this cache retires when a page gains code, and
	 * the query the miss handler uses to record whether a page has any.
	 */
	void set_write_translations(NativeTlbEntry *entries) noexcept
	{
		write_translations_ = entries;
	}

	void set_code_page_registries(CodePageRegistries *registries) noexcept
	{
		registries_ = registries;
	}

	[[nodiscard]] bool page_holds_code(std::uint64_t address) const noexcept
	{
		if (window_.current == nullptr)
			return false;
		return window_.current->space->holds_page(
			address & code_page_mask);
	}

	/* A write made by this executor already names its exact guest range.
	 * Reset only translations whose saved instruction bytes overlap it, and
	 * where translations are shared mark them stale; unrelated links can
	 * remain live when this executor accepts its own publication at the
	 * next branch boundary.
	 */
	void invalidate_code(std::uint64_t address, std::size_t size) noexcept
	{
		if (window_.current == nullptr || size == 0)
			return;
		window_.current->space->note_store(address, size);
	}

	void accept_code_changes(std::uint64_t changes) noexcept
	{
		if (window_.current == nullptr ||
		    window_.current->changes == changes)
			return;
		window_.current->space->accept(changes);
		window_.current->changes = changes;
		advance_epoch(*window_.current);
	}

	[[nodiscard]] const BlockLink *link(
		std::uint64_t address, ExecutionMode mode)
	{
		return current().space->link(address, mode);
	}

	/* The tier an entering block probes. It belongs to the address space, so
	 * retiring a generation retires its tier with it and no entry can name a
	 * record that has been destroyed. */
	[[nodiscard]] const TargetLinkEntry *target_links(
		ExecutionMode mode) noexcept
	{
		return window_.current == nullptr ?
			vacant_target_links().data() :
			window_.current->space->target_links(mode);
	}

	[[nodiscard]] IndirectLink *indirect_link(
		std::uint64_t source, ExecutionMode mode)
	{
		return current().space->indirect_link(source, mode);
	}

	void learn_indirect(IndirectLink &site, std::uint64_t target,
		ExecutionMode mode)
	{
		current().space->learn_indirect(site, target, mode);
	}

	/* The pages a build covers, pinned in the address space's page index
	 * for as long as the guard lives: stores that land on them from before
	 * the build reads a byte until it publishes are counted. */
	class Pinned final {
	public:
		Pinned(Space *space, std::span<const std::uint64_t> pages)
			: space_{space}, pins_{space->pin(pages)}
		{}
		~Pinned()
		{
			if (space_ != nullptr)
				space_->unpin(pins_);
		}
		Pinned(const Pinned &) = delete;
		Pinned &operator=(const Pinned &) = delete;
		Pinned(Pinned &&other) noexcept
			: space_{std::exchange(other.space_, nullptr)},
			  pins_{other.pins_}
		{}
		Pinned &operator=(Pinned &&) = delete;

		[[nodiscard]] const PagePins &pins() const noexcept
		{
			return pins_;
		}

	private:
		Space *space_{};
		PagePins pins_{};
	};

	[[nodiscard]] Pinned pin(std::span<const std::uint64_t> pages)
	{
		return {current().space, pages};
	}

	[[nodiscard]] const Program *find(std::uint64_t address,
		ExecutionMode mode, std::size_t maximum_instructions,
		[[maybe_unused]] std::uint64_t changes,
		std::uint64_t mapping_changes,
		::uml::memory::GuestAddressSpace &memory, void *execution_context,
		ProgramCacheSequenceReader sequence_reader,
		ProgramCacheDiagnostics &diagnostics, bool &cache_hit)
	{
		cache_hit = false;
		/* The slots' validation is kept as an epoch of the address space's
		 * code, which prepare() and accept_code_changes() have been told. */
		assert(window_.current == nullptr ||
		       window_.current->changes == changes);
#if UML_GADGET_CACHE_CENSUS
		if ((history_.lookups() & 4095U) == 0)
			census::snapshot_if_due(schedule_, *this);
#endif
		const auto tag = tag_of(address, mode);
		if (preceding_ != nullptr) {
			for (std::size_t way = 0; way < preceding_->entries.size();
			     ++way) {
				const auto successor = preceding_->entry(way);
				if (successor == 0)
					continue;
				auto &slot = slot_at(successor - 1U);
				if (slot.tag != tag)
					continue;
				const auto *program = use(slot,
					maximum_instructions, mapping_changes,
					memory, execution_context, sequence_reader,
					diagnostics);
				if (program == nullptr) {
					preceding_ = nullptr;
					return nullptr;
				}
				activate(slot, successors_at(successor - 1U));
				++diagnostics.successor_hits;
				cache_hit = true;
				return program;
			}
		}
		const auto location = locate_program(
			window_.current_generation, address, mode);
		auto &slab = slabs_[location.slab];
		if (slab) {
			for (std::size_t way = 0; way < program_cache_ways; ++way) {
				auto &slot = slab->slots[location.offset + way];
				if (slot.tag != tag)
					continue;
				const auto *program = use(slot, maximum_instructions,
					mapping_changes, memory, execution_context,
					sequence_reader, diagnostics);
				if (program == nullptr)
					return nullptr;
				activate(slot, slab->successors[location.offset + way]);
				cache_hit = true;
				return program;
			}
		}
		/* No slot here holds the address, so ask the address space:
		 * another executor may hold its translation. */
		if (share_ && window_.current != nullptr) {
			if (const auto *program = adopt(address, mode,
				    maximum_instructions, mapping_changes, memory,
				    execution_context, sequence_reader,
				    diagnostics)) {
#if UML_GADGET_CACHE_CENSUS
				history_.adopted();
#endif
				cache_hit = true;
				return program;
			}
		}
#if UML_GADGET_CACHE_CENSUS
		history_.missed();
#endif
		return nullptr;
	}

	void cancel_selection() noexcept { preceding_ = nullptr; }

	/* The resident program whose stream holds `word`, which a linked run can
	 * be inside of: a block a link enters is live in the address space, held
	 * by some executor's slot or waiting for the executors that may still be
	 * running it to leave. */
	[[nodiscard]] const Program *holding(const std::uintptr_t *word) const noexcept
	{
		if (window_.current == nullptr)
			return nullptr;
		const auto *body = window_.current->space->owner(word);
		if (body == nullptr)
			return nullptr;
		const auto stream = body->program.native_stream();
		if (word < stream.data() || word >= stream.data() + stream.size())
			return nullptr;
		return &body->program;
	}

	/* Store a built program, or adopt an equal one another executor stored
	 * first, and hold it in a slot. Null, with nothing changed, when a store
	 * landed over the pages the build read, so that the program may be
	 * stale; the caller runs it once. Throws when the memory is not there.
	 */
	[[nodiscard]] const Program *store(const Program &program,
		[[maybe_unused]] std::uint64_t changes,
		std::size_t instruction_limit, ProgramCodeStamps code_stamps,
		const PagePins &pins, ProgramCacheDiagnostics &diagnostics)
	{
		(void)link(program.address(), program.execution_mode());
		/* The address space is registered, and told the code it holds is
		 * as current as the caller's sequence, before the tag naming it is
		 * made. */
		assert(window_.current != nullptr &&
		       window_.current->changes == changes);
		const auto tag = tag_of(program.address(), program.execution_mode());
		/* An address no slot can name is not cached, which the caller
		 * answers by running the program once. */
		if (tag == unmatchable_tag)
			throw std::length_error("no cacheable translation key");

		const auto location = locate_program(window_.current_generation,
			program.address(), program.execution_mode());
		auto &slab = slabs_[location.slab];
		if (!slab) {
			slab = std::make_unique<Slab>();
			for (std::size_t offset = 0; offset < program_cache_slab_slots;
			     ++offset)
				slab->slots[offset].index = static_cast<std::uint32_t>(
					location.slab * program_cache_slab_slots + offset);
		}

		/* The body is made before anything is disturbed, so that running
		 * out of memory here leaves the cache as it was. */
		auto &state = current();
		auto *body = state.space->publish(program,
			kept_instruction_limit(instruction_limit), code_stamps, pins,
			registries_ != nullptr ? &registries_->participant : nullptr);
		if (body == nullptr)
			return nullptr;

#if UML_GADGET_CACHE_CENSUS
		/* A lookup that could not use its translation can leave it
		 * resident, and this store makes a second copy beside it. */
		bool restored = false;
		for (std::size_t way = 0; way < program_cache_ways; ++way)
			if (slab->slots[location.offset + way].tag == tag)
				restored = true;
#endif
		auto &slot = install(location, *slab, tag, state, body,
			kept_instruction_limit(instruction_limit));
		auto &successors = successors_at(slot.index);
		activate_link(slot);
		activate(slot, successors);
#if UML_GADGET_CACHE_CENSUS
		history_.arrive(census_id_, window_.current_generation,
			census_key(slot), census_digest(slot), restored);
#endif
		state.space->reclaim(announcement_);
		renew_if_due(*state.space);
		/* The build's words are in the slot's program now, so nothing
		 * outside what a sweep reads names a record it made. */
		if (!state.space->shared() && state.space->sweep_due())
			sweep(state, diagnostics);
		/* Past the budget, the largest address spaces are asked to shed;
		 * their caches let go of them at their next run. This is on the
		 * carrier thread with no space, page or registry lock held, which
		 * relieve(), waiting for the budget's own lock, requires. */
		if (gadget_cache::over_budget())
			gadget_cache::relieve();
		return &slot.body->program;
	}

	[[nodiscard]] std::size_t resident_generations() const noexcept
	{
		return window_.generations.size();
	}

#if UML_GADGET_CACHE_CENSUS
	/* What this cache costs, in bytes it actually holds rather than in the
	 * capacity it was configured with. The slab array is the only part that
	 * exists whether or not its ways are occupied; everything else is
	 * proportional to what the workload translated. A translation's parts
	 * are counted as the allocator sized them (X79). */
	struct Footprint final {
		std::size_t slabs, streams, decoded, links, indirect, targets;
		[[nodiscard]] std::size_t total() const noexcept
		{
			return slabs + streams + decoded + links + indirect +
				targets;
		}
	};

	/* One address space's link state. */
	struct GenerationBytes final {
		std::size_t links, indirect, targets;
	};

	/* What an address space costs this cache: its slot list, and, if the
	 * cache keeps the address space to itself, the address space's records
	 * and tables. A shared one is written once, on an S line, and counted
	 * there. */
	[[nodiscard]] static GenerationBytes generation_bytes(
		const Generation &state) noexcept
	{
		GenerationBytes bytes{
			.links = state.slots.capacity() * sizeof(std::uint32_t),
			.indirect = 0,
			.targets = 0,
		};
		if (!state.space->shared()) {
			const auto space = state.space->link_bytes();
			bytes.links += space.links + space.pages;
			bytes.indirect = space.indirect;
			bytes.targets = space.targets;
		}
		return bytes;
	}

	[[nodiscard]] Footprint footprint() const noexcept
	{
		std::size_t slabs = 0;
		std::size_t streams = 0;
		std::size_t decoded = 0;
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			slabs += malloc_size(slab.get());
			for (const auto &slot : slab->slots) {
				if (!slot.resident())
					continue;
				/* A translation is its record, the native
				 * stream and the saved source; a block some
				 * replay has ended inside also holds what that
				 * replay decoded. */
				const auto parts = translation_bytes(slot);
				if (!shares(slot))
					streams += parts.allocated - parts.decoded;
				decoded += parts.decoded;
			}
		}
		std::size_t links = 0;
		std::size_t indirect = 0;
		std::size_t targets = 0;
		for (const auto &[generation, state] : window_.generations) {
			const auto bytes = generation_bytes(state);
			links += bytes.links;
			indirect += bytes.indirect;
			targets += bytes.targets;
		}
		return {
			.slabs = slabs,
			.streams = streams,
			.decoded = decoded,
			.links = links,
			.indirect = indirect,
			.targets = targets,
		};
	}

	[[nodiscard]] std::size_t occupancy() const noexcept
	{
		std::size_t occupied = 0;
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			for (const auto &slot : slab->slots)
				if (slot.resident())
					++occupied;
		}
		return occupied;
	}

	/* How the runtime last said to ask whether a generation is alive
	 * (Executor::leave_gone_generations), which the census asks too. */
	void census_liveness(GenerationLiveness live) noexcept
	{
		census_live_ = live;
	}

	/* Write what this cache holds and how it came to hold it, so
	 * duplication, dormancy and lost work can be counted instead of
	 * estimated. `snapshot` numbers a periodic snapshot; zero is the one
	 * written at teardown. The record format is the arm64 executor's, so
	 * one analyzer reads either engine's capture.
	 *
	 * A translation is identified by the guest bytes it consumed, not by the
	 * stream it produced: two executors translating the same code emit the
	 * same gadgets but embed pointers to their own link records, so only the
	 * source identifies the work. The parts of its allocations go out with
	 * it because they decide what a translation costs.
	 */
	void census(std::FILE *out, std::uint64_t snapshot) const
	{
		const auto parts = footprint();
		/* sizeof(Slot) is reported because the slab total covers the
		 * successor sets as well as the tags, and only the slot size
		 * separates them. */
		const auto slab_count = static_cast<std::size_t>(
			std::count_if(slabs_.begin(), slabs_.end(),
				[](const auto &slab) { return slab != nullptr; }));
		/* The process's gadget-cache budget, the same on every E line
		 * of a snapshot, and what this cache's address spaces are
		 * charged. */
		const auto book = gadget_cache::ledger_state();
		std::size_t charged = 0;
		for (const auto &[generation, state] : window_.generations)
			if (!state.space->shared())
				charged += state.space->account().bytes();
		std::fprintf(out,
			"E %p generations=%zu occupancy=%zu slots=%zu "
			"slot_bytes=%zu slab_slots=%zu slab_count=%zu slabs=%zu "
			"streams=%zu decoded=%zu links=%zu indirect=%zu "
			"targets=%zu total=%zu sweeps=%llu swept_links=%llu "
			"swept_sites=%llu sweep_ns=%llu dead_left=%llu "
			"lru_tomb=%llu charged=%zu budget=%zu metered=%zu "
			"metered_shedding=%zu metered_peak=%zu reliefs=%llu "
			"shed=%llu renewals=%llu",
			static_cast<const void *>(this), resident_generations(),
			occupancy(), program_cache_slots, sizeof(Slot),
			program_cache_slab_slots, slab_count, parts.slabs,
			parts.streams, parts.decoded, parts.links,
			parts.indirect, parts.targets, parts.total(),
			static_cast<unsigned long long>(sweeps_),
			static_cast<unsigned long long>(swept_links_),
			static_cast<unsigned long long>(swept_sites_),
			static_cast<unsigned long long>(sweep_ns_),
			static_cast<unsigned long long>(dead_left_),
			static_cast<unsigned long long>(lru_tomb_), charged,
			book.budget, book.total, book.shedding, book.peak_open,
			static_cast<unsigned long long>(book.reliefs),
			static_cast<unsigned long long>(book.shed_accounts),
			static_cast<unsigned long long>(Space::closed_spaces()));
		census::write_when(out, snapshot);
		history_.write(out);
		std::fputc('\n', out);
		const auto now = census::elapsed_seconds();
		/* What each address space's resident translations occupy. */
		std::unordered_map<std::uint16_t, std::size_t> held;
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			for (const auto &slot : slab->slots)
				if (slot.resident() && !shares(slot))
					held[slot.owner()] +=
						translation_bytes(slot).allocated;
		}
		for (const auto &[generation, state] : window_.generations) {
			const auto found = std::find(window_.order.rbegin(),
				window_.order.rend(), generation);
			const bool running =
				generation == window_.current_generation;
			const auto bytes = generation_bytes(state);
			/* Whether the MMU still has the address space, asked as the
			 * runtime asks: 1 live, 0 destroyed, negative when it cannot
			 * say from this thread or the cache has not been told how
			 * to ask. */
			const int alive = census_live_ != nullptr ?
				census_live_(static_cast<unsigned long>(generation)) :
				-1;
			/* The G line's bytes are this cache's own list of its slots
			 * in the space, and all of a private space's; a shared
			 * space is on its S line, named by its identity, which
			 * every cache holding it writes and a reader counts
			 * once. */
			std::fprintf(out,
				"G %llu current=%d rank=%td idle_switches=%llu "
				"idle_seconds=%.3f link_records=%zu links=%zu "
				"indirect=%zu targets=%zu alive=%d held=%zu",
				static_cast<unsigned long long>(generation),
				running ? 1 : 0, found - window_.order.rbegin(),
				static_cast<unsigned long long>(running ? 0 :
					switches_ - state.census_switch),
				running ? 0.0 : now - state.census_seconds,
				state.space->link_records(),
				bytes.links, bytes.indirect, bytes.targets, alive,
				held[state.id]);
			if (state.space->shared())
				std::fprintf(out, " space=%p",
					static_cast<const void *>(state.space));
			std::fputc('\n', out);
			if (state.space->shared()) {
				const auto space = state.space->link_bytes();
				std::fprintf(out,
					"S %p generation=%llu mode=%u attached=%zu "
					"stream_index=%zu streams=%zu arena_empty=%zu "
					"links=%zu indirect=%zu link_records=%zu "
					"limbo=%zu alive=%d charged=%zu shedding=%d\n",
					static_cast<const void *>(state.space),
					static_cast<unsigned long long>(generation),
					unsigned{checked_}, state.space->attached(),
					state.space->stream_index_bytes(),
					state.space->stream_bytes(),
					state.space->empty_bytes(),
					space.links + space.pages + space.targets,
					space.indirect, state.space->link_records(),
					state.space->limbo_bodies(), alive,
					state.space->account().bytes(),
					state.space->shedding() ? 1 : 0);
			}
		}
		if (census::totals_only())
			return;
		const auto sample = census::word_sample();
		/* The same link targets are sampled in every cache, so the
		 * sample says how many of an address space's records each
		 * cache keeps for itself. A shared space writes its records
		 * from every cache that holds it, which a reader removes by
		 * the space's identity. */
		if (sample != 0)
			for (const auto &[generation, state] :
			     window_.generations)
				state.space->for_each_link(
					[&, generation = generation, &state = state](
						const LinkRecord &record) {
						if (!census::sampled(record.link.guest_rip,
							    sample))
							return;
						std::fprintf(out, "L %llu 0x%llx",
							static_cast<unsigned long long>(
								generation),
							static_cast<unsigned long long>(
								record.link.guest_rip));
						if (state.space->shared())
							std::fprintf(out, " %p",
								static_cast<const void *>(
									state.space));
						std::fputc('\n', out);
					});
		const auto text = census::text_of(reinterpret_cast<const void *>(
			reinterpret_cast<std::uintptr_t>(
				native::uml_x86_64_gadget_link_exit)));
		CensusRecords records;
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			for (const auto &slot : slab->slots) {
				if (!slot.resident())
					continue;
				const auto &program = slot.body->program;
				const auto bytes = translation_bytes(slot);
				std::fprintf(out,
					"P %llu 0x%llx %zu %zu %zu 0x%llx %zu %zu "
					"%zu %zu %p\n",
					static_cast<unsigned long long>(
						generation_of(slot)),
					static_cast<unsigned long long>(
						slot.address()),
					program.instruction_count(),
					program.native_stream().size(),
					program.source().size(),
					static_cast<unsigned long long>(
						census_digest(slot)),
					bytes.allocated, sizeof(Body),
					program.native_stream().size_bytes(),
					program.source().size(),
					static_cast<const void *>(slot.body));
				if (sample != 0 &&
				    census::sampled(slot.address(), sample))
					census_words(out, slot, text, records);
			}
		}
	}
#endif

private:
	static constexpr std::uint64_t code_page_mask =
		~std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE - 1U};
#if UML_GADGET_CACHE_CENSUS
	/* libc++ keeps one heap node per element -- the value, a next pointer
	 * and the cached hash -- plus one pointer per bucket. Exact for the
	 * value, close enough for the node. */
	template <typename Map>
	[[nodiscard]] static std::size_t map_bytes(const Map &map) noexcept
	{
		constexpr std::size_t node = 2 * sizeof(void *);
		return map.size() *
			(sizeof(typename Map::value_type) + node) +
			map.bucket_count() * sizeof(void *);
	}

	/* A resident translation's allocations as the allocator sized them:
	 * its record, stream and source, and what a replay decoded. */
	struct TranslationBytes final {
		std::size_t allocated, decoded;
	};

	[[nodiscard]] static TranslationBytes translation_bytes(
		const Slot &slot) noexcept
	{
		const auto &program = slot.body->program;
		const auto replay = program.replay_bytes();
		const auto decoded = replay != 0 ? malloc_good_size(replay) : 0;
		return {
			.allocated = gadget_cache::BodyArena::block_bytes(
					slot.body->allocation_bytes()) +
				gadget_cache::BodyArena::block_bytes(
					program.byte_count()) + decoded,
			.decoded = decoded,
		};
	}

	/* The census keys a translation by address space and guest address;
	 * the execution mode takes the top bit, which no canonical address
	 * of either mode sets. */
	[[nodiscard]] static std::uint64_t census_key(const Slot &slot) noexcept
	{
		return slot.address() ^
			(static_cast<std::uint64_t>(mode_index(slot.mode())) << 63);
	}

	/* The same bytes decoded in the other execution mode are a different
	 * translation, so the mode seeds the digest. */
	[[nodiscard]] static std::uint64_t census_digest(const Slot &slot) noexcept
	{
		auto digest = census::digest_seed ^ mode_index(slot.mode());
		for (const auto byte : slot.body->program.source())
			digest = census::digest_add(digest,
				static_cast<std::uint8_t>(byte));
		return digest;
	}

	void census_depart(const Slot &slot, census::Departure why)
	{
		if (slot.body == nullptr)
			return;
		history_.depart(generation_of(slot), census_key(slot),
			census_digest(slot), translation_bytes(slot).allocated, why);
	}

	/* What each record a stream can point at names, for each address
	 * space. */
	using CensusRecords =
		std::unordered_map<std::uint64_t, census::RecordNames>;

	/* One sampled translation's stream. */
	void census_words(std::FILE *out, const Slot &slot,
		census::TextRange text, CensusRecords &records) const
	{
		auto [named, inserted] = records.try_emplace(generation_of(slot));
		if (inserted) {
			const auto *owner = owner_of(slot);
			if (owner != nullptr) {
				auto &table = named->second;
				owner->space->for_each_link(
					[&table](const LinkRecord &record) {
						table[reinterpret_cast<std::uintptr_t>(
							&record.link)] =
							{'l', record.link.guest_rip};
					});
				owner->space->for_each_indirect(
					[&table](const IndirectRecord &record) {
						table[reinterpret_cast<std::uintptr_t>(
							&record.site)] =
							{'i', record.source_rip};
					});
			}
		}
		census::write_words(out, slot.address(),
			slot.body->program.native_stream(), text, named->second);
	}
#endif

	/* Take the write translations of a body's pages out of this executor's
	 * software TLB, so that its next store to one of them asks the miss
	 * handler, which tags it for publication now that the page holds code. */
	void retire_write_translations(const Program &program) noexcept
	{
		if (write_translations_ == nullptr)
			return;
		const auto pages = program_pages(program);
		for (std::size_t entry = 0; entry < pages.count; ++entry)
			/* One is the unmatchable empty tag: guest pages are
			 * aligned, so no real page compares equal to it. */
			write_translations_[native_tlb_index(pages.pages[entry])]
				.write_page = 1;
	}

	/* Put a held body in the set for its address, letting go of whatever
	 * the way it takes held, and return the slot. Nothing here allocates,
	 * so it cannot fail once the body is held. */
	Slot &install(const ProgramCacheLocation &location, Slab &slab,
		std::uint64_t tag, Generation &state, Body *body,
		std::uint8_t instruction_limit) noexcept
	{
		auto &replacement = next_replacement_[location.set];
		std::size_t selected = replacement;
		for (std::size_t way = 0; way < program_cache_ways; ++way) {
			const auto &slot = slab.slots[location.offset + way];
			if (!slot.resident()) {
				selected = way;
				break;
			}
		}
		replacement = static_cast<std::uint8_t>(
			(selected + 1U) & (program_cache_ways - 1U));

		if (state.slots_complete) {
			if (state.slots.size() < program_cache_slots) {
				try {
					state.slots.push_back(static_cast<std::uint32_t>(
						location.slab * program_cache_slab_slots +
						location.offset + selected));
				} catch (...) {
					/* Without room for the slot in the list the
					 * address space's slots are found by walking
					 * the cache. */
					state.slots_complete = false;
					state.slots.clear();
					state.slots.shrink_to_fit();
				}
			} else {
				state.slots_complete = false;
				state.slots.clear();
				state.slots.shrink_to_fit();
			}
		}

		auto &slot = slab.slots[location.offset + selected];
		auto &successors = slab.successors[location.offset + selected];
#if UML_GADGET_CACHE_CENSUS
		if (slot.resident())
			census_depart(slot, census::Departure::evicted);
#endif
		/* Whatever occupied the slot lets go of its body, whose address
		 * space may not be the running one. */
		release_body(slot, false);
		slot.tag = 0;
		successors = {};
		slot.body = body;
		slot.memo = 0;
		slot.set_epoch(window_.current_epoch);
		slot.instruction_limit = instruction_limit;
		slot.terminal = body->program.terminal();
		/* The tag is the occupancy marker and is written only after the
		 * slot is complete; owner() reads the id from it. */
		slot.tag = tag;
		retire_write_translations(body->program);
		return slot;
	}

	/* Take the translation another executor of the address space holds for
	 * `address` instead of building it: validated against guest memory
	 * exactly as a stale slot is, then held and linked like a stored one. */
	[[nodiscard]] const Program *adopt(std::uint64_t address,
		ExecutionMode mode, std::size_t maximum_instructions,
		std::uint64_t mapping_changes,
		::uml::memory::GuestAddressSpace &memory, void *execution_context,
		ProgramCacheSequenceReader sequence_reader,
		ProgramCacheDiagnostics &diagnostics)
	{
		const auto tag = tag_of(address, mode);
		if (tag == unmatchable_tag)
			return nullptr;
		auto &state = *window_.current;
		auto *body = state.space->hold_indexed(address, mode,
			kept_instruction_limit(maximum_instructions));
		if (body == nullptr)
			return nullptr;
		++diagnostics.validation_attempts;
		const auto verdict = validate_body(*body, mapping_changes, memory,
			execution_context, sequence_reader, diagnostics);
		if (verdict != Validation::valid) {
			state.space->release(body, verdict == Validation::stale);
			state.space->reclaim(announcement_);
			return nullptr;
		}
		const auto location = locate_program(
			window_.current_generation, address, mode);
		auto &slab = slabs_[location.slab];
		if (!slab) {
			try {
				slab = std::make_unique<Slab>();
			} catch (...) {
				state.space->release(body, false);
				return nullptr;
			}
			for (std::size_t offset = 0; offset < program_cache_slab_slots;
			     ++offset)
				slab->slots[offset].index = static_cast<std::uint32_t>(
					location.slab * program_cache_slab_slots + offset);
		}
		auto &slot = install(location, *slab, tag, state, body,
			body->instruction_limit);
		activate_link(slot);
		activate(slot, successors_at(slot.index));
		state.space->reclaim(announcement_);
		return &slot.body->program;
	}

	/* A slot keeps the limit it was built under in one byte. The runtime
	 * asks for at most 64 instructions; a larger limit is kept, and
	 * compared, as 255, so a block built under one serves any other. */
	[[nodiscard]] static std::uint8_t kept_instruction_limit(
		std::size_t limit) noexcept
	{
		return static_cast<std::uint8_t>(std::min<std::size_t>(limit, 0xffU));
	}

	/* The tag a lookup in the running address space compares. */
	[[nodiscard]] std::uint64_t tag_of(std::uint64_t address,
		ExecutionMode mode) const noexcept
	{
		const auto compact = compact_tag_address(address, mode);
		return compact == unmatchable_tag ? unmatchable_tag :
			compact | window_.current_owner;
	}

	[[nodiscard]] Generation *owner_of(const Slot &slot) const noexcept
	{
		return window_.owners[slot.owner()];
	}

#if UML_GADGET_CACHE_CENSUS
	/* Whether the slot's body belongs to an address space that other
	 * executors may hold too, and so is counted with it. */
	[[nodiscard]] bool shares(const Slot &slot) const noexcept
	{
		const auto *owner = owner_of(slot);
		return owner != nullptr && owner->space->shared();
	}

	[[nodiscard]] std::uint64_t generation_of(
		const Slot &slot) const noexcept
	{
		return window_.owners[slot.owner()]->value;
	}
#endif

	/* A code change was seen in the running address space: every slot's
	 * validation is out of date. When the epoch would pass what a slot can
	 * hold, the slots that carry one are cleared first. */
	void advance_epoch(Generation &state) noexcept
	{
		const auto forget_epochs = [this](Generation &wrapped) noexcept {
			const auto id = wrapped.id;
			const auto forget = [id](Slot &slot) noexcept {
				if (slot.resident() && slot.owner() == id)
					slot.set_epoch(0);
			};
			if (wrapped.slots_complete) {
				for (const auto index : wrapped.slots)
					forget(slot_at(index));
			} else {
				for (auto &slab : slabs_)
					if (slab)
						for (auto &slot : slab->slots)
							forget(slot);
			}
		};
		window_.advance_epoch(state, forget_epochs);
	}

	[[nodiscard]] const Program *use(Slot &slot,
		std::size_t maximum_instructions,
		std::uint64_t mapping_changes,
		::uml::memory::GuestAddressSpace &memory, void *execution_context,
		ProgramCacheSequenceReader sequence_reader,
		ProgramCacheDiagnostics &diagnostics)
	{
		if (slot.body == nullptr) {
#if UML_GADGET_CACHE_CENSUS
			history_.missed();
#endif
			discard(slot, false);
			return nullptr;
		}
		if (!slot.terminal && kept_instruction_limit(maximum_instructions) >
			slot.instruction_limit) {
#if UML_GADGET_CACHE_CENSUS
			history_.missed();
			census_depart(slot, census::Departure::discarded);
#endif
			discard(slot, false);
			return nullptr;
		}
#if UML_GADGET_CACHE_CENSUS
		if (slot.epoch() == window_.current_epoch)
			history_.hit();
#endif
		if (slot.epoch() != window_.current_epoch) {
			++diagnostics.validation_attempts;
			if (!validate(slot, mapping_changes, memory,
				execution_context, sequence_reader, diagnostics)) {
#if UML_GADGET_CACHE_CENSUS
				history_.missed();
#endif
				return nullptr;
			}
#if UML_GADGET_CACHE_CENSUS
			history_.revalidated();
#endif
		}
		/* Whatever reset this record -- a replaced slot, or a code change
		 * that cooled the whole address space -- the program in this slot is
		 * valid for the current sequence by the time it is returned, so it is
		 * a legitimate link target again. Re-arming here is what lets a chain
		 * rebuild as blocks run, rather than only when one needs revalidating.
		 */
		activate_link(slot);
		return &slot.body->program;
	}

	void activate(Slot &slot, SuccessorSet &successors) noexcept
	{
		if (preceding_ != nullptr) {
			const auto id = slot.index + 1U;
			auto selected = preceding_->next_replacement();
			for (std::size_t way = 0; way < preceding_->entries.size(); ++way) {
				const auto entry = preceding_->entry(way);
				if (entry == 0 || entry == id) {
					selected = way;
					break;
				}
			}
			preceding_->set(selected, id, (selected + 1U) & 1U);
		}
		preceding_ = &successors;
	}

	/* Link the block a slot holds: the record its address names is pointed at
	 * the body unless a code change has been published since the slot was
	 * validated, or a store has landed over the body. The record is the
	 * address space's, so what another executor armed is what this one
	 * follows, and the slot keeps it for next time. */
	void activate_link(Slot &slot) noexcept
	{
		if (slot.body == nullptr)
			return;
		auto *owner = owner_of(slot);
		if (owner == nullptr)
			return;
		/* Publishing here, rather than at a branch that asked for this
		 * address, is what lets a RET reach a return site it has never
		 * reached before: the block that runs there announces itself. */
		auto *record = owner->space->activate(slot.link(),
			slot.address(), slot.mode(), *slot.body, owner->changes);
		if (record != nullptr)
			slot.set_link(record);
	}

	/* Release the link records and indirect sites of one address space that
	 * nothing it holds names any more, as the arm64 cache does (G226).
	 *
	 * A resident translation's stream words name its successors' records and
	 * its indirect sites, and a resident block keeps its own record, which
	 * activation arms; every stream word is tried, operands too, which only
	 * ever keeps a record that nothing needed. Site cells and the target
	 * tables may still name a record no translation names, whose block is
	 * gone: those entries are emptied, as a code change empties them, and
	 * learn the target again if it runs. A transient program is never alive
	 * across a store, the only place a sweep runs.
	 */
	void sweep(Generation &state,
		ProgramCacheDiagnostics &diagnostics) noexcept
	{
#if UML_GADGET_CACHE_CENSUS
		const auto started = std::chrono::steady_clock::now();
		const auto links = diagnostics.released_links;
		const auto sites = diagnostics.released_sites;
#endif
		try {
			sweep_records(state, diagnostics);
		} catch (const std::bad_alloc &) {
			/* A sweep only returns memory; without the memory to run
			 * one, the records stay. */
		}
		state.space->swept();
		++diagnostics.link_sweeps;
#if UML_GADGET_CACHE_CENSUS
		++sweeps_;
		swept_links_ += diagnostics.released_links - links;
		swept_sites_ += diagnostics.released_sites - sites;
		sweep_ns_ += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - started).count());
#endif
	}

	/* sweep(), with everything it allocates allocated before anything is
	 * released. */
	void sweep_records(Generation &state,
		ProgramCacheDiagnostics &diagnostics)
	{
		auto links = state.space->link_marks();
		auto sites = state.space->site_marks();
		std::vector<std::uint64_t> seen((program_cache_slots + 63) / 64);
		std::vector<std::uint32_t> held;
		const auto visit = [&](std::uint32_t index) {
			const auto &slot = slot_at(index);
			if (!slot.resident() || slot.owner() != state.id ||
			    slot.body == nullptr ||
			    (seen[index / 64] >> (index % 64) & 1U) != 0)
				return;
			seen[index / 64] |= std::uint64_t{1} << (index % 64);
			held.push_back(index);
			const auto own = mode_index(slot.mode());
			if (const auto *memo = slot.link(); memo != nullptr)
				links[own].mark(memo);
			else if (const auto *record =
					 state.space->find_link(slot.address(), slot.mode()))
				links[own].mark(record);
			for (const auto word :
			     slot.body->program.native_stream())
				for (std::size_t mode = 0; mode < links.size(); ++mode) {
					links[mode].mark(
						word - offsetof(LinkRecord, link));
					sites[mode].mark(
						word - offsetof(IndirectRecord, site));
				}
		};
		if (state.slots_complete) {
			for (const auto index : state.slots)
				visit(index);
		} else {
			for (std::size_t slab = 0; slab < program_cache_slab_count;
			     ++slab)
				if (slabs_[slab])
					for (std::size_t offset = 0;
					     offset < program_cache_slab_slots;
					     ++offset)
						visit(static_cast<std::uint32_t>(
							slab * program_cache_slab_slots +
							offset));
		}
		held.shrink_to_fit();
		const auto released = state.space->sweep(links, sites);
		diagnostics.released_sites += released.sites;
		diagnostics.released_links += released.links;
		state.slots = std::move(held);
		state.slots_complete = true;
	}

	void touch(std::uint64_t generation) { window_.touch(generation); }

	/* The generation's number leaves the window's order unless `tombstone`
	 * keeps its place there (leave_gone). A number in the order that names
	 * no address space is such a tombstone: retiring it only drops the
	 * place. */
	void retire(std::uint64_t generation, bool tombstone = false)
	{
		const auto owner = window_.generations.find(generation);
		if (owner != window_.generations.end()) {
			/* Everything that can fail comes before anything changes, so
			 * that a failure leaves the address space held and retirable
			 * again. */
			window_.reserve_id();
			const auto id = owner->second.id;
			if (owner->second.slots_complete) {
				for (const auto index : owner->second.slots)
					release(slot_at(index), id);
			} else {
				for (auto &slab : slabs_) {
					if (!slab)
						continue;
					for (auto &slot : slab->slots)
						release(slot, id);
				}
			}
			auto *space = owner->second.space;
			window_.leave(owner, id);
			/* Letting go of its slots may have left a shared space
			 * mostly freed arena; this cache still holds the space,
			 * which is what makes it safe to close. */
			renew_if_due(*space);
			/* The last executor to let go of a shared space takes it
			 * with it; what other executors still hold of it stays. */
			space->reclaim(announcement_);
			SpaceRegistry::instance().detach(space, announcement_);
		}
		if (registries_ != nullptr)
			registries_->release(generation);
		if (tombstone)
			return;
		window_.forget_place(generation);
	}

	void release(Slot &slot, std::uint16_t owner) noexcept
	{
		if (!slot.resident() || slot.owner() != owner)
			return;
#if UML_GADGET_CACHE_CENSUS
		census_depart(slot, census::Departure::retired);
#endif
		discard(slot, false);
	}

	[[nodiscard]] Slot &slot_at(std::uint32_t index) noexcept
	{
		auto &slab = slabs_[index / program_cache_slab_slots];
		return slab->slots[index % program_cache_slab_slots];
	}

	[[nodiscard]] SuccessorSet &successors_at(std::uint32_t index) noexcept
	{
		auto &slab = slabs_[index / program_cache_slab_slots];
		return slab->successors[index % program_cache_slab_slots];
	}

	[[nodiscard]] Generation &current()
	{
		if (window_.current == nullptr) {
			bool inserted = false;
			window_.current = &generation_for(
				window_.current_generation, 0, inserted);
			window_.bind(*window_.current);
		}
		return *window_.current;
	}

	/* The state for an address space, made and attached to its translations
	 * on first sight: shared with the other executors running it, or this
	 * cache's own. A failure leaves nothing behind. */
	[[nodiscard]] Generation &generation_for(std::uint64_t generation,
		std::uint64_t changes, bool &inserted)
	{
		auto [entry, fresh] =
			window_.generations.try_emplace(generation);
		inserted = fresh;
		auto &state = entry->second;
		if (!fresh)
			return state;
		try {
			state.space = SpaceRegistry::instance().attach(
				SpaceKey{
					.generation = static_cast<unsigned long>(generation),
					.mode = checked_,
					.owner = share_ ? nullptr :
						static_cast<const void *>(&shed_requested_),
					.options = 0,
				},
				changes, announcement_);
		} catch (...) {
			window_.generations.erase(entry);
			throw;
		}
		try {
			window_.enroll(state, generation);
		} catch (...) {
			SpaceRegistry::instance().detach(state.space,
				announcement_);
			window_.generations.erase(entry);
			throw;
		}
		return state;
	}

	/* Let a slot go of its body: counted as leaving its address space, whose
	 * next sweep waits for enough of them (gadget_cache::sweep_due), and
	 * given back to it, which frees the body when no other executor holds
	 * it, once none can be running it. What it held was charged through the
	 * arenas it was in, and is refunded as they give it back. `stale` says
	 * the body's code is no longer the guest's, so that no executor adopts
	 * it. */
	void release_body(Slot &slot, bool stale) noexcept
	{
		if (slot.body == nullptr)
			return;
		if (auto *owner = owner_of(slot); owner != nullptr) {
			owner->space->note_departed();
			owner->space->release(slot.body, stale);
		}
		slot.body = nullptr;
	}

	/* Close a shared address space that holds enough freed arena
	 * (Space::renewal_due), for every executor attached to let go of. Only
	 * one this cache holds, and so is attached to throughout: a cache closes
	 * the space it stores into, and the ones it lets go of whole, since a
	 * mostly dead space receives no stores. Called with no lock of the space
	 * held. */
	static void renew_if_due(Space &space) noexcept
	{
		if (!space.renewal_due())
			return;
		try {
			SpaceRegistry::instance().close(&space);
		} catch (...) {
			/* Still due: the next body let go of closes it. */
		}
	}

	void discard(Slot &slot, bool stale) noexcept
	{
		if (slot.body != nullptr) {
			auto *owner = owner_of(slot);
			release_body(slot, stale);
			if (owner != nullptr)
				owner->space->reclaim(announcement_);
		}
		slot.tag = 0;
		slot.memo = 0;
		slot.terminal = false;
	}

	enum class Validation : std::uint8_t { valid, stale, race };

	/* Whether a body still matches guest memory. It does when every page
	 * stamp it recorded is unchanged, or else when its saved source still
	 * matches guest memory; the stamps are taken once, before a byte of
	 * source is read, and a successful comparison records those same
	 * values. A moved code-change sequence only brings this vCPU here and
	 * discards nothing by itself. The mapping sequence and an execution
	 * interrupt still abandon the validation, because no page stamp
	 * describes either ("Keeping a cached translation" in README.md): that
	 * is a race, and the body is neither valid nor known to be stale. */
	[[nodiscard]] Validation validate_body(Body &body,
		std::uint64_t mapping_changes,
		::uml::memory::GuestAddressSpace &memory, void *execution_context,
		ProgramCacheSequenceReader sequence_reader,
		ProgramCacheDiagnostics &diagnostics)
	{
		const auto &program = body.program;
		if (body.stale.load(std::memory_order_acquire))
			return Validation::stale;
		if (program.source().empty() ||
			program.source().size() != program.byte_count() ||
			program.source().size() > maximum_cached_source_bytes) {
			++diagnostics.validation_mismatches;
			return Validation::stale;
		}
		const auto memory_settled = [&]() noexcept {
			if (execution_context == nullptr || sequence_reader == nullptr)
				return true;
			const auto current = sequence_reader(execution_context);
			if (!current.interrupted &&
			    current.mapping_changes == mapping_changes)
				return true;
			++diagnostics.validation_sequence_races;
			return false;
		};

		const auto current_stamps = capture_program_code_stamps(
			memory, program, execution_context);
		const auto recorded = body.stamps();
		if (recorded.complete && current_stamps.complete &&
		    current_stamps == recorded) {
			if (!memory_settled())
				return Validation::race;
			++diagnostics.validation_stamp_hits;
			++diagnostics.validation_hits;
			return Validation::valid;
		}

		switch (compare_program_source(memory, program,
			program.source())) {
		case ProgramSourceComparison::fault:
			++diagnostics.validation_faults;
			return Validation::stale;
		case ProgramSourceComparison::mismatch:
			++diagnostics.validation_mismatches;
			return Validation::stale;
		case ProgramSourceComparison::match:
			break;
		}

		if (!memory_settled())
			return Validation::race;
		body.set_stamps(current_stamps);
		++diagnostics.validation_hits;
		return Validation::valid;
	}

	/* validate_body() for a slot's body: a valid one is current for the
	 * running address space's code, and one that is not the guest's any
	 * more is let go of, as stale. */
	[[nodiscard]] bool validate(Slot &slot,
		std::uint64_t mapping_changes,
		::uml::memory::GuestAddressSpace &memory, void *execution_context,
		ProgramCacheSequenceReader sequence_reader,
		ProgramCacheDiagnostics &diagnostics)
	{
		if (slot.body == nullptr) {
			++diagnostics.validation_mismatches;
			discard(slot, false);
			return false;
		}
		switch (validate_body(*slot.body, mapping_changes, memory,
			execution_context, sequence_reader, diagnostics)) {
		case Validation::valid:
			slot.set_epoch(window_.current_epoch);
			return true;
		case Validation::stale:
#if UML_GADGET_CACHE_CENSUS
			census_depart(slot, census::Departure::discarded);
#endif
			discard(slot, true);
			return false;
		case Validation::race:
			return false;
		}
		return false;
	}

	/* Set (Space::request_renewal) when the budget asks an address space
	 * held here to shed, which the cache lets go of at the start of its next
	 * run (leave_shedding()); before the window's address spaces, whose
	 * accounts may set it until they are destroyed. */
	std::atomic<bool> shed_requested_{};
	/* What this cache's executor says about itself for the bodies other
	 * executors free, null when it shares nothing; whether the cache shares
	 * its address spaces; and the exceptions its blocks are built for. */
	const Announcement *announcement_{};
	bool share_{};
	std::uint8_t checked_{};
	Window window_;
	std::array<std::unique_ptr<Slab>, program_cache_slab_count> slabs_{};
	std::array<std::uint8_t, program_cache_sets> next_replacement_{};
	SuccessorSet *preceding_{};
	NativeTlbEntry *write_translations_{};
	CodePageRegistries *registries_{};
	ProgramCacheCodeChangeReader code_change_reader_{};
	void *code_change_context_{};
#if UML_GADGET_CACHE_CENSUS
	census::History history_;
	census::Schedule schedule_;
	unsigned census_id_{census::next_cache_id()};
	/* Address-space switches, the clock dormancy is counted in. */
	std::uint64_t switches_{};
	/* Link sweeps, what they released, and the time they took. */
	std::uint64_t sweeps_{};
	std::uint64_t swept_links_{};
	std::uint64_t swept_sites_{};
	std::uint64_t sweep_ns_{};
	/* Address spaces let go of because their generation was destroyed, and
	 * the tombstones of those the window's order retired. */
	std::uint64_t dead_left_{};
	std::uint64_t lru_tomb_{};
	GenerationLiveness census_live_{};
#endif
};

#if UML_GADGET_CACHE_CENSUS
void dump_cache_census(const ProgramCache &programs) noexcept
{
	/* Appended, because every executor of the run reports its own cache and
	 * the union across them is the measurement. */
	census::append([&programs](std::FILE *out) {
		programs.census(out, 0);
	});
}
#endif

} // namespace

struct Executor::Storage final {
	/* Executors running one address space share its translations unless
	 * DARWIN_UML_GADGET_SHARED_BODIES=0 keeps them apart, and a reclaimer
	 * can see whether this one is running translated code (announcements are
	 * a bounded resource). */
	Storage()
		: announcement{shared_bodies_enabled() ? acquire_announcement() :
				nullptr},
		  programs{announcement, announcement != nullptr}
	{}
	/* A move-assigned executor drops its old storage here, so the pages it
	 * counted on the shared list are subtracted here rather than in the
	 * executor's own destructor. */
	~Storage()
	{
#if UML_GADGET_CACHE_CENSUS
		/* Before the cache is emptied, and here rather than in the
		 * executor's own destructor so that a move-assigned executor
		 * reports the storage it drops. */
		dump_cache_census(programs);
#endif
		programs.invalidate();
		registries.drop_all();
		/* The cache let go of its address spaces before the executor
		 * stops being one a reclaimer waits for. */
		release_announcement(announcement);
	}
	Storage(const Storage &) = delete;
	Storage &operator=(const Storage &) = delete;

	std::array<NativeTlbEntry, tlb_entry_count> entries{};
	CodePageRegistries registries;
	Announcement *announcement{};
	ProgramCache programs;
	std::optional<Program> transient_program;
	::uml::memory::GuestAddressSpace *memory{};
	std::uint64_t generation{};
	std::uint64_t mapping_changes{};
	std::uint64_t code_changes{};
	void *execution_context{};
	ProgramCacheSequenceReader sequence_reader{};
	MappingChangeReader mapping_change_reader{};
	/* The mapping sequence every resident software-TLB entry is coherent
	 * for. A replay advances it to what the address space reported, which
	 * can be newer than the sequence the caller asked about. */
	std::uint64_t tlb_mapping_sequence{};
	ProgramCacheDiagnostics diagnostics{};
	/* The MMU's count of destroyed generations when this executor last let
	 * go of the address spaces of destroyed ones. */
	unsigned long long retirements_seen{};
	bool prepared{};
	bool cache_ready{};
	unsigned checked{};
	std::vector<std::uintptr_t> checked_stream;
};

Executor::Executor() : storage_{std::make_unique<Storage>()}
{
	storage_->programs.set_write_translations(storage_->entries.data());
	storage_->registries.participant.write_translations =
		storage_->entries.data();
	storage_->programs.set_code_page_registries(&storage_->registries);
}
Executor::~Executor() = default;
Executor::Executor(Executor &&) noexcept = default;
Executor &Executor::operator=(Executor &&) noexcept = default;

namespace {

/* Retire the software-TLB entries covering one changed guest range. A write
 * entry carries its publication tags in the low bits of the page it holds, so
 * the compare masks them exactly as the engine's own hit path does. */
void retire_mapping_range(std::span<NativeTlbEntry> entries,
	std::uint64_t address, std::uint64_t length) noexcept
{
	constexpr std::uint64_t page_mask = ~std::uint64_t{0xfff};
	constexpr std::uint64_t write_tags =
		UML_X86_64_TLB_WRITE_CODE | UML_X86_64_TLB_WRITE_CACHED_CODE;
	const auto first = address & page_mask;
	const auto last = (address + length - 1) & page_mask;
	for (auto page = first;; page += 0x1000) {
		auto &entry = entries[native_tlb_index(page)];
		if ((entry.read_page & page_mask) == page)
			entry.read_page = NativeTlbEntry{}.read_page;
		if ((entry.write_page & ~write_tags & page_mask) == page)
			entry.write_page = NativeTlbEntry{}.write_page;
		if (page == last)
			break;
	}
}

/* Replaying a range list costs a lookup per page, so a span wider than a
 * fraction of the table is retired by clearing the table instead. */
constexpr std::uint64_t maximum_replayed_pages = tlb_entry_count / 4;

} // namespace

void Executor::prepare(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t generation, std::uint64_t mapping_changes,
	std::uint64_t code_changes, void *execution_context,
	ProgramCacheSequenceReader sequence_reader,
	ProgramCacheCodeChangeReader code_change_reader,
	MappingChangeReader mapping_change_reader) noexcept
{
	const bool memory_changed = storage_->memory != &memory;
	const bool mapping_changed = memory_changed || !storage_->prepared ||
		storage_->generation != generation ||
		storage_->mapping_changes != mapping_changes;
	const bool cache_binding_changed = memory_changed || !storage_->prepared ||
		storage_->generation != generation ||
		storage_->code_changes != code_changes;
	storage_->mapping_change_reader = mapping_change_reader;
	if (mapping_changed) {
		/* The address space hands a reader the ranges it has not seen, so a
		 * reader only a few changes behind retires exactly those pages
		 * instead of every entry it holds. Anything it cannot describe, and
		 * any move to another address space or generation, still clears the
		 * whole table. */
		bool replayed = false;
		if (!memory_changed && storage_->prepared &&
			storage_->generation == generation &&
			mapping_change_reader != nullptr) {
			std::array<ProgramMappingChange, 128> ranges{};
			std::uint64_t current = mapping_changes;
			const int count = mapping_change_reader(execution_context,
				generation, storage_->tlb_mapping_sequence, ranges.data(),
				ranges.size(), &current);
			if (count >= 0) {
				std::uint64_t pages = 0;
				for (int index = 0; index < count; ++index)
					pages += ranges[index].length / 0x1000 + 2;
				if (pages <= maximum_replayed_pages) {
					for (int index = 0; index < count; ++index)
						retire_mapping_range(storage_->entries,
							ranges[index].address, ranges[index].length);
					storage_->tlb_mapping_sequence = current;
					replayed = true;
				}
			}
		}
		if (!replayed) {
			std::ranges::fill(storage_->entries, NativeTlbEntry{});
			storage_->tlb_mapping_sequence = mapping_changes;
		}
	}
	if (memory_changed)
		storage_->programs.invalidate();
	if (memory_changed || !storage_->prepared ||
	    storage_->generation != generation) {
		try {
			storage_->registries.bind(generation);
		} catch (...) {
			/* Without a registry every executable store publishes, which
			 * is the conservative contract this replaces. */
			storage_->registries.unbind();
		}
	}
	storage_->memory = &memory;
	storage_->generation = generation;
	storage_->mapping_changes = mapping_changes;
	storage_->code_changes = code_changes;
	storage_->execution_context = execution_context;
	storage_->sequence_reader = sequence_reader;
	storage_->programs.set_code_change_reader(
		code_change_reader, execution_context);
	storage_->prepared = true;
	if (!cache_binding_changed && storage_->cache_ready)
		return;
	try {
		storage_->programs.prepare(generation, code_changes);
		storage_->cache_ready = true;
	} catch (...) {
		storage_->programs.invalidate();
		storage_->cache_ready = false;
	}
}

void Executor::leave_gone_generations(unsigned long long retirements,
	GenerationLiveness live) noexcept
{
#if UML_GADGET_CACHE_CENSUS
	storage_->programs.census_liveness(live);
#endif
	try {
		/* The address spaces the gadget-cache budget asked to shed,
		 * first. */
		if (storage_->programs.leave_shedding())
			storage_->cache_ready = false;
		if (retirements == storage_->retirements_seen)
			return;
		/* A cache that lost the address space it last prepared must
		 * prepare again before it is used. */
		if (storage_->programs.leave_gone(live))
			storage_->cache_ready = false;
		storage_->retirements_seen = retirements;
	} catch (...) {
		/* Out of memory part way, so some may still be held: the next
		 * call asks again, and the cache prepares afresh meanwhile,
		 * since the one last prepared may be among those let go of. */
		storage_->cache_ready = false;
	}
}

NativeTlbEntry *Executor::tlb_entries() noexcept
{
	return storage_->entries.data();
}

std::vector<std::uintptr_t> &Executor::checked_stream() noexcept
{
	return storage_->checked_stream;
}

bool Executor::page_holds_cached_code(std::uint64_t address) noexcept
{
	return storage_->prepared && storage_->cache_ready &&
		storage_->programs.page_holds_code(address);
}

CodePageRegistry *Executor::code_page_registry() const noexcept
{
	return storage_->registries.current.get();
}

void Executor::enter_engine() noexcept
{
	storage_->registries.participant.engine_epoch.fetch_add(
		1, std::memory_order_seq_cst);
}

void Executor::leave_engine() noexcept
{
	storage_->registries.participant.engine_epoch.fetch_add(
		1, std::memory_order_release);
}

void Executor::check_exceptions(unsigned checked) noexcept
{
	storage_->checked = checked;
	storage_->programs.set_checked(checked);
}

void Executor::begin_run() noexcept
{
	if (storage_->announcement != nullptr)
		announce_entry(*storage_->announcement);
}

void Executor::end_run() noexcept
{
	if (storage_->announcement != nullptr)
		announce_exit(*storage_->announcement);
}

unsigned Executor::checked_exceptions() const noexcept
{
	return storage_->checked;
}

void *Executor::execution_context() const noexcept
{
	return storage_->execution_context;
}

std::uint64_t Executor::prepared_code_changes() const noexcept
{
	return storage_->code_changes;
}

std::uint64_t Executor::current_code_changes() const noexcept
{
	if (storage_->execution_context == nullptr ||
	    storage_->sequence_reader == nullptr)
		return storage_->code_changes;
	return storage_->sequence_reader(
		storage_->execution_context).code_changes;
}

void Executor::invalidate_cached_code(
	std::uint64_t address, std::size_t size) noexcept
{
	if (storage_->prepared && storage_->cache_ready)
		storage_->programs.invalidate_code(address, size);
}

bool Executor::accept_code_changes(std::uint64_t code_changes) noexcept
{
	if (!storage_->prepared || !storage_->cache_ready)
		return false;
	storage_->programs.accept_code_changes(code_changes);
	storage_->code_changes = code_changes;
	return true;
}

const TargetLinkEntry *Executor::target_links(ExecutionMode mode) noexcept
{
	return storage_->programs.target_links(mode);
}

ProgramSelection Executor::select_program(
	::uml::memory::GuestAddressSpace &memory, std::uint64_t address,
	ExecutionMode mode, std::size_t maximum_instructions) noexcept
{
	if (!storage_->prepared || storage_->memory != &memory)
		prepare(memory);
	storage_->transient_program.reset();
	address = normalize_instruction_pointer(mode, address);
	if (storage_->cache_ready) {
		bool cache_hit = false;
		if (const auto *program = storage_->programs.find(address, mode,
			maximum_instructions, storage_->code_changes,
			storage_->mapping_changes, memory, storage_->execution_context,
			storage_->sequence_reader, storage_->diagnostics, cache_hit);
			program != nullptr) {
			return {
				.status = BuildStatus::built,
				.program = program,
				.fault_address = 0,
				.cache_hit = cache_hit,
			};
		}
	}

	++storage_->diagnostics.translations;
	ProgramLinkResolver links{};
	if (storage_->cache_ready) {
		links = {
			.context = &storage_->programs,
			.resolve = [](void *context, std::uint64_t target,
				ExecutionMode target_mode) {
				return static_cast<ProgramCache *>(context)->link(
					target, target_mode);
			},
			.resolve_indirect = [](void *context, std::uint64_t source,
				ExecutionMode source_mode) {
				return static_cast<ProgramCache *>(context)->indirect_link(
					source, source_mode);
			},
		};
	}
	const CodePageHold hold{
		storage_->cache_ready ? &storage_->registries : nullptr, address,
		mode};
	/* The address space counts the stores that land on these pages from
	 * here until the program is stored, so that a build a store landed
	 * across is not kept for others to adopt. After the hold, so that every
	 * such store is published, and before the decoder reads a byte. */
	std::optional<ProgramCache::Pinned> pinned;
	if (storage_->cache_ready)
		try {
			pinned.emplace(storage_->programs.pin(hold.pages()));
		} catch (...) {
			/* Not cached, and run once. */
		}
	/* Before the decoder reads a byte, and after the hold, so a store another
	 * executor has already translated for one of these pages either lands
	 * ahead of the read or moves the stamp this reads. */
	const auto held_stamps = capture_held_code_stamps(
		memory, hold.pages(), storage_->execution_context);
	auto built = build(memory, address, mode, maximum_instructions, links,
		storage_->checked);
	if (built.status != BuildStatus::built) {
		storage_->programs.cancel_selection();
		return {
			.status = built.status,
			.program = nullptr,
			.fault_address = built.fault_address,
			.cache_hit = false,
		};
	}

	const auto use_transient = [&]() noexcept {
		storage_->programs.cancel_selection();
		storage_->transient_program.emplace(std::move(built.program));
		return ProgramSelection{
			.status = BuildStatus::built,
			.program = &*storage_->transient_program,
			.fault_address = 0,
			.cache_hit = false,
		};
	};
	if (!storage_->cache_ready || !pinned ||
		built.program.byte_count() == 0 ||
		built.program.byte_count() > maximum_cached_source_bytes)
		return use_transient();
	const auto code_stamps = program_code_stamps_from_hold(
		built.program, hold.pages(), held_stamps);
	if (!code_stamps)
		return use_transient();

	try {
		const auto *stored = storage_->programs.store(built.program,
			storage_->code_changes, maximum_instructions, *code_stamps,
			pinned->pins(), storage_->diagnostics);
		if (stored == nullptr)
			return use_transient();
		return {
			.status = BuildStatus::built,
			.program = stored,
			.fault_address = 0,
			.cache_hit = false,
		};
	} catch (...) {
		return use_transient();
	}
}

const Program *Executor::program_holding([[maybe_unused]] std::uint64_t address,
	[[maybe_unused]] ExecutionMode mode, const std::uintptr_t *word) const noexcept
{
	if (const auto &transient = storage_->transient_program; transient) {
		const auto stream = transient->native_stream();
		if (word >= stream.data() && word < stream.data() + stream.size())
			return &*transient;
	}
	return storage_->programs.holding(word);
}

void Executor::train_indirect(IndirectLink *site, std::uint64_t target,
	ExecutionMode mode) noexcept
{
	if (site == nullptr || !storage_->cache_ready)
		return;
	try {
		storage_->programs.learn_indirect(*site, target, mode);
	} catch (...) {
		/* Training is optional. Allocation pressure must preserve the cold
		 * indirect exit rather than changing architectural execution. */
	}
}

const ProgramCacheDiagnostics &Executor::cache_diagnostics() const noexcept
{
	return storage_->diagnostics;
}

std::size_t Executor::resident_generations() const noexcept
{
	return storage_->programs.resident_generations();
}

} // namespace uml::cpu::x86_64::gadgets
