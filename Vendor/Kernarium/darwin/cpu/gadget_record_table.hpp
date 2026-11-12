// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "gadget_cache_budget.hpp"
#include "memory/domain_heap.hpp"

namespace uml::cpu::gadget_cache {

/* Records that translated streams point at, found by a 64-bit key.
 *
 * A link or indirect record is named by address from generated code, so it may
 * never move. That makes a node-based hash map the wrong shape twice over:
 * every record costs a node header and a bucket, and a pooled allocator behind
 * it rounds the node up to a power of two and keeps every bucket array a
 * rehash ever outgrew. Measured on a multi-core compile, a 64-byte arm64 link
 * record cost 234 bytes that way.
 *
 * Here records are carved from chunks, and the index is one open-addressed
 * array that is replaced whole when it grows or is swept. The key is read back
 * out of the record (`KeyOf`), so an index entry is 32 bits: the record's
 * number -- its place in the order the chunks carve records -- with the top
 * bits of the key's hash beside it so that a probe only resolves a record
 * whose tag already matches. Chunk sizes follow a fixed sequence, so a number
 * names its chunk and slot arithmetically.
 *
 * Nothing is erased one record at a time, because only the owner knows what
 * still names a record. It sweeps instead: it marks every record anything it
 * holds may name (`Marks`), and `sweep` releases the rest. A released place is
 * reused, lowest number first, by the next record made, and chunks at the end
 * that hold no record any more are returned, so the table follows what is
 * named rather than everything that ever was.
 *
 * A sweep is three steps, so that an owner whose records are still reachable
 * while it decides can hold back the last one. `presence` says which records
 * exist, and are therefore the only ones a sweep may release: a record made
 * after it is never a candidate, whatever the marks say, so an owner may go on
 * making records while it marks. `unlink` takes the unmarked candidates out of
 * the index, so no lookup finds them, and leaves their memory as it was.
 * `release` zeroes them and returns their places. `sweep` is the three in a
 * row, for an owner that nothing else reaches while it runs.
 *
 * A table given an Account (`meter`) charges it every change in bytes(), so
 * the account holds exactly what the table does.
 *
 * A record type may shape its chunks. `Record::chunk_prefix` bytes come before
 * a chunk's first record, which `Record::prepare_chunk(storage)` initializes
 * when the chunk is made; a released place zeroes only the record's first
 * `Record::cleared_bytes`; and `Record::Companion`, when the type names one, is
 * a value kept for each record apart from it, in an array behind the chunk's
 * records, reached by the record's number (`companion_at`). That lets a record
 * that generated code reads keep only what the code reads, and lets a record
 * hold words that belong to its neighbour.
 */
template <typename Record>
struct RecordCompanion final {
	struct None final {};
	using type = None;
	static constexpr std::size_t bytes = 0;
};

template <typename Record>
	requires requires { typename Record::Companion; }
struct RecordCompanion<Record> final {
	using type = typename Record::Companion;
	static constexpr std::size_t bytes = sizeof(type);
};

template <typename Record, typename KeyOf>
class StableRecordTable final {
	static_assert(std::is_trivially_destructible_v<Record>,
		"records are released with their chunk, without a destructor");
	static_assert(alignof(Record) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
		"chunks come from the ordinary array allocation");

	static constexpr std::size_t prefix_bytes = [] {
		if constexpr (requires { Record::chunk_prefix; })
			return std::size_t{Record::chunk_prefix};
		else
			return std::size_t{0};
	}();
	static constexpr std::size_t cleared_bytes = [] {
		if constexpr (requires { Record::cleared_bytes; })
			return std::size_t{Record::cleared_bytes};
		else
			return sizeof(Record);
	}();
	static_assert(prefix_bytes % alignof(Record) == 0 &&
		cleared_bytes <= sizeof(Record));

public:
	using Companion = typename RecordCompanion<Record>::type;
	static constexpr std::size_t companion_bytes =
		RecordCompanion<Record>::bytes;
	static_assert(std::is_trivially_destructible_v<Companion> &&
		alignof(Companion) <= alignof(Record));

	/* The records a sweep keeps, found from their addresses, over one table
	 * or several (marks_of), so that a value can be tried against every table
	 * an owner keeps at once. They also remember which records each table
	 * held when they were taken (presence), which are the only ones the
	 * sweep can release. */
	class Marks final {
	public:
		/* Keep the record that begins at `address`. Any other value --
		 * one inside a record, past the records made, or outside the
		 * tables -- is ignored, so a caller may pass every value that
		 * might name a record, at the cost of keeping a few that
		 * nothing names. */
		void mark(std::uintptr_t address) noexcept
		{
			if (const auto found = locate(address); found.table != absent)
				tables_[found.table].bits[found.ordinal / 64] |=
					std::uint64_t{1} << (found.ordinal % 64);
		}

		void mark(const Record *record) noexcept
		{
			mark(reinterpret_cast<std::uintptr_t>(record));
		}

		[[nodiscard]] bool marked(const Record *record) const noexcept
		{
			const auto found = locate(
				reinterpret_cast<std::uintptr_t>(record));
			return found.table != absent &&
				test(tables_[found.table].bits, found.ordinal);
		}

	private:
		friend class StableRecordTable;

		struct Range final {
			std::uintptr_t begin;
			std::uintptr_t end;
			std::size_t first;
			std::size_t table;
		};

		struct Table final {
			const StableRecordTable *table;
			std::vector<std::uint64_t> bits;
			std::vector<std::uint64_t> present;
		};

		struct Found final {
			std::size_t table;
			std::size_t ordinal;
		};

		static constexpr std::size_t absent = ~std::size_t{};

		[[nodiscard]] Found locate(std::uintptr_t address) const noexcept
		{
			if (address < lowest_ || address >= highest_)
				return {absent, 0};
			auto found = std::upper_bound(ranges_.begin(), ranges_.end(),
				address, [](std::uintptr_t value, const Range &range) {
					return value < range.begin;
				});
			if (found == ranges_.begin())
				return {absent, 0};
			--found;
			if (address >= found->end)
				return {absent, 0};
			const auto offset = address - found->begin;
			if (offset % sizeof(Record) != 0)
				return {absent, 0};
			return {found->table, found->first + offset / sizeof(Record)};
		}

		[[nodiscard]] static bool test(const std::vector<std::uint64_t> &bits,
			std::size_t ordinal) noexcept
		{
			return (bits[ordinal / 64] >> (ordinal % 64) & 1U) != 0;
		}

		[[nodiscard]] const Table &of(
			const StableRecordTable *table) const noexcept
		{
			const auto found = std::find_if(tables_.begin(), tables_.end(),
				[table](const Table &entry) {
					return entry.table == table;
				});
			assert(found != tables_.end());
			return *found;
		}

		/* Every table's chunks by address, each over the records it has
		 * carved. */
		std::vector<Range> ranges_;
		std::uintptr_t lowest_{};
		std::uintptr_t highest_{};
		std::vector<Table> tables_;
	};

	/* Records `unlink` took out of the index, whose places are not yet free:
	 * their memory is as it was until `release` returns it. */
	class Unlinked final {
	public:
		[[nodiscard]] std::size_t size() const noexcept
		{
			return records_.size();
		}
		[[nodiscard]] bool empty() const noexcept { return records_.empty(); }
		[[nodiscard]] std::span<Record *const> records() const noexcept
		{
			return records_;
		}

	private:
		friend class StableRecordTable;

		std::vector<Record *> records_;
		/* Their numbers, as the index holds them. */
		std::vector<std::uint32_t> numbers_;
	};

	StableRecordTable() = default;
	StableRecordTable(const StableRecordTable &) = delete;
	StableRecordTable &operator=(const StableRecordTable &) = delete;
	StableRecordTable(StableRecordTable &&) = delete;
	StableRecordTable &operator=(StableRecordTable &&) = delete;
	~StableRecordTable() = default;

	[[nodiscard]] Record *find(std::uint64_t key) const noexcept
	{
		const auto number = find_number(key);
		return number == 0 ? nullptr : record_at(number);
	}

	/* The number of the record for `key`, or zero: one-based, and the
	 * record's for as long as it lives. */
	[[nodiscard]] std::uint32_t find_number(std::uint64_t key) const noexcept
	{
		if (index_ == nullptr)
			return 0;
		const auto hash = mix(key);
		const auto tag = tag_of(hash);
		for (auto position = hash & mask_;; position = (position + 1) & mask_) {
			const auto entry = index_[position];
			if (entry == 0)
				return 0;
			if ((entry & tag_mask) == tag) {
				const auto number = entry & ~tag_mask;
				if (KeyOf{}(*record_at(number)) == key)
					return number;
			}
		}
	}

	/* The record for `key`, constructed from `arguments` when absent. */
	template <typename... Arguments>
	[[nodiscard]] std::pair<Record *, bool> try_emplace(
		std::uint64_t key, Arguments &&...arguments)
	{
		const auto [number, inserted] = try_emplace_number(key,
			std::forward<Arguments>(arguments)...);
		return {record_at(number), inserted};
	}

	/* try_emplace() by number; a new record's companion is
	 * value-initialized. */
	template <typename... Arguments>
	[[nodiscard]] std::pair<std::uint32_t, bool> try_emplace_number(
		std::uint64_t key, Arguments &&...arguments)
	{
		/* A place is taken before its record is constructed, which a
		 * constructor that threw would leave taken and unindexed. */
		static_assert(std::is_nothrow_constructible_v<Record, Arguments...>);
		if (const auto found = find_number(key); found != 0)
			return {found, false};
		const ChargeScope charged{meter_, [this] { return bytes(); }};
		if ((size_ + 1) * 4 > capacity() * 3)
			grow();
		const auto ordinal = take();
		[[maybe_unused]] auto *record = new (place(ordinal)) Record(
			std::forward<Arguments>(arguments)...);
		if constexpr (companion_bytes != 0)
			new (companion_place(ordinal)) Companion{};
		assert(KeyOf{}(*record) == key);
		const auto number = static_cast<std::uint32_t>(ordinal + 1U);
		insert(number, mix(key));
		++size_;
		return {number, true};
	}

	/* The record with one-based number `number`, which must live. */
	[[nodiscard]] Record *record_at(std::uint32_t number) const noexcept
	{
		return std::launder(reinterpret_cast<Record *>(place(number - 1U)));
	}

	/* The companion of the record with number `number`. */
	[[nodiscard]] Companion &companion_at(std::uint32_t number) const noexcept
		requires(companion_bytes != 0)
	{
		return *std::launder(reinterpret_cast<Companion *>(
			companion_place(number - 1U)));
	}

	[[nodiscard]] std::size_t size() const noexcept { return size_; }

	/* What the table holds: its chunks, its index and its map of the
	 * places a sweep left free. */
	[[nodiscard]] std::size_t bytes() const noexcept
	{
		return capacity() * sizeof(std::uint32_t) +
			chunks_.capacity() * sizeof(Chunk) +
			vacant_.capacity() * sizeof(std::uint64_t) + record_bytes_;
	}

	/* Charge `account`, null for none, with what the table holds from here
	 * on, and refund the account it charged until now. */
	void meter(Account *account) noexcept
	{
		if (meter_ != nullptr)
			meter_->charge(-static_cast<std::ptrdiff_t>(bytes()));
		meter_ = account;
		if (meter_ != nullptr)
			meter_->charge(static_cast<std::ptrdiff_t>(bytes()));
	}

	/* Every record, in the order of their numbers. */
	template <typename Visitor>
	void for_each(Visitor &&visit) const
	{
		for (std::size_t ordinal = 0; ordinal < carved_; ++ordinal)
			if (!vacant(ordinal))
				visit(*std::launder(reinterpret_cast<const Record *>(
					place(ordinal))));
	}

	template <typename Visitor>
	void for_each(Visitor &&visit)
	{
		for (std::size_t ordinal = 0; ordinal < carved_; ++ordinal)
			if (!vacant(ordinal))
				visit(*std::launder(reinterpret_cast<Record *>(
					place(ordinal))));
	}

	/* Marks over the records made so far, none of them kept yet. A record
	 * made after them is not covered, so it is not a candidate either: a
	 * sweep with these marks keeps it. The caller holds whatever keeps the
	 * table from changing while they are taken. */
	[[nodiscard]] Marks marks() const
	{
		const StableRecordTable *self = this;
		return marks_of(std::span{&self, 1});
	}

	/* marks() over several tables of one owner, each swept with them. */
	[[nodiscard]] static Marks marks_of(
		std::span<const StableRecordTable *const> tables)
	{
		Marks marks;
		marks.tables_.reserve(tables.size());
		std::size_t chunks = 0;
		for (const auto *table : tables)
			chunks += table->chunks_.size();
		marks.ranges_.reserve(chunks);
		for (const auto *table : tables) {
			const auto index = marks.tables_.size();
			marks.tables_.push_back({table, std::vector<std::uint64_t>(
				(table->carved_ + 63) / 64), table->presence()});
			std::size_t first = 0;
			for (const auto &chunk : table->chunks_) {
				const auto begin = reinterpret_cast<std::uintptr_t>(
					chunk.storage + prefix_bytes);
				const auto used = &chunk == &table->chunks_.back() ?
					table->used_ : chunk.count;
				marks.ranges_.push_back({begin,
					begin + used * sizeof(Record), first, index});
				first += chunk.count;
			}
		}
		std::sort(marks.ranges_.begin(), marks.ranges_.end(),
			[](const auto &left, const auto &right) {
				return left.begin < right.begin;
			});
		if (!marks.ranges_.empty()) {
			marks.lowest_ = marks.ranges_.front().begin;
			for (const auto &range : marks.ranges_)
				marks.highest_ = std::max(marks.highest_, range.end);
		}
		return marks;
	}

	/* The records that exist now, one bit per number: the only ones a sweep
	 * can release. Places a sweep left free, and places taken later, are not
	 * among them. */
	[[nodiscard]] std::vector<std::uint64_t> presence() const
	{
		std::vector<std::uint64_t> present((carved_ + 63) / 64);
		for (std::size_t position = 0; position < capacity(); ++position)
			if (const auto entry = index_[position]; entry != 0) {
				const auto ordinal = (entry & ~tag_mask) - 1U;
				present[ordinal / 64] |= std::uint64_t{1} << (ordinal % 64);
			}
		return present;
	}

	/* Take every record `marks` does not keep, and had when it was taken,
	 * out of the index, and return them. They are found by no lookup from
	 * here on, and a record made for the same key is made anew; the places
	 * and their contents stay until `release`. Everything this allocates
	 * comes before anything changes, so it happens whole or, when its new
	 * index cannot be allocated, not at all. */
	[[nodiscard]] Unlinked unlink(const Marks &marks)
	{
		const auto &table = marks.of(this);
		assert(table.bits.size() == table.present.size());
		const auto doomed = [&](std::uint32_t entry) noexcept {
			const auto ordinal = (entry & ~tag_mask) - 1U;
			return ordinal / 64 < table.present.size() &&
				Marks::test(table.present, ordinal) &&
				!Marks::test(table.bits, ordinal);
		};
		std::size_t gone = 0;
		for (std::size_t position = 0; position < capacity(); ++position)
			if (const auto entry = index_[position];
			    entry != 0 && doomed(entry))
				++gone;
		Unlinked unlinked;
		if (gone == 0)
			return unlinked;
		const ChargeScope charged{meter_, [this] { return bytes(); }};
		const auto remaining = size_ - gone;
		std::vector<std::uint32_t> kept;
		kept.reserve(remaining);
		unlinked.records_.reserve(gone);
		unlinked.numbers_.reserve(gone);
		std::size_t next = first_capacity;
		while (remaining * 4 > next * 3)
			next *= 2;
		auto replacement = remaining == 0 ? Index{} :
			::uml::memory::make_leased_array<std::uint32_t>(next, true);

		for (std::size_t position = 0; position < capacity(); ++position) {
			const auto entry = index_[position];
			if (entry == 0)
				continue;
			const auto number = entry & ~tag_mask;
			if (doomed(entry)) {
				unlinked.records_.push_back(record_at(number));
				unlinked.numbers_.push_back(number);
			} else {
				kept.push_back(number);
			}
		}
		index_ = std::move(replacement);
		mask_ = index_ == nullptr ? 0 : next - 1;
		size_ = kept.size();
		for (const auto number : kept)
			insert(number, mix(KeyOf{}(*record_at(number))));
		return unlinked;
	}

	/* Free the places of records `unlink` took, zeroing them so that a
	 * reference their owner failed to mark faults rather than reaching
	 * whatever is made there next, and return the chunks at the end that
	 * hold no record any more. */
	std::size_t release(const Unlinked &unlinked) noexcept
	{
		const ChargeScope charged{meter_, [this] { return bytes(); }};
		for (const auto number : unlinked.numbers_)
			free_place(number - 1U);
		lowest_vacant_ = 0;
		trim();
		return unlinked.numbers_.size();
	}

	/* Release every record `marks` does not keep, and return how many went:
	 * unlink and release in a row. */
	std::size_t sweep(const Marks &marks)
	{
		return release(unlink(marks));
	}

private:
	/* Records per chunk: small for the many short-lived address spaces a
	 * desktop guest creates, doubling to a bound so a long-lived one wastes at
	 * most one partly used chunk of that size. The chunks that double before
	 * reaching the bound hold `doubling_records` between them; every chunk
	 * after them holds `largest_chunk`. */
	static constexpr std::size_t first_chunk = 32;
	static constexpr std::size_t stride_bytes =
		sizeof(Record) + companion_bytes;
	static constexpr std::size_t largest_chunk =
		std::max<std::size_t>(first_chunk, (std::size_t{128} << 10) / stride_bytes);
	static constexpr unsigned doubling_chunks = static_cast<unsigned>(
		std::bit_width((largest_chunk - 1U) / first_chunk));
	static constexpr std::size_t doubling_records =
		first_chunk * ((std::size_t{1} << doubling_chunks) - 1U);
	static constexpr std::size_t first_capacity = 64;
	/* An entry is a record number, one-based so that zero is empty, below a
	 * six-bit tag. That numbers more records than a table could hold in
	 * any memory the host would give it -- 2.5 GiB of 40-byte ones -- while
	 * a probe still resolves only one mismatched record in 64. */
	static constexpr unsigned number_bits = 26;
	static constexpr std::uint32_t tag_mask =
		~((std::uint32_t{1} << number_bits) - 1U);
	static constexpr std::size_t maximum_records =
		(std::size_t{1} << number_bits) - 1U;

	/* What a chunk of `count` records takes: its prefix, the records and
	 * their companions. */
	[[nodiscard]] static constexpr std::size_t chunk_bytes(
		std::size_t count) noexcept
	{
		return prefix_bytes + count * stride_bytes;
	}

	/* A chunk's storage comes from the engine's leased heap. Its size is the
	 * chunk's own count, so that the deleting is stateless and a Chunk stays
	 * two words. */
	struct Chunk final {
		Chunk(std::byte *bytes, std::size_t records) noexcept
			: storage{bytes}, count{records}
		{
		}
		Chunk(Chunk &&other) noexcept
			: storage{std::exchange(other.storage, nullptr)},
			  count{other.count}
		{
		}
		Chunk &operator=(Chunk &&other) noexcept
		{
			if (this != &other) {
				drop();
				storage = std::exchange(other.storage, nullptr);
				count = other.count;
			}
			return *this;
		}
		Chunk(const Chunk &) = delete;
		Chunk &operator=(const Chunk &) = delete;
		~Chunk() { drop(); }

		std::byte *storage;
		std::size_t count;

	private:
		void drop() noexcept
		{
			if (storage != nullptr)
				::uml::memory::HeapLease::heap().deallocate(
					storage, chunk_bytes(count),
					alignof(Record));
		}
	};

	/* A full finalizer, not one multiply: the engines shard their tables
	 * by bits of one multiply of the same key, so a key below 2^31 would
	 * otherwise reach only the index positions its shard's other keys
	 * reach, and the index would fill one eighth of itself. */
	[[nodiscard]] static std::uint64_t mix(std::uint64_t key) noexcept
	{
		key ^= key >> 33;
		key *= 0xff51afd7ed558ccdULL;
		key ^= key >> 33;
		key *= 0xc4ceb9fe1a85ec53ULL;
		return key ^ (key >> 33);
	}

	[[nodiscard]] static std::uint32_t tag_of(std::uint64_t hash) noexcept
	{
		return static_cast<std::uint32_t>(hash >> 32) & tag_mask;
	}

	/* The ordinal of the first record chunk `chunk` holds. */
	[[nodiscard]] static std::size_t chunk_first(std::size_t chunk) noexcept
	{
		return chunk < doubling_chunks ?
			first_chunk * ((std::size_t{1} << chunk) - 1U) :
			doubling_records + (chunk - doubling_chunks) * largest_chunk;
	}

	struct Position final {
		std::size_t chunk;
		std::size_t slot;
	};

	/* Which chunk holds zero-based ordinal `ordinal`, and where in it. */
	[[nodiscard]] static Position position_of(std::size_t ordinal) noexcept
	{
		if (ordinal < doubling_records) {
			/* Chunk k begins at first_chunk * (2^k - 1). */
			const auto chunk = static_cast<std::size_t>(
				std::bit_width(ordinal / first_chunk + 1U)) - 1U;
			return {chunk, ordinal -
				first_chunk * ((std::size_t{1} << chunk) - 1U)};
		}
		const auto beyond = ordinal - doubling_records;
		return {doubling_chunks + beyond / largest_chunk,
			beyond % largest_chunk};
	}

	/* Where the record with zero-based ordinal `ordinal` lives. */
	[[nodiscard]] std::byte *place(std::size_t ordinal) const noexcept
	{
		const auto at = position_of(ordinal);
		return chunks_[at.chunk].storage + prefix_bytes +
			at.slot * sizeof(Record);
	}

	/* Where its companion lives, behind the chunk's records. */
	[[nodiscard]] std::byte *companion_place(
		std::size_t ordinal) const noexcept
	{
		const auto at = position_of(ordinal);
		const auto &chunk = chunks_[at.chunk];
		return chunk.storage + prefix_bytes +
			chunk.count * sizeof(Record) + at.slot * companion_bytes;
	}

	[[nodiscard]] bool vacant(std::size_t ordinal) const noexcept
	{
		return (vacant_[ordinal / 64] >> (ordinal % 64) & 1U) != 0;
	}

	[[nodiscard]] std::size_t capacity() const noexcept
	{
		return index_ == nullptr ? 0 : mask_ + 1;
	}

	void insert(std::uint32_t number, std::uint64_t hash) noexcept
	{
		assert(number != 0 && (number & tag_mask) == 0);
		auto position = hash & mask_;
		while (index_[position] != 0)
			position = (position + 1) & mask_;
		index_[position] = tag_of(hash) | number;
	}

	void grow()
	{
		const auto next = std::max<std::size_t>(first_capacity, capacity() * 2);
		auto replacement =
			::uml::memory::make_leased_array<std::uint32_t>(next, true);
		auto previous = std::move(index_);
		const auto previous_capacity = previous == nullptr ? 0 : mask_ + 1;
		index_ = std::move(replacement);
		mask_ = next - 1;
		for (std::size_t position = 0; position < previous_capacity; ++position)
			if (const auto entry = previous[position]; entry != 0) {
				const auto number = entry & ~tag_mask;
				insert(number, mix(KeyOf{}(*record_at(number))));
			}
	}

	/* Take a place for a record: the lowest one a sweep left free, or else
	 * a new one carved past the others. */
	[[nodiscard]] std::size_t take()
	{
		if (vacancies_ != 0) {
			auto word = lowest_vacant_;
			while (vacant_[word] == 0)
				++word;
			lowest_vacant_ = word;
			const auto bit = static_cast<std::size_t>(
				std::countr_zero(vacant_[word]));
			vacant_[word] &= vacant_[word] - 1U;
			--vacancies_;
			return word * 64 + bit;
		}
		if (carved_ == maximum_records)
			throw std::bad_alloc{};
		/* Everything that can fail comes before the place is taken. */
		if (carved_ / 64 == vacant_.size())
			vacant_.push_back(0);
		if (chunks_.empty() || used_ == chunks_.back().count) {
			const auto count = chunks_.empty() ? first_chunk :
				std::min(largest_chunk, chunks_.back().count * 2);
			/* Reserve before allocating, so a failed push cannot leak
			 * the chunk. */
			if (chunks_.size() == chunks_.capacity())
				chunks_.reserve(std::max<std::size_t>(
					8, chunks_.size() * 2));
			chunks_.emplace_back(static_cast<std::byte *>(
				::uml::memory::HeapLease::heap().allocate(
					chunk_bytes(count), alignof(Record))),
				count);
			if constexpr (requires(std::byte *storage) {
					      Record::prepare_chunk(storage);
				      })
				Record::prepare_chunk(chunks_.back().storage);
			record_bytes_ += chunk_bytes(count);
			used_ = 0;
		}
		++used_;
		return carved_++;
	}

	void free_place(std::size_t ordinal) noexcept
	{
		assert(!vacant(ordinal));
		std::memset(static_cast<void *>(place(ordinal)), 0, cleared_bytes);
		if constexpr (companion_bytes != 0)
			std::memset(static_cast<void *>(
				companion_place(ordinal)), 0, companion_bytes);
		vacant_[ordinal / 64] |= std::uint64_t{1} << (ordinal % 64);
		++vacancies_;
	}

	/* Return the chunks at the end that no longer hold a record. */
	void trim() noexcept
	{
		while (!chunks_.empty()) {
			const auto first = chunk_first(chunks_.size() - 1U);
			for (auto ordinal = first; ordinal < carved_; ++ordinal)
				if (!vacant(ordinal))
					return;
			for (auto ordinal = first; ordinal < carved_; ++ordinal)
				vacant_[ordinal / 64] &=
					~(std::uint64_t{1} << (ordinal % 64));
			vacancies_ -= carved_ - first;
			carved_ = first;
			record_bytes_ -= chunk_bytes(chunks_.back().count);
			chunks_.pop_back();
			used_ = chunks_.empty() ? 0 : chunks_.back().count;
			vacant_.resize((carved_ + 63) / 64);
		}
	}

	/* First, so it ends after everything below that the heap holds. */
	::uml::memory::HeapLease lease_;
	std::vector<Chunk> chunks_;
	/* Places carved from the last chunk, and from all of them. */
	std::size_t used_{};
	std::size_t carved_{};
	/* A bit per carved place, set while it is free; the lowest word that
	 * may have one set, and how many are. */
	std::vector<std::uint64_t> vacant_;
	std::size_t lowest_vacant_{};
	std::size_t vacancies_{};
	using Index = ::uml::memory::LeasedArray<std::uint32_t>;
	Index index_;
	std::size_t mask_{};
	std::size_t size_{};
	/* The bytes of the record chunks, and the account they are charged
	 * to. */
	std::size_t record_bytes_{};
	Account *meter_{};
};

} // namespace uml::cpu::gadget_cache
