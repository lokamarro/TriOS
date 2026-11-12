// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/gadget_record_table.hpp"
#include "memory/domain_heap.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cache = uml::cpu::gadget_cache;

namespace {

using uml::testing::require;

/* A table's chunks double from 32 records until they would reach the 128 KiB
 * that bounds them. At 24 bytes that bound is 5461 records, which no doubling
 * lands on; at 1 KiB it is 128, which the third chunk reaches exactly; at
 * 8 KiB a chunk never grows past its first 32 records. */
template <std::size_t Bytes>
struct Sized final {
	explicit Sized(std::uint64_t value) noexcept
		: key{value}
	{
		payload.fill(static_cast<std::uint8_t>(value));
	}

	std::uint64_t key;
	std::array<std::uint8_t, Bytes - sizeof(std::uint64_t)> payload{};
};

struct KeyOf final {
	template <typename Record>
	[[nodiscard]] std::uint64_t operator()(
		const Record &record) const noexcept
	{
		return record.key;
	}
};

[[nodiscard]] std::uint64_t key_of(std::uint64_t index) noexcept
{
	return index * 0x9e37'79b9'7f4a'7c15ULL;
}

/* Every record stays where it was made, however far the table has grown,
 * and each key finds its own. */
template <typename Record>
void exercise(std::uint64_t count)
{
	cache::StableRecordTable<Record, KeyOf> table;
	require(table.find(key_of(1)) == nullptr);
	std::vector<Record *> records;
	for (std::uint64_t index = 0; index < count; ++index) {
		const auto key = key_of(index);
		const auto [record, inserted] = table.try_emplace(key, key);
		require(inserted && record->key == key);
		records.push_back(record);
	}
	require(table.size() == count);
	for (std::uint64_t index = 0; index < count; ++index) {
		const auto key = key_of(index);
		require(table.find(key) == records[index]);
		require(records[index]->payload.back() ==
			static_cast<std::uint8_t>(key));
		const auto [again, inserted] = table.try_emplace(key, key);
		require(!inserted && again == records[index]);
	}
	for (std::uint64_t index = count; index < count + 1000; ++index)
		require(table.find(key_of(index)) == nullptr);
	std::uint64_t visited = 0;
	table.for_each([&](const Record &record) {
		require(record.key == key_of(visited));
		++visited;
	});
	require(visited == count);
	require(table.bytes() >= count * sizeof(Record));
}

/* A sweep keeps exactly the records marked, where they were, and the next
 * records made take the places it released, lowest first, before any more
 * are carved. */
template <typename Record>
void sweep(std::uint64_t count)
{
	cache::StableRecordTable<Record, KeyOf> table;
	std::vector<Record *> records;
	for (std::uint64_t index = 0; index < count; ++index) {
		const auto key = key_of(index);
		records.push_back(table.try_emplace(key, key).first);
	}
	const auto full = table.bytes();
	auto marks = table.marks();
	for (std::uint64_t index = 0; index < count; index += 3)
		marks.mark(records[index]);
	/* Values that only resemble a record's address keep nothing. */
	marks.mark(reinterpret_cast<std::uintptr_t>(records[1]) + 8);
	marks.mark(std::uintptr_t{0});
	marks.mark(~std::uintptr_t{0});
	require(marks.marked(records[0]) && !marks.marked(records[1]));
	const auto kept = (count + 2) / 3;
	require(table.sweep(marks) == count - kept);
	require(table.size() == kept);
	for (std::uint64_t index = 0; index < count; ++index) {
		const auto *found = table.find(key_of(index));
		if (index % 3 != 0) {
			require(found == nullptr);
			continue;
		}
		require(found == records[index]);
		require(found->payload.back() ==
			static_cast<std::uint8_t>(key_of(index)));
	}
	std::uint64_t visited = 0;
	table.for_each([&](const Record &record) {
		require(record.key == key_of(visited * 3));
		++visited;
	});
	require(visited == kept);

	std::vector<Record *> released;
	for (std::uint64_t index = 0; index < count; ++index)
		if (index % 3 != 0)
			released.push_back(records[index]);
	for (std::uint64_t index = 0; index < count - kept; ++index) {
		const auto key = key_of(count + index);
		const auto [record, inserted] = table.try_emplace(key, key);
		require(inserted && record == released[index]);
	}
	require(table.size() == count);
	require(table.bytes() == full);
	for (std::uint64_t index = 0; index < count; index += 3)
		require(table.find(key_of(index)) == records[index]);

	/* Sweeping with everything marked releases nothing. */
	auto all = table.marks();
	table.for_each([&](const Record &record) { all.mark(&record); });
	require(table.sweep(all) == 0 && table.size() == count);
}

/* A sweep in its steps. Unlinking takes the unmarked records out of the index
 * and nothing else: no lookup finds them, a record made for the same key is
 * made anew in a place of its own, and what they held is still there for
 * whoever has not let go of them. Releasing zeroes them and frees their
 * places, which the next records take lowest first. */
template <typename Record>
void unlink_then_release(std::uint64_t count)
{
	cache::StableRecordTable<Record, KeyOf> table;
	std::vector<Record *> records;
	for (std::uint64_t index = 0; index < count; ++index) {
		const auto key = key_of(index);
		records.push_back(table.try_emplace(key, key).first);
	}
	auto marks = table.marks();
	for (std::uint64_t index = 0; index < count; index += 3)
		marks.mark(records[index]);
	const auto kept = (count + 2) / 3;

	auto unlinked = table.unlink(marks);
	require(unlinked.size() == count - kept && !unlinked.empty());
	require(table.size() == kept);
	std::vector<const Record *> doomed(unlinked.records().begin(),
		unlinked.records().end());
	std::sort(doomed.begin(), doomed.end());
	for (std::uint64_t index = 0; index < count; ++index) {
		const bool gone = index % 3 != 0;
		require(std::binary_search(doomed.begin(), doomed.end(),
			records[index]) == gone);
		require((table.find(key_of(index)) == nullptr) == gone);
		/* Nothing about a record changed. */
		require(records[index]->key == key_of(index));
		require(records[index]->payload.back() ==
			static_cast<std::uint8_t>(key_of(index)));
	}
	/* Their places are still taken: they are visited, and a record made
	 * for a key that went is not made in one of them. */
	std::uint64_t visited = 0;
	table.for_each([&](const Record &) { ++visited; });
	require(visited == count);
	const auto [fresh, inserted] = table.try_emplace(key_of(1), key_of(1));
	require(inserted && fresh->key == key_of(1));
	require(!std::binary_search(doomed.begin(), doomed.end(), fresh));
	require(table.size() == kept + 1);

	require(table.release(unlinked) == count - kept);
	visited = 0;
	table.for_each([&](const Record &) { ++visited; });
	require(visited == kept + 1);
	/* The first chunk holds the first record, which is kept, so the
	 * released ones in it are still readable, and zero. */
	require(records[1]->key == 0 && records[1]->payload.back() == 0);
	require(records[2]->key == 0 && records[2]->payload.back() == 0);
	for (std::uint64_t index = 0; index < count; index += 3)
		require(table.find(key_of(index)) == records[index] &&
			records[index]->payload.back() ==
				static_cast<std::uint8_t>(key_of(index)));
	/* The lowest places go first. */
	require(table.try_emplace(key_of(count), key_of(count)).first ==
		records[1]);
	require(table.try_emplace(key_of(count + 1), key_of(count + 1)).first ==
		records[2]);

	/* Nothing to take changes nothing. */
	auto everything = table.marks();
	table.for_each([&](const Record &record) { everything.mark(&record); });
	const auto before = table.size();
	require(table.unlink(everything).empty() && table.size() == before);
}

/* What a sweep may release is what existed when its marks were taken. A
 * record made afterwards is kept whatever the marks say, whether it takes a
 * place a sweep freed (inside the range the marks cover) or one carved past
 * them. */
void candidates()
{
	using Record = Sized<24>;
	using Table = cache::StableRecordTable<Record, KeyOf>;
	const auto count = [](const std::vector<std::uint64_t> &bits) {
		std::size_t total = 0;
		for (const auto word : bits)
			total += static_cast<std::size_t>(std::popcount(word));
		return total;
	};
	Table table;
	std::vector<Record *> records;
	for (std::uint64_t index = 0; index < 200; ++index)
		records.push_back(table.try_emplace(index, index).first);
	require(count(table.presence()) == 200);
	{
		auto marks = table.marks();
		for (std::uint64_t index = 0; index < 200; index += 2)
			marks.mark(records[index]);
		require(table.sweep(marks) == 100);
	}
	/* The places the sweep freed are not present. */
	require(count(table.presence()) == 100);

	auto marks = table.marks();
	std::vector<Record *> later;
	for (std::uint64_t index = 0; index < 150; ++index)
		later.push_back(table.try_emplace(1000 + index, 1000 + index).first);
	/* The first hundred took the freed places, the rest were carved. */
	require(std::count_if(later.begin(), later.end(),
		[&](const Record *record) {
			return std::find(records.begin(), records.end(), record) !=
				records.end();
		}) == 100);
	/* Nothing is marked: every record that existed goes, and no other. */
	auto unlinked = table.unlink(marks);
	require(unlinked.size() == 100);
	require(table.size() == 150);
	for (std::uint64_t index = 0; index < 150; ++index) {
		require(table.find(1000 + index) == later[index]);
		require(later[index]->key == 1000 + index);
	}
	for (std::uint64_t index = 0; index < 200; index += 2)
		require(table.find(index) == nullptr);
	require(table.release(unlinked) == 100);
	for (std::uint64_t index = 0; index < 150; ++index)
		require(table.find(1000 + index) == later[index] &&
			later[index]->payload.back() ==
				static_cast<std::uint8_t>(1000 + index));
	/* Marks taken before any record was made cover none of those made
	 * after. */
	Table fresh;
	auto early = fresh.marks();
	for (std::uint64_t index = 0; index < 50; ++index)
		require(fresh.try_emplace(index, index).second);
	require(fresh.sweep(early) == 0 && fresh.size() == 50);
}

/* Chunks at the end that a sweep empties are returned, and the table grows
 * past them again. */
void trim()
{
	using Record = Sized<1024>;
	cache::StableRecordTable<Record, KeyOf> table;
	std::vector<Record *> records;
	for (std::uint64_t index = 0; index < 1000; ++index)
		records.push_back(table.try_emplace(index, index).first);
	const auto full = table.bytes();
	auto marks = table.marks();
	for (std::uint64_t index = 0; index < 10; ++index)
		marks.mark(records[index]);
	require(table.sweep(marks) == 990 && table.size() == 10);
	require(table.bytes() < full / 16);
	for (std::uint64_t index = 1000; index < 3000; ++index)
		require(table.try_emplace(index, index).second);
	for (std::uint64_t index = 0; index < 10; ++index)
		require(table.find(index) == records[index]);
	for (std::uint64_t index = 10; index < 1000; ++index)
		require(table.find(index) == nullptr);
	for (std::uint64_t index = 1000; index < 3000; ++index)
		require(table.find(index)->key == index);

	/* Releasing everything leaves an empty table that still works. */
	require(table.sweep(table.marks()) == 2010 && table.size() == 0);
	require(table.find(0) == nullptr);
	require(table.bytes() < 1024);
	require(table.try_emplace(7, 7).second && table.find(7)->key == 7);
	require(table.sweep(table.marks()) == 1 && table.size() == 0);
	cache::StableRecordTable<Record, KeyOf> untouched;
	require(untouched.sweep(untouched.marks()) == 0);
}

/* One set of marks covers several tables, as an owner that splits its records
 * into shards keeps them: a value is tried against all of them at once, and
 * each table releases only its own records that were left unmarked. */
void several_tables()
{
	using Record = Sized<24>;
	using Table = cache::StableRecordTable<Record, KeyOf>;
	Table first;
	Table second;
	std::vector<Record *> kept;
	std::vector<Record *> gone;
	for (std::uint64_t index = 0; index < 3000; ++index) {
		auto &table = index % 2 == 0 ? first : second;
		auto *record = table.try_emplace(index, index).first;
		(index % 3 == 0 ? kept : gone).push_back(record);
	}
	const std::array<const Table *, 2> tables{&first, &second};
	auto marks = Table::marks_of(tables);
	for (auto *record : kept)
		marks.mark(reinterpret_cast<std::uintptr_t>(record));
	for (const auto *record : kept)
		require(marks.marked(record));
	for (const auto *record : gone)
		require(!marks.marked(record));
	/* Each table takes its own, in steps. */
	const auto first_gone = first.unlink(marks);
	const auto second_gone = second.unlink(marks);
	require(first_gone.size() + second_gone.size() == gone.size());
	require(first.release(first_gone) + second.release(second_gone) ==
		gone.size());
	require(first.size() + second.size() == kept.size());
	for (std::uint64_t index = 0; index < 3000; ++index) {
		const auto &table = index % 2 == 0 ? first : second;
		require((table.find(index) != nullptr) == (index % 3 == 0));
	}
}

/* A metered table charges its account exactly what bytes() counts, through
 * growth, sweeps in one step or two and the chunks they return, and refunds
 * it when it is metered elsewhere. */
template <typename Record>
void metered(std::uint64_t count)
{
	cache::Account account{[](void *) noexcept {}, nullptr};
	cache::StableRecordTable<Record, KeyOf> table;
	table.meter(&account);
	require(account.bytes() == table.bytes());
	std::vector<Record *> records;
	for (std::uint64_t index = 0; index < count; ++index) {
		records.push_back(table.try_emplace(index, index).first);
		require(account.bytes() == table.bytes());
	}
	auto marks = table.marks();
	for (std::uint64_t index = 0; index < count; index += 3)
		marks.mark(records[index]);
	const auto unlinked = table.unlink(marks);
	require(account.bytes() == table.bytes());
	require(table.release(unlinked) == count - (count + 2) / 3);
	require(account.bytes() == table.bytes());
	for (std::uint64_t index = count; index < 2 * count; ++index)
		(void)table.try_emplace(index, index);
	require(account.bytes() == table.bytes());
	const auto before = table.size();
	require(table.sweep(table.marks()) == before && table.size() == 0);
	require(account.bytes() == table.bytes());
	/* An empty table still holds its index, which it charges. */
	require(table.bytes() != 0);
	table.meter(nullptr);
	require(account.bytes() == 0);
	(void)table.try_emplace(1, 1);
	require(account.bytes() == 0);
}

/* The chunks and the index of every table come from the engine's leased
 * heap, which goes back to the region whole when the last table ends. */
void leased_heap()
{
	auto &region = uml::memory::HeapRegion::instance();
	auto &leased = uml::memory::LeasedHeap::engine();
	const auto committed = region.committed();
	require(leased.leases() == 0);
	{
		cache::StableRecordTable<Sized<24>, KeyOf> first;
		require(leased.leases() == 1);
		std::vector<Sized<24> *> records;
		for (std::uint64_t index = 0; index < 20'000; ++index)
			records.push_back(first.try_emplace(index, index).first);
		/* The records and the index are the heap's, not the zone's. */
		require(region.committed() >=
			committed + 20'000 * sizeof(Sized<24>));
		{
			cache::StableRecordTable<Sized<1024>, KeyOf> second;
			require(leased.leases() == 2);
			for (std::uint64_t index = 0; index < 500; ++index)
				(void)second.try_emplace(index, index);
		}
		/* One table ending leaves the other's records where they were. */
		require(leased.leases() == 1);
		for (std::uint64_t index = 0; index < 20'000; ++index)
			require(first.find(index) == records[index] &&
				records[index]->key == index);
		/* A sweep that returns every chunk keeps the heap for its table. */
		require(first.sweep(first.marks()) == 20'000);
		(void)first.try_emplace(1, 1);
		require(first.find(1) != nullptr);
	}
	require(leased.leases() == 0);
	require(region.committed() == committed,
		"the record tables left heap pages committed");
}

/* Counts the keys a lookup reads back, which is one per index entry whose
 * tag matched. */
struct CountingKeyOf final {
	static inline std::size_t reads = 0;

	template <typename Record>
	[[nodiscard]] std::uint64_t operator()(
		const Record &record) const noexcept
	{
		++reads;
		return record.key;
	}
};

/* The engines pick a record's shard from bits of one multiply of its key, so
 * every key one shard's table holds agrees on those bits. For an A64 address
 * below 2^31 (non-PIE code), and for an AArch32 key, whose top bits are mode
 * flags, the table's own hash would otherwise share most of that multiply.
 * Their index positions must still spread, or each probe walks a run of
 * other keys. */
void spread_within_a_shard()
{
	using Table = cache::StableRecordTable<Sized<24>, CountingKeyOf>;
	const auto shard = [](std::uint64_t key) {
		auto mixed = key * 0x9e37'79b9'7f4a'7c15ULL;
		mixed ^= mixed >> 29;
		return (mixed >> 7) & 7U;
	};
	const auto a64 = [](std::uint64_t index) {
		return 0x40'0000 + index * 4;
	};
	const auto t32 = [](std::uint64_t index) {
		return (std::uint64_t{3} << 62) |
			((0x1'0000 + index * 2) << 1);
	};
	const auto check = [&](const auto &key_of_index) {
		std::vector<std::uint64_t> keys;
		for (std::uint64_t index = 0; keys.size() < 50'000; ++index)
			if (const auto key = key_of_index(index); shard(key) == 0)
				keys.push_back(key);
		Table table;
		for (const auto key : keys)
			require(table.try_emplace(key, key).second);
		CountingKeyOf::reads = 0;
		for (const auto key : keys)
			require(table.find(key)->key == key);
		/* A 6-bit tag passes one entry in 64 that is not the key, so
		 * a run of r entries costs about 1 + r/64 reads. */
		require(CountingKeyOf::reads * 4 < keys.size() * 5,
			"a shard's keys cluster in the record index");
	};
	check(a64);
	check(t32);
}

} // namespace

int main()
{
	leased_heap();
	exercise<Sized<24>>(200'000);
	exercise<Sized<1024>>(5'000);
	exercise<Sized<8192>>(1'000);
	sweep<Sized<24>>(200'000);
	sweep<Sized<1024>>(5'000);
	sweep<Sized<8192>>(1'000);
	unlink_then_release<Sized<24>>(200'000);
	unlink_then_release<Sized<1024>>(5'000);
	unlink_then_release<Sized<8192>>(1'000);
	candidates();
	trim();
	several_tables();
	metered<Sized<24>>(20'000);
	metered<Sized<1024>>(2'000);
	spread_within_a_shard();

	/* Consecutive keys, as neighbouring guest addresses are. */
	cache::StableRecordTable<Sized<24>, KeyOf> table;
	for (std::uint64_t index = 0; index < 100'000; ++index)
		require(table.try_emplace(index, index).second);
	for (std::uint64_t index = 0; index < 100'000; ++index)
		require(table.find(index)->key == index);
	return 0;
}
