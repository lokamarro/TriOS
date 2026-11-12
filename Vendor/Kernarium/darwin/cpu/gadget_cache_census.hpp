// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* What both gadget engines' caches report in a UML_GADGET_CACHE_CENSUS build
 * (tests/perf/README.md, "Cache census"). Only the executors of such a build
 * include this; nothing here reaches a default build.
 *
 * A census is the cache's own account of what it holds and of how it came to
 * hold it. The holdings are walked when a snapshot is written. The history
 * cannot be walked afterwards, so the cache keeps it as it goes: how each
 * lookup ended, why each translation left, and, when the same guest code is
 * translated again, why the earlier copy was gone and how long ago.
 */

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dlfcn.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mutex>
#include <new>
#include <span>
#include <unordered_map>
#include <utility>

namespace uml::cpu::gadget_cache::census {

/* FNV-1a over the guest units a translation consumed. The engines seed it
 * differently where one ISA has several decodings of the same bytes. */
inline constexpr std::uint64_t digest_seed = 0xcbf29ce484222325ULL;

[[nodiscard]] inline constexpr std::uint64_t digest_add(
	std::uint64_t digest, std::uint64_t value) noexcept
{
	return (digest ^ value) * 0x100000001b3ULL;
}

[[nodiscard]] inline const char *path() noexcept
{
#ifdef UML_APP_STORE_RUNTIME
	return nullptr;
#else
	const char *value = std::getenv("DARWIN_UML_GADGET_CACHE_CENSUS");
	return value != nullptr && *value != '\0' ? value : nullptr;
#endif
}

/* Read once: a positive count of seconds, or zero. */
[[nodiscard]] inline std::uint64_t environment_count(const char *name) noexcept
{
#ifdef UML_APP_STORE_RUNTIME
	(void)name;
	const char *value = nullptr;
#else
	const char *value = std::getenv(name);
#endif
	if (value == nullptr)
		return 0;
	char *end = nullptr;
	const auto parsed = std::strtoull(value, &end, 10);
	return end != value && *end == '\0' ? parsed : 0;
}

/* Seconds between periodic snapshots; zero writes only the one at
 * teardown. */
[[nodiscard]] inline std::uint64_t interval_seconds() noexcept
{
	static const std::uint64_t seconds =
		environment_count("DARWIN_UML_GADGET_CACHE_CENSUS_SECONDS");
	return seconds;
}

/* One body in this many, chosen by guest PC, has its stream written out
 * word by word; zero writes none. */
[[nodiscard]] inline std::uint64_t word_sample() noexcept
{
	static const std::uint64_t one_in =
		environment_count("DARWIN_UML_GADGET_CACHE_CENSUS_WORDS");
	return one_in;
}

/* Whether the body at guest PC `key` is one of the one in `one_in` whose
 * stream is written word by word. A hash of the PC chooses, so every cache
 * samples the same code. */
[[nodiscard]] inline bool sampled(
	std::uint64_t key, std::uint64_t one_in) noexcept
{
	auto value = key * 0x9e3779b97f4a7c15ULL;
	value ^= value >> 29;
	return value % one_in == 0;
}

/* Whether snapshots carry only their E and G records and stores go unmatched
 * against other caches, which DARWIN_UML_GADGET_CACHE_CENSUS_TOTALS=1 asks
 * for. A browser guest's start-up on a loaded host could not afford the
 * per-store registry and the per-translation records: its own connection
 * timeouts fired and the page never loaded. Lookups, departures and
 * footprints are still counted in full. */
[[nodiscard]] inline bool totals_only() noexcept
{
	static const bool totals =
		environment_count("DARWIN_UML_GADGET_CACHE_CENSUS_TOTALS") != 0;
	return totals;
}

[[nodiscard]] inline std::chrono::steady_clock::time_point start() noexcept
{
	static const auto started = std::chrono::steady_clock::now();
	return started;
}

[[nodiscard]] inline double elapsed_seconds() noexcept
{
	return std::chrono::duration<double>(
		std::chrono::steady_clock::now() - start()).count();
}

/* A small number per cache, so the registry can say which caches have
 * translated a piece of code. */
[[nodiscard]] inline unsigned next_cache_id() noexcept
{
	static std::mutex mutex;
	static unsigned next = 0;
	const std::lock_guard guard{mutex};
	return next++;
}

/* The host's floating-point control and status, set aside while the census
 * writes. A snapshot can be written at a lookup, between two blocks of a run,
 * where the guest's own floating-point environment may be live in the host's
 * registers: the arm64 engine restores its native one only as the run ends.
 * The writer's arithmetic runs in the default environment instead, and the
 * guest's comes back as it was, cumulative flags included. */
class DefaultFloatingEnvironment final {
public:
	DefaultFloatingEnvironment() noexcept
	{
		__asm__ volatile("mrs %0, fpcr" : "=r"(fpcr_) : : "memory");
		__asm__ volatile("mrs %0, fpsr" : "=r"(fpsr_) : : "memory");
		__asm__ volatile("msr fpcr, xzr" : : : "memory");
	}
	~DefaultFloatingEnvironment()
	{
		__asm__ volatile("msr fpcr, %0" : : "r"(fpcr_) : "memory");
		__asm__ volatile("msr fpsr, %0" : : "r"(fpsr_) : "memory");
	}
	DefaultFloatingEnvironment(const DefaultFloatingEnvironment &) = delete;
	DefaultFloatingEnvironment &operator=(
		const DefaultFloatingEnvironment &) = delete;

private:
	std::uint64_t fpcr_{};
	std::uint64_t fpsr_{};
};

/* Append one cache's records. Every executor of a run appends to the same
 * file, so whole records are serialised here; they are formatted in memory
 * first, so a vCPU writing a large cache holds up the others only while its
 * text reaches the file. A snapshot is taken at a lookup or at an executor's
 * teardown, neither of which may fail for it, so one the census has no memory
 * to finish is left out whole rather than written in part. */
template <typename Write>
void append(Write &&write) noexcept
{
	const char *file = path();
	if (file == nullptr)
		return;
	char *text = nullptr;
	std::size_t size = 0;
	std::FILE *buffer = open_memstream(&text, &size);
	if (buffer == nullptr)
		return;
	const DefaultFloatingEnvironment environment;
	bool whole = true;
	try {
		write(buffer);
	} catch (const std::bad_alloc &) {
		whole = false;
	}
	std::fclose(buffer);
	if (whole) {
		static std::mutex mutex;
		const std::lock_guard guard{mutex};
		if (std::FILE *out = std::fopen(file, "ae"); out != nullptr) {
			std::fwrite(text, 1, size, out);
			std::fclose(out);
		}
	}
	std::free(text);
}

/* The fields every snapshot's executor record begins its timing with. */
inline void write_when(std::FILE *out, std::uint64_t snapshot) noexcept
{
	if (snapshot == 0)
		std::fprintf(out, " snapshot=final");
	else
		std::fprintf(out, " snapshot=%llu",
			static_cast<unsigned long long>(snapshot));
	std::fprintf(out, " seconds=%.3f wall=%lld", elapsed_seconds(),
		static_cast<long long>(std::time(nullptr)));
}

/* The executable's text, which every gadget entry lies in: a stream word
 * inside it names a gadget. */
struct TextRange final {
	std::uintptr_t begin{};
	std::uintptr_t end{};

	[[nodiscard]] bool contains(std::uintptr_t word) const noexcept
	{
		return word >= begin && word < end;
	}
};

[[nodiscard]] inline TextRange text_of(const void *inside) noexcept
{
	Dl_info info{};
	if (dladdr(inside, &info) == 0 || info.dli_fbase == nullptr)
		return {};
	unsigned long size = 0;
	const auto *data = getsegmentdata(
		static_cast<const mach_header_64 *>(info.dli_fbase), "__TEXT",
		&size);
	const auto begin = reinterpret_cast<std::uintptr_t>(data);
	return {begin, begin + size};
}

/* What each record a stream can point at names, by the record's address, for
 * one address space: a link record by the guest PC it links to (`l`), an
 * indirect site by the branch that owns it (`i`). */
using RecordNames =
	std::unordered_map<std::uintptr_t, std::pair<char, std::uint64_t>>;

/* One sampled body's stream, word by word: `g` a gadget by its offset in the
 * executable's text, `l` and `i` a record by `names`, and `o` any other word
 * as it is. */
inline void write_words(std::FILE *out, std::uint64_t key,
	std::span<const std::uintptr_t> words, TextRange text,
	const RecordNames &names)
{
	std::fprintf(out, "W 0x%llx", static_cast<unsigned long long>(key));
	for (const auto value : words) {
		if (text.contains(value)) {
			std::fprintf(out, " g%llx",
				static_cast<unsigned long long>(
					value - text.begin));
		} else if (const auto found = names.find(value);
			   found != names.end()) {
			std::fprintf(out, " %c%llx", found->second.first,
				static_cast<unsigned long long>(
					found->second.second));
		} else {
			std::fprintf(out, " o%llx",
				static_cast<unsigned long long>(value));
		}
	}
	std::fputc('\n', out);
}

/* Periodic snapshots are numbered from one by the interval they fall in, so
 * the records every cache writes for the same moment share a number. */
class Schedule final {
public:
	/* The snapshot due now, or zero. */
	[[nodiscard]] std::uint64_t due() noexcept
	{
		const auto interval = interval_seconds();
		if (interval == 0 || path() == nullptr)
			return 0;
		/* In whole seconds, as integers: a lookup asks, while the guest's
		 * floating-point environment may be live (append()). */
		const auto number = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::steady_clock::now() - start())
				.count()) / interval;
		if (number <= written_)
			return 0;
		written_ = number;
		return number;
	}

private:
	std::uint64_t written_{};
};

/* Write `cache`'s snapshot if one is due. */
template <typename Cache>
void snapshot_if_due(Schedule &schedule, const Cache &cache)
{
	if (const auto number = schedule.due(); number != 0)
		append([&cache, number](std::FILE *out) {
			cache.census(out, number);
		});
}

/* Whether some other cache translated the same guest code first: another
 * executor of the same address space, which a body shared between executors
 * would have saved, or only another address space, which sharing across a
 * fork family would. */
enum class Repeat : std::uint8_t { none, other_executor, other_space };

class Registry final {
public:
	/* Every store of every vCPU asks, so one lock serialized them: under a
	 * browser's start-up the census spent more of a vCPU's time waiting for
	 * it than translating, and the guest's own timeouts fired. A piece of
	 * code always lands in the same shard, so each shard answers for the
	 * code it holds on its own. */
	static constexpr std::size_t shards = 256;

	[[nodiscard]] static Repeat note(unsigned cache, std::uint64_t generation,
		std::uint64_t pc, std::uint64_t digest)
	{
		static std::array<Registry, shards> registries;
		const auto shard = Hash::mix(Hash::mix(pc) ^ digest) >>
			(64U - std::bit_width(shards - 1U));
		return registries[shard].record(cache, generation, pc, digest);
	}

private:
	struct SpaceKey final {
		std::uint64_t generation, pc, digest;
		bool operator==(const SpaceKey &) const noexcept = default;
	};
	struct CodeKey final {
		std::uint64_t pc, digest;
		bool operator==(const CodeKey &) const noexcept = default;
	};
	struct Hash final {
		[[nodiscard]] static std::size_t mix(std::uint64_t value) noexcept
		{
			value ^= value >> 33;
			value *= 0xff51afd7ed558ccdULL;
			value ^= value >> 33;
			return static_cast<std::size_t>(value);
		}
		[[nodiscard]] std::size_t operator()(const SpaceKey &key) const noexcept
		{
			return mix(key.generation * 0x9e3779b97f4a7c15ULL ^
				mix(key.pc) ^ key.digest);
		}
		[[nodiscard]] std::size_t operator()(const CodeKey &key) const noexcept
		{
			return mix(mix(key.pc) ^ key.digest);
		}
	};

	[[nodiscard]] Repeat record(unsigned cache, std::uint64_t generation,
		std::uint64_t pc, std::uint64_t digest)
	{
		const auto bit = std::uint64_t{1} << (cache % 64U);
		const std::lock_guard guard{mutex_};
		auto [space, first_in_space] = spaces_.try_emplace(
			SpaceKey{generation, pc, digest}, 0U);
		const bool elsewhere = (space->second & ~bit) != 0;
		space->second |= bit;
		auto [code, first_anywhere] = codes_.try_emplace(
			CodeKey{pc, digest}, generation);
		if (elsewhere)
			return Repeat::other_executor;
		if (first_in_space && !first_anywhere &&
		    code->second != generation)
			return Repeat::other_space;
		return Repeat::none;
	}

	std::mutex mutex_;
	/* Which caches translated this code in this address space. */
	std::unordered_map<SpaceKey, std::uint64_t, Hash> spaces_;
	/* The first address space this code was translated in. */
	std::unordered_map<CodeKey, std::uint64_t, Hash> codes_;
};

/* Why a translation left its cache. */
enum class Departure : std::uint8_t {
	/* Way replacement made room for another translation. */
	evicted,
	/* Its address space was retired from the cache. */
	retired,
	/* Validation found it stale, or it could not serve the request. */
	discarded,
};

/* How one cache's lookups ended and how its translations came and went.
 * Recording never fails: a cache records from operations that a default
 * build performs without allocating, some of them noexcept, and a census
 * build's cache must do there what a default build's does. A departure or
 * store the census has no memory to remember is counted as unrecorded
 * instead, so the counts stay exact and only the matching of stores to
 * earlier departures and to other caches is a lower bound. */
class History final {
public:
	static constexpr std::size_t distance_buckets = 48;

	/* Each lookup counts once, as its outcome. */
	void hit() noexcept { ++lookups_; ++hits_; }
	void revalidated() noexcept { ++lookups_; ++revalidated_; }
	void missed() noexcept { ++lookups_; ++misses_; }
	/* A miss that took the translation another executor of the address
	 * space holds instead of translating. */
	void adopted() noexcept { ++lookups_; ++adopted_; }
	[[nodiscard]] std::uint64_t lookups() const noexcept { return lookups_; }

	void depart(std::uint64_t generation, std::uint64_t pc,
		std::uint64_t digest, std::size_t bytes, Departure why) noexcept
	{
		try {
			departed_[Key{generation, pc}] = {digest, evictions_, why};
		} catch (const std::bad_alloc &) {
			++unrecorded_;
		}
		switch (why) {
		case Departure::evicted:
			++evictions_;
			evicted_bytes_ += bytes;
			break;
		case Departure::retired:
			++retirements_;
			break;
		case Departure::discarded:
			++discards_;
			break;
		}
	}

	/* A translation was stored for this key: say whether this cache, or
	 * any other, held the same code before. `restored` says the cache
	 * still held its own copy, which a lookup could not use. */
	void arrive(unsigned cache, std::uint64_t generation, std::uint64_t pc,
		std::uint64_t digest, bool restored) noexcept
	{
		++stores_;
		auto repeat = Repeat::none;
		if (!totals_only()) {
			try {
				repeat = Registry::note(cache, generation, pc, digest);
			} catch (const std::bad_alloc &) {
				++unrecorded_;
			}
		}
		switch (repeat) {
		case Repeat::none:
			break;
		case Repeat::other_executor:
			++other_executor_;
			break;
		case Repeat::other_space:
			++other_space_;
			break;
		}
		if (restored) {
			++restored_;
			departed_.erase(Key{generation, pc});
			return;
		}
		const auto found = departed_.find(Key{generation, pc});
		if (found == departed_.end()) {
			++first_;
			return;
		}
		const auto departed = found->second;
		departed_.erase(found);
		if (departed.digest != digest) {
			++changed_;
			return;
		}
		switch (departed.why) {
		case Departure::evicted: {
			++again_evicted_;
			const auto distance = evictions_ - departed.sequence;
			const auto bucket = std::min<std::size_t>(
				static_cast<std::size_t>(std::bit_width(distance)),
				distance_buckets - 1U);
			++distance_[bucket];
			break;
		}
		case Departure::retired:
			++again_retired_;
			break;
		case Departure::discarded:
			++again_discarded_;
			break;
		}
	}

	void write(std::FILE *out) const noexcept
	{
		std::fprintf(out,
			" lookups=%llu hits=%llu revalidated=%llu misses=%llu"
			" adopted=%llu"
			" stores=%llu first=%llu again_evicted=%llu"
			" again_retired=%llu again_discarded=%llu changed=%llu"
			" restored=%llu other_executor=%llu other_space=%llu"
			" evictions=%llu evicted_bytes=%llu retirements=%llu"
			" discards=%llu departed=%zu unrecorded=%llu distance=",
			value(lookups_), value(hits_), value(revalidated_),
			value(misses_), value(adopted_), value(stores_),
			value(first_),
			value(again_evicted_), value(again_retired_),
			value(again_discarded_), value(changed_),
			value(restored_), value(other_executor_),
			value(other_space_), value(evictions_),
			value(evicted_bytes_), value(retirements_),
			value(discards_), departed_.size(), value(unrecorded_));
		std::size_t used = distance_buckets;
		while (used > 1 && distance_[used - 1U] == 0)
			--used;
		for (std::size_t bucket = 0; bucket < used; ++bucket)
			std::fprintf(out, bucket == 0 ? "%llu" : ",%llu",
				value(distance_[bucket]));
	}

private:
	[[nodiscard]] static unsigned long long value(std::uint64_t count) noexcept
	{
		return static_cast<unsigned long long>(count);
	}

	struct Key final {
		std::uint64_t generation, pc;
		bool operator==(const Key &) const noexcept = default;
	};
	struct KeyHash final {
		[[nodiscard]] std::size_t operator()(const Key &key) const noexcept
		{
			auto value = key.pc ^ key.generation * 0x9e3779b97f4a7c15ULL;
			value ^= value >> 33;
			value *= 0xff51afd7ed558ccdULL;
			value ^= value >> 33;
			return static_cast<std::size_t>(value);
		}
	};
	struct Departed final {
		std::uint64_t digest;
		/* The eviction count when it left. */
		std::uint64_t sequence;
		Departure why;
	};

	std::uint64_t lookups_{}, hits_{}, revalidated_{}, misses_{},
		adopted_{};
	std::uint64_t stores_{}, first_{}, again_evicted_{}, again_retired_{},
		again_discarded_{}, changed_{}, restored_{};
	std::uint64_t other_executor_{}, other_space_{};
	std::uint64_t evictions_{}, evicted_bytes_{}, retirements_{},
		discards_{};
	std::uint64_t unrecorded_{};
	/* Evictions between a translation's eviction and the store of the
	 * same code under the same key, by bit width: bucket b holds
	 * distances below 2^b and at least 2^(b-1). An exclusive victim tier
	 * holding the last V evicted bodies would have served every return
	 * closer than V. */
	std::array<std::uint64_t, distance_buckets> distance_{};
	std::unordered_map<Key, Departed, KeyHash> departed_;
};

} // namespace uml::cpu::gadget_cache::census
