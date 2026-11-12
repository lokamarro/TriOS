// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/jit/jit_shared_translations.hpp"
#include "allocation_failure.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <thread>

namespace {

namespace arm64 = uml::cpu::arm64;
namespace aarch32 = uml::cpu::arm64::aarch32;

using uml::testing::require;

[[nodiscard]] std::shared_ptr<arm64::SharedTranslation>
make_translation(std::span<const std::uint32_t> guest_words,
	std::uint32_t compiler_identity, std::size_t host_word_count = 2)
{
	auto translation = std::make_shared<arm64::SharedTranslation>();
	auto saved =
		std::make_shared_for_overwrite<std::uint32_t[]>(guest_words.size());
	std::copy(guest_words.begin(), guest_words.end(), saved.get());
	translation->words.assign(host_word_count, compiler_identity);
	translation->guest_words = std::move(saved);
	translation->guest_word_count = guest_words.size();
	translation->guest_instructions = guest_words.size();
	translation->hot_words = 2;
	translation->internal_entry_word = 1;
	return translation;
}

template<typename Callback>
[[nodiscard]] arm64::SharedTranslationPublicationPreflight
allocation_free_preflight(Callback callback)
{
	arm64::SharedTranslationPublicationPreflight result;
	bool threw = false;
	uml::testing::begin_allocation_count();
	uml::testing::fail_next_allocation();
	try {
		result = callback();
	} catch (const std::bad_alloc &) {
		threw = true;
	}
	const auto allocations = uml::testing::end_allocation_count();
	const bool failure_still_pending =
		uml::testing::cancel_pending_allocation_failure();
	require(!threw);
	require(allocations == 0);
	require(failure_still_pending);
	return result;
}

void publication_preflight_avoids_definitely_full_candidates()
{
	const arm64::SharedTranslationCache::CodegenProfile profile{};
	const std::array first_words{0xd400'0001U};
	const std::array other_words{0xd420'0000U};
	constexpr arm64::SharedTranslationPublicationSizes sizes{
		.host_words = 2,
		.relocations = 0,
		.guest_words = 1,
	};

	arm64::SharedTranslationCache shard_full{/*shard_capacity=*/1};
	shard_full.add_executor();
	shard_full.add_executor();
	constexpr std::uint64_t guest_pc = 0x17a'8000;
	const auto canonical = shard_full.insert(
		profile, guest_pc, first_words,
		make_translation(first_words, 1));
	require(canonical != nullptr);
	const auto before_race = shard_full.statistics();
	const auto raced = allocation_free_preflight([&] {
		return shard_full.preflight_publication(
			profile, guest_pc, first_words, sizes,
			/*direct_link_refusals=*/2,
			/*indirect_link_refusals=*/3);
	});
	require(raced.canonical == canonical);
	require(!raced.may_construct);
	require(shard_full.statistics().rejected == before_race.rejected);
	require(shard_full.statistics().degraded_rejected ==
		before_race.degraded_rejected);

	const auto degraded = allocation_free_preflight([&] {
		return shard_full.preflight_publication(
			profile, guest_pc, other_words, sizes,
			/*direct_link_refusals=*/2,
			/*indirect_link_refusals=*/3);
	});
	require(degraded.canonical == nullptr);
	require(!degraded.may_construct);
	require(shard_full.statistics().rejected == before_race.rejected);
	require(shard_full.statistics().degraded_rejected ==
		before_race.degraded_rejected + 1U);

	const auto refused = allocation_free_preflight([&] {
		return shard_full.preflight_publication(
			profile, guest_pc, other_words, sizes,
			/*direct_link_refusals=*/0,
			/*indirect_link_refusals=*/0);
	});
	require(refused.canonical == nullptr);
	require(!refused.may_construct);
	require(shard_full.statistics().rejected == before_race.rejected + 1U);
	shard_full.remove_executor();
	shard_full.remove_executor();

	/* Mirror the accepted diagnostic's final 608-byte residual. Even before
	 * payload, a valid entry's conservative fixed charge is larger. Repeated
	 * compiles must remain on the allocation-free rejection path.
	 */
	arm64::SharedTranslationCache bytes_full{
		/*shard_capacity=*/8,
		/*maximum_accounted_bytes=*/
			arm64::minimum_shared_translation_cache_bytes + 608U};
	bytes_full.add_executor();
	bytes_full.add_executor();
	for (unsigned attempt = 0; attempt < 2; ++attempt) {
		const auto plan = allocation_free_preflight([&] {
			return bytes_full.preflight_publication(
				profile, guest_pc + attempt * 4U, first_words, sizes,
				/*direct_link_refusals=*/0,
				/*indirect_link_refusals=*/0);
		});
		require(plan.canonical == nullptr);
		require(!plan.may_construct);
	}
	require(bytes_full.statistics().rejected == 2);
	bytes_full.remove_executor();
	bytes_full.remove_executor();

	arm64::SharedTranslationCache overflow;
	overflow.add_executor();
	overflow.add_executor();
	const auto overflowed = allocation_free_preflight([&] {
		return overflow.preflight_publication(
			profile, guest_pc, first_words,
			{.host_words = std::numeric_limits<std::size_t>::max(),
			 .relocations = 0,
			 .guest_words = first_words.size()},
			/*direct_link_refusals=*/0,
			/*indirect_link_refusals=*/0);
	});
	require(overflowed.canonical == nullptr);
	require(!overflowed.may_construct);
	require(overflow.statistics().rejected == 1);
	overflow.remove_executor();
	overflow.remove_executor();
}

void retired_peer_preserves_exact_owners_without_new_admission()
{
	arm64::SharedTranslationCache cache;
	cache.add_executor();
	cache.add_executor();
	const arm64::SharedTranslationCache::CodegenProfile profile{};
	constexpr std::uint64_t guest_pc = 0x17a'c000;
	const std::array canonical_words{0xd400'0001U};
	const std::array other_words{0xd420'0000U};
	constexpr arm64::SharedTranslationPublicationSizes sizes{
		.host_words = 2,
		.relocations = 0,
		.guest_words = 1,
	};

	const auto canonical = cache.insert(
		profile, guest_pc, canonical_words,
		make_translation(canonical_words, 1));
	require(canonical != nullptr);
	cache.remove_executor();
	require(!cache.accepts_publications());
	const auto before = cache.statistics();

	/* A compiler that overlapped the publishing peer may arrive here only
	 * after that peer retires. Its exact owner wins before both the live gate
	 * and its own degraded-link refusal counts.
	 */
	const auto preflight_owner = allocation_free_preflight([&] {
		return cache.preflight_publication(
			profile, guest_pc, canonical_words, sizes,
			/*direct_link_refusals=*/2,
			/*indirect_link_refusals=*/3);
	});
	require(preflight_owner.canonical == canonical);
	require(!preflight_owner.may_construct);

	const auto gated_preflight = allocation_free_preflight([&] {
		return cache.preflight_publication(
			profile, guest_pc, other_words, sizes,
			/*direct_link_refusals=*/2,
			/*indirect_link_refusals=*/3);
	});
	require(gated_preflight.canonical == nullptr);
	require(!gated_preflight.may_construct);

	auto exact_candidate = make_translation(canonical_words, 2);
	exact_candidate->direct_link_refusals = 1;
	require(cache.insert(
		profile, guest_pc, canonical_words, exact_candidate) == canonical);
	require(cache.insert(
		profile, guest_pc, other_words,
		make_translation(other_words, 3)) == nullptr);

	const auto after = cache.statistics();
	require(after.entries == before.entries);
	require(after.insertions == before.insertions);
	require(after.rejected == before.rejected);
	require(after.degraded_rejected == before.degraded_rejected);
	require(after.malformed_rejected == before.malformed_rejected);
	require(after.insertion_failures == before.insertion_failures);
	require(after.failed_insertion_retained_accounted_bytes ==
		before.failed_insertion_retained_accounted_bytes);
	require(after.accounted_bytes == before.accounted_bytes);
	cache.remove_executor();
}

void failed_new_entry_growth_reports_retained_accounting()
{
	const arm64::SharedTranslationCache::CodegenProfile profile{};
	constexpr std::uint64_t guest_pc = 0x17a'e000;
	const std::array guest_words{0xd400'0001U};

	/* Measure an otherwise identical successful insertion. Candidate ownership
	 * is already allocated, so the final counted allocation is Entry-vector
	 * growth after try_emplace() has completed every map allocation.
	 */
	arm64::SharedTranslationCache calibration;
	calibration.add_executor();
	calibration.add_executor();
	auto calibration_candidate = make_translation(guest_words, 1);
	uml::testing::begin_allocation_count();
	const auto admitted = calibration.insert(
		profile, guest_pc, guest_words, calibration_candidate);
	const auto successful_insert_allocations =
		uml::testing::end_allocation_count();
	require(admitted == calibration_candidate);
	require(successful_insert_allocations >= 2);
	calibration.remove_executor();
	calibration.remove_executor();

	arm64::SharedTranslationCache cache;
	cache.add_executor();
	cache.add_executor();
	auto candidate = make_translation(guest_words, 2);
	const auto before = cache.statistics();
	uml::testing::fail_after_allocations(
		successful_insert_allocations - 1U);
	bool threw = false;
	try {
		(void)cache.insert(profile, guest_pc, guest_words, candidate);
	} catch (const std::bad_alloc &) {
		threw = true;
	}
	require(threw);
	require(uml::testing::last_allocation_failure().observed);
	require(!uml::testing::cancel_pending_allocation_failure());

	const auto after = cache.statistics();
	require(after.entries == before.entries);
	require(after.insertions == before.insertions);
	require(after.rejected == before.rejected);
	require(after.payload_bytes == before.payload_bytes);
	require(after.insertion_failures == before.insertion_failures + 1U);
	require(after.failed_insertion_retained_accounted_bytes >
		before.failed_insertion_retained_accounted_bytes);
	const auto retained =
		after.failed_insertion_retained_accounted_bytes -
		before.failed_insertion_retained_accounted_bytes;
	require(after.accounted_bytes == before.accounted_bytes + retained);
	require(after.accounted_bytes <= after.maximum_accounted_bytes);
	cache.remove_executor();
	cache.remove_executor();
}

void fallback_budget_limits_live_executor_leases()
{
	constexpr std::size_t fallback_total = 512U * 1024U * 1024U;
	{
		arm64::JitCacheExecutorLease first;
		arm64::JitCacheExecutorLease excess;
		require(first.admitted());
		require(!excess.admitted());
		require(first.geometry().valid());
		require(!excess.geometry().valid());
		require(first.geometry().total_bytes() +
			arm64::jit_shared_translation_cache_budget() <=
			fallback_total);
	}
	arm64::JitCacheExecutorLease recycled;
	require(recycled.admitted());
	require(arm64::set_jit_cache_budget(fallback_total, 1));
	require(!arm64::set_jit_cache_budget(fallback_total, 2));
}

void shared_shards_are_backed_only_by_publication()
{
	arm64::SharedTranslationCache cache;
	const arm64::SharedTranslationCache::CodegenProfile profile{};
	constexpr std::uint64_t guest_pc = 0x179'0000;
	const std::array guest_words{0xd400'0001U};
	require(cache.allocated_shards() == 0);
	require(cache.find(profile, guest_pc, guest_words) == nullptr);
	cache.add_executor();
	cache.add_executor();
	const auto preflight = cache.preflight_publication(
		profile, guest_pc, guest_words,
		{.host_words = 2, .relocations = 0, .guest_words = 1}, 0, 0);
	require(preflight.may_construct && cache.allocated_shards() == 0);
	require(cache.insert(profile, guest_pc, guest_words,
		make_translation(guest_words, 1)) != nullptr);
	require(cache.allocated_shards() == 1);
	cache.remove_executor();
	cache.remove_executor();
}

void concurrent_publication_keeps_one_canonical_owner()
{
	constexpr std::size_t publisher_count = 6;
	constexpr std::size_t reader_count = 6;
	constexpr std::size_t rounds = 256;
	constexpr std::uint64_t guest_pc = 0x17a'0000;
	const std::array guest_words{0xd280'0020U, 0xd400'0001U};
	const std::array other_guest_words{0xd280'0040U, 0xd400'0001U};
	const arm64::SharedTranslationCache::CodegenProfile profile{};

	std::shared_ptr<const arm64::SharedTranslation> survivor;
	std::weak_ptr<const arm64::SharedTranslation> canonical_weak;
	const arm64::SharedTranslation *canonical_address = nullptr;
	{
		/* A one-entry shard proves that duplicate publication wins before the
		 * capacity check, while a different identity must fall back privately.
		 */
		arm64::SharedTranslationCache cache{1};
		cache.add_executor();
		cache.add_executor();
		require(cache.accepts_publications());

		std::array<std::weak_ptr<const arm64::SharedTranslation>,
			publisher_count>
			compiler_owners;
		std::array<std::shared_ptr<const arm64::SharedTranslation>,
			publisher_count>
			published;
		std::array<std::shared_ptr<const arm64::SharedTranslation>,
			reader_count>
			read;
		std::atomic<bool> publication_finished{false};
		std::atomic<bool> valid{true};
		std::barrier start{publisher_count + reader_count};
		std::array<std::thread, publisher_count> publishers;
		std::array<std::thread, reader_count> readers;

		for (std::size_t index = 0; index < publishers.size(); ++index) {
			publishers[index] = std::thread{[&, index] {
				auto candidate = make_translation(
					guest_words, static_cast<std::uint32_t>(index + 1U));
				compiler_owners[index] = candidate;
				start.arrive_and_wait();

				std::shared_ptr<const arm64::SharedTranslation> canonical;
				for (std::size_t round = 0; round < rounds; ++round) {
					auto admitted = cache.insert(
						profile, guest_pc, guest_words, candidate);
					if (admitted == nullptr) {
						valid.store(false, std::memory_order_relaxed);
						publication_finished.store(
							true, std::memory_order_release);
						break;
					}
					if (canonical != nullptr && admitted != canonical)
						valid.store(false, std::memory_order_relaxed);
					canonical = std::move(admitted);
					publication_finished.store(
						true, std::memory_order_release);

					const auto found =
						cache.find(profile, guest_pc, guest_words);
					if (found != canonical)
						valid.store(false, std::memory_order_relaxed);
				}
				/* The compiler's candidate owner ends here. Only the candidate
				 * selected by the cache may remain alive after all threads join.
				 */
				candidate.reset();
				published[index] = std::move(canonical);
			}};
		}

		for (std::size_t index = 0; index < readers.size(); ++index) {
			readers[index] = std::thread{[&, index] {
				start.arrive_and_wait();
				std::shared_ptr<const arm64::SharedTranslation> canonical;
				for (std::size_t round = 0; round < rounds; ++round) {
					auto found = cache.find(profile, guest_pc, guest_words);
					if (found != nullptr) {
						if (canonical != nullptr && found != canonical)
							valid.store(false, std::memory_order_relaxed);
						canonical = std::move(found);
					} else {
						std::this_thread::yield();
					}
				}
				while (!publication_finished.load(std::memory_order_acquire))
					std::this_thread::yield();
				const auto final = cache.find(profile, guest_pc, guest_words);
				if (final == nullptr ||
					(canonical != nullptr && final != canonical))
					valid.store(false, std::memory_order_relaxed);
				read[index] = final;
			}};
		}

		for (auto &publisher : publishers)
			publisher.join();
		for (auto &reader : readers)
			reader.join();
		require(valid.load(std::memory_order_relaxed));

		auto canonical = cache.find(profile, guest_pc, guest_words);
		require(canonical != nullptr);
		canonical_address = canonical.get();
		for (const auto &result : published)
			require(result == canonical);
		for (const auto &result : read)
			require(result == canonical);

		std::size_t retained_compiler_owners = 0;
		for (const auto &owner : compiler_owners) {
			const auto locked = owner.lock();
			if (locked == nullptr)
				continue;
			require(locked == canonical);
			canonical_weak = owner;
			++retained_compiler_owners;
		}
		require(retained_compiler_owners == 1);

		auto statistics = cache.statistics();
		require(statistics.entries == 1);
		require(statistics.insertions == 1);
		require(statistics.rejected == 0);
		require(statistics.matches >=
			publisher_count * rounds + reader_count);

		auto refused_candidate = make_translation(other_guest_words, 0xffffU);
		const std::weak_ptr<const arm64::SharedTranslation> refused_weak =
			refused_candidate;
		const auto refused = cache.insert(
			profile, guest_pc, other_guest_words, refused_candidate);
		require(refused == nullptr);
		refused_candidate.reset();
		require(refused_weak.expired());
		require(cache.find(profile, guest_pc, other_guest_words) == nullptr);
		statistics = cache.statistics();
		require(statistics.entries == 1);
		require(statistics.insertions == 1);
		require(statistics.rejected == 1);

		/* Drop every caller-owned result. The cache remains the canonical
		 * owner, and a lookup owner remains valid after the cache is destroyed.
		 */
		canonical.reset();
		for (auto &result : published)
			result.reset();
		for (auto &result : read)
			result.reset();
		require(!canonical_weak.expired());
		survivor = cache.find(profile, guest_pc, guest_words);
		require(survivor != nullptr);
		require(survivor.get() == canonical_address);

		cache.remove_executor();
		cache.remove_executor();
		require(!cache.accepts_publications());
	}

	require(!canonical_weak.expired());
	require(survivor != nullptr);
	require(survivor->guest_word_count == guest_words.size());
	require(std::equal(
		survivor->guest_words.get(),
		survivor->guest_words.get() + survivor->guest_word_count,
		guest_words.begin()));
	survivor.reset();
	require(canonical_weak.expired());
}

void lookup_identity_stops_at_the_compiled_prefix()
{
	arm64::SharedTranslationCache cache;
	cache.add_executor();
	cache.add_executor();
	const arm64::SharedTranslationCache::CodegenProfile profile{};
	constexpr std::uint64_t guest_pc = 0x17b'0000;
	const std::array compiled_words{0xd280'0020U};
	const std::array first_fetch{compiled_words[0], 0xd400'0001U};
	const std::array changed_tail{compiled_words[0], 0xd420'0000U};

	const auto admitted = cache.insert(
		profile, guest_pc, compiled_words,
		make_translation(compiled_words, 1));
	require(admitted != nullptr);
	require(cache.find(profile, guest_pc, first_fetch) == admitted);
	require(cache.find(profile, guest_pc, changed_tail) == admitted);
	const std::array changed_prefix{0xd280'0040U, first_fetch[1]};
	require(cache.find(profile, guest_pc, changed_prefix) == nullptr);
	cache.remove_executor();
	cache.remove_executor();
}

/* A translation is stored under the ISA of its key: A32 words count as A64
 * words do, and T32 halfwords are packed into the 32-bit elements of
 * guest_words, so the identity is the halfword bytes and any padding in the
 * last element is no part of it. The isa a translation carries must be its
 * key's, and no key of one ISA finds another's translation. */
void aarch32_translations_are_keyed_and_sized_by_isa()
{
	arm64::SharedTranslationCache cache;
	cache.add_executor();
	cache.add_executor();
	const arm64::SharedTranslationCache::CodegenProfile profile{};
	constexpr std::uint32_t address = 0x17c'0000;
	const auto a32 = aarch32::a32_key(address);
	const auto t32 = aarch32::t32_key(address);
	const std::array a32_words{0xe3a0'0001U, 0xef00'0000U};
	/* Three halfwords: a 32-bit Thumb instruction and a 16-bit one. */
	const std::array t32_words{0xe92d'4ff0U, 0x0000'bd00U};

	const auto aarch32_translation = [](auto words, std::uint8_t isa,
		std::size_t units, std::size_t instructions) {
		auto translation = make_translation(words, 1);
		translation->isa = isa;
		translation->guest_word_count = units;
		translation->guest_instructions = instructions;
		return translation;
	};

	/* The ISA must be the key's. */
	for (const auto isa : {std::uint8_t{0}, std::uint8_t{2}, std::uint8_t{3}})
		require(cache.insert(profile, a32, a32_words,
			aarch32_translation(std::span{a32_words}, isa, 2, 2)) ==
			nullptr);
	for (const auto isa : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{3}})
		require(cache.insert(profile, t32, t32_words,
			aarch32_translation(std::span{t32_words}, isa, 3, 2)) ==
			nullptr);
	require(cache.insert(profile, address, a32_words,
		aarch32_translation(std::span{a32_words}, 1, 2, 2)) == nullptr,
		"an A32 translation was stored under an A64 key");
	require(cache.statistics().malformed_rejected == 7);
	require(cache.statistics().entries == 0);

	/* The unit count: one word per A32 instruction; one or two halfwords
	 * per T32 instruction; the elements that hold them are what the lookup
	 * span carries. */
	require(cache.insert(profile, a32, a32_words,
		aarch32_translation(std::span{a32_words}, 1, 3, 2)) == nullptr);
	for (const std::size_t units : {std::size_t{1}, std::size_t{5}})
		require(cache.insert(profile, t32, t32_words,
			aarch32_translation(std::span{t32_words}, 2, units, 2)) ==
			nullptr, "a T32 unit count outside its instructions' range");
	const std::array three_elements{t32_words[0], t32_words[1], 0U};
	require(cache.insert(profile, t32, three_elements,
		aarch32_translation(std::span{three_elements}, 2, 3, 2)) == nullptr,
		"a T32 span with more elements than its units need");
	require(cache.statistics().entries == 0);

	const auto admitted_a32 = cache.insert(profile, a32, a32_words,
		aarch32_translation(std::span{a32_words}, 1, 2, 2));
	const auto admitted_t32 = cache.insert(profile, t32, t32_words,
		aarch32_translation(std::span{t32_words}, 2, 3, 2));
	require(admitted_a32 != nullptr && admitted_t32 != nullptr);
	require(admitted_a32 != admitted_t32);
	require(cache.statistics().entries == 2);

	/* The same address under another ISA's key is a different entry. */
	require(cache.find(profile, a32, a32_words) == admitted_a32);
	require(cache.find(profile, t32, t32_words) == admitted_t32);
	require(cache.find(profile, address, a32_words) == nullptr);
	require(cache.find(profile, aarch32::t32_key(address + 2U),
		t32_words) == nullptr);

	/* The identity of the Thumb translation is its three halfwords: a
	 * different padding halfword matches; a different third halfword or a
	 * longer fetch with the same prefix of elements does not change that. */
	const std::array other_padding{t32_words[0], 0x1234'bd00U};
	require(cache.find(profile, t32, other_padding) == admitted_t32,
		"padding after the identity is part of it");
	const std::array third_changed{t32_words[0], 0x0000'bd01U};
	require(cache.find(profile, t32, third_changed) == nullptr);
	const std::array second_changed{t32_words[0] ^ 0x0001'0000U,
		t32_words[1]};
	require(cache.find(profile, t32, second_changed) == nullptr);
	const std::array longer{t32_words[0], t32_words[1], 0xdead'beefU};
	require(cache.find(profile, t32, longer) == admitted_t32);

	/* An exact re-insert returns the canonical translation. */
	require(cache.insert(profile, t32, t32_words,
		aarch32_translation(std::span{t32_words}, 2, 3, 2)) == admitted_t32);
	cache.remove_executor();
	cache.remove_executor();
}

void codegen_profile_partitions_the_cache_identity()
{
	arm64::SharedTranslationCache cache;
	cache.add_executor();
	cache.add_executor();
	constexpr std::uint64_t guest_pc = 0x17b'8000;
	const std::array guest_words{0xd400'0001U};

	const arm64::SharedTranslationCache::CodegenProfile compiled{};
	auto same = compiled;
	require(same == compiled);

	auto other = compiled;
	other.capabilities.cssc = !compiled.capabilities.cssc;
	require(!(other == compiled));

	const auto compiled_entry = cache.insert(
		compiled, guest_pc, guest_words,
		make_translation(guest_words, 1));
	const auto other_entry = cache.insert(
		other, guest_pc, guest_words,
		make_translation(guest_words, 2));
	require(compiled_entry != nullptr && other_entry != nullptr);
	require(compiled_entry != other_entry);
	require(cache.find(compiled, guest_pc, guest_words) == compiled_entry);
	require(cache.find(other, guest_pc, guest_words) == other_entry);
	require(cache.statistics().entries == 2);
	cache.remove_executor();
	cache.remove_executor();
}

void capacity_degraded_translation_leaves_the_key_publishable()
{
	arm64::SharedTranslationCache cache;
	cache.add_executor();
	cache.add_executor();
	const arm64::SharedTranslationCache::CodegenProfile profile{};
	constexpr std::uint64_t guest_pc = 0x17c'0000;
	const std::array guest_words{0x5400'0040U}; // b.eq pc + 8

	auto degraded = make_translation(guest_words, 1);
	degraded->direct_link_refusals = 1;
	const std::weak_ptr<const arm64::SharedTranslation> degraded_weak =
		degraded;
	require(cache.insert(profile, guest_pc, guest_words, degraded) == nullptr);
	require(cache.find(profile, guest_pc, guest_words) == nullptr);
	auto statistics = cache.statistics();
	require(statistics.entries == 0);
	require(statistics.insertions == 0);
	require(statistics.rejected == 0);
	require(statistics.degraded_rejected == 1);
	degraded.reset();
	require(degraded_weak.expired());

	auto indirect_degraded = make_translation(guest_words, 2);
	indirect_degraded->indirect_link_refusals = 1;
	require(cache.insert(profile, guest_pc, guest_words,
		indirect_degraded) == nullptr);
	require(cache.find(profile, guest_pc, guest_words) == nullptr);
	statistics = cache.statistics();
	require(statistics.entries == 0);
	require(statistics.insertions == 0);
	require(statistics.degraded_rejected == 2);

	const auto complete = make_translation(guest_words, 3);
	const auto canonical =
		cache.insert(profile, guest_pc, guest_words, complete);
	require(canonical == complete);
	require(cache.find(profile, guest_pc, guest_words) == canonical);
	statistics = cache.statistics();
	require(statistics.entries == 1);
	require(statistics.insertions == 1);
	require(statistics.rejected == 0);
	require(statistics.degraded_rejected == 2);

	/* Malformed direct-API input is rejected before the canonical lookup. It
	 * must not be disguised as a successful duplicate publication.
	 */
	auto late_malformed = make_translation(guest_words, 4);
	late_malformed->words.clear();
	require(cache.insert(profile, guest_pc, guest_words, late_malformed) ==
		nullptr);
	statistics = cache.statistics();
	require(statistics.malformed_rejected == 1);
	require(statistics.entries == 1);

	/* A degraded racing candidate may reuse an already canonical entry, but
	 * must never replace it or create a second owner.
	 */
	auto late_degraded = make_translation(guest_words, 5);
	late_degraded->direct_link_refusals = 2;
	late_degraded->indirect_link_refusals = 1;
	require(cache.insert(profile, guest_pc, guest_words, late_degraded) ==
		canonical);
	statistics = cache.statistics();
	require(statistics.entries == 1);
	require(statistics.insertions == 1);
	require(statistics.degraded_rejected == 2);
	cache.remove_executor();
	cache.remove_executor();
}

void early_degraded_refusal_is_one_counter_event()
{
	arm64::SharedTranslationCache cache;
	require(!cache.refuse_degraded_publication(0, 0));
	require(cache.refuse_degraded_publication(2, 3));
	const auto statistics = cache.statistics();
	require(statistics.entries == 0);
	require(statistics.insertions == 0);
	require(statistics.degraded_rejected == 1);
}

void malformed_translation_metadata_is_rejected_before_admission()
{
	arm64::SharedTranslationCache cache;
	cache.add_executor();
	cache.add_executor();
	const arm64::SharedTranslationCache::CodegenProfile profile{};
	constexpr std::uint64_t guest_pc = 0x17e'0000;
	const std::array guest_words{0x5400'0040U}; // b.eq pc + 8
	std::uint64_t rejected = 0;
	auto conditional_shape = make_translation(guest_words, 1, 8);
	std::fill(conditional_shape->words.begin() + 4,
		conditional_shape->words.end(), 0);
	conditional_shape->relocations = {
		{.word = 4,
		 .key = guest_pc + 4,
		 .kind = arm64::JitRelocationKind::direct_link},
		{.word = 6,
		 .key = guest_pc + 8,
		 .kind = arm64::JitRelocationKind::direct_link},
	};
	require(conditional_shape->relocations.size() ==
		arm64::maximum_shared_translation_relocations);
	require(arm64::valid_shared_translation(
		*conditional_shape, guest_words));

	const auto reject = [&](auto mutate) {
		auto candidate = make_translation(guest_words, 1, 8);
		mutate(*candidate);
		require(cache.insert(profile, guest_pc, guest_words,
			std::move(candidate)) == nullptr);
		++rejected;
		const auto statistics = cache.statistics();
		require(statistics.malformed_rejected == rejected);
		require(statistics.entries == 0);
		require(statistics.insertions == 0);
	};

	require(cache.insert(profile, guest_pc, guest_words, nullptr) == nullptr);
	++rejected;
	require(cache.statistics().malformed_rejected == rejected);
	reject([](auto &translation) { translation.guest_words.reset(); });
	reject([](auto &translation) { translation.guest_word_count = 0; });
	reject([](auto &translation) { translation.guest_instructions = 2; });
	reject([](auto &translation) {
		translation.guest_word_count = arm64::maximum_block_instructions + 1U;
		translation.guest_instructions = translation.guest_word_count;
	});
	reject([](auto &translation) { translation.words.clear(); });
	reject([](auto &translation) { translation.hot_words = 0; });
	reject([](auto &translation) { translation.hot_words = 9; });
	reject([](auto &translation) { translation.internal_entry_word = 0; });
	reject([](auto &translation) { translation.internal_entry_word = 2; });
	reject([](auto &translation) { translation.native_fp = true; });
	reject([](auto &translation) {
		translation.direct_memory_write_mask = 1;
	});
	reject([](auto &translation) { translation.memory_write_mask = 2; });
	reject([](auto &translation) {
		translation.relocations.assign(
			arm64::maximum_shared_translation_relocations + 1U,
			arm64::JitRelocation{
				.word = 4,
				.key = 0,
				.kind = arm64::JitRelocationKind::direct_link,
			});
	});
	reject([](auto &translation) {
		translation.relocations.push_back({
			.word = 4,
			.key = 0,
			.kind = static_cast<arm64::JitRelocationKind>(0xff),
		});
	});
	reject([](auto &translation) {
		translation.words.resize(6);
		translation.hot_words = 2;
		translation.relocations.push_back({
			.word = 4,
			.key = 0,
			.kind = arm64::JitRelocationKind::direct_link,
		});
	});
	reject([](auto &translation) {
		translation.relocations.push_back({
			.word = 8,
			.key = 0,
			.kind = arm64::JitRelocationKind::direct_link,
		});
	});
	reject([](auto &translation) {
		translation.relocations.push_back({
			.word = 5,
			.key = 0,
			.kind = arm64::JitRelocationKind::direct_link,
		});
	});
	reject([](auto &translation) {
		translation.relocations.push_back({
			.word = 0,
			.key = 0,
			.kind = arm64::JitRelocationKind::direct_link,
		});
	});
	reject([](auto &translation) {
		translation.relocations.push_back({
			.word = 4,
			.key = 0,
			.kind = arm64::JitRelocationKind::direct_link,
		});
		translation.relocations.push_back({
			.word = 4,
			.key = 0,
			.kind = arm64::JitRelocationKind::indirect_link,
		});
	});

	const auto statistics = cache.statistics();
	require(statistics.rejected == 0);
	require(statistics.degraded_rejected == 0);
	require(statistics.accounted_bytes ==
		arm64::minimum_shared_translation_cache_bytes);
	require(statistics.accounted_bytes <=
		statistics.maximum_accounted_bytes);
	cache.remove_executor();
	cache.remove_executor();
}

void cumulative_byte_budget_spans_all_shards()
{
	constexpr std::size_t maximum_bytes = 1U * 1024U * 1024U;
	constexpr std::size_t host_word_count =
		(600U * 1024U) / sizeof(std::uint32_t);
	arm64::SharedTranslationCache cache{/*shard_capacity=*/8,
		/*maximum_accounted_bytes=*/maximum_bytes};
	cache.add_executor();
	cache.add_executor();
	const arm64::SharedTranslationCache::CodegenProfile profile{};
	const std::array guest_words{0xd400'0001U};
	const auto shard = [](std::uint64_t pc) {
		auto hash = pc * 0x9e3779b97f4a7c15ULL;
		hash ^= hash >> 29U;
		return hash & 63U;
	};
	constexpr std::uint64_t first_pc = 0x17d'0000;
	auto second_pc = first_pc + 4U;
	while (shard(second_pc) == shard(first_pc))
		second_pc += 4U;

	const auto first = cache.insert(
		profile, first_pc, guest_words,
		make_translation(guest_words, 1, host_word_count));
	require(first != nullptr);
	require(cache.insert(
		profile, second_pc, guest_words,
		make_translation(guest_words, 2, host_word_count)) == nullptr);
	const auto statistics = cache.statistics();
	require(statistics.entries == 1);
	require(statistics.insertions == 1);
	require(statistics.rejected == 1);
	require(statistics.payload_bytes >= 600U * 1024U);
	require(statistics.accounted_bytes > statistics.payload_bytes);
	require(statistics.accounted_bytes <=
		statistics.maximum_accounted_bytes);
	require(statistics.maximum_accounted_bytes == maximum_bytes);
	cache.remove_executor();
	cache.remove_executor();
}

void full_cache_replaces_unused_translations_without_spending_index_twice()
{
	constexpr std::size_t maximum_bytes = 128U * 1024U;
	constexpr std::size_t host_words = 8192;
	constexpr std::uint64_t guest_pc = 0x17e'0000;
	const arm64::SharedTranslationCache::CodegenProfile profile{};
	arm64::SharedTranslationCache cache{8, maximum_bytes};
	cache.add_executor();
	cache.add_executor();
	std::size_t high_water = 0;
	for (unsigned iteration = 0; iteration < 128; ++iteration) {
		const std::array words{0xd280'0000U | (iteration << 5U)};
		const auto plan = allocation_free_preflight([&] {
			return cache.preflight_publication(profile, guest_pc, words,
				{.host_words = host_words, .relocations = 0,
				 .guest_words = words.size()}, 0, 0);
		});
		require(plan.may_construct);
		const auto admitted = cache.insert(profile, guest_pc, words,
			make_translation(words, iteration, host_words));
		require(admitted != nullptr);
		require(cache.find(profile, guest_pc, words) == admitted);
		const auto statistics = cache.statistics();
		require(statistics.accounted_bytes <= maximum_bytes);
		high_water = std::max<std::size_t>(
			high_water, statistics.accounted_bytes);
		if (iteration != 0)
			require(statistics.evictions == iteration);
	}
	require(cache.statistics().entries == 1);
	require(cache.statistics().accounted_bytes == high_water);
	cache.remove_executor();
	cache.remove_executor();
}

void private_validation_words_and_materialization_owners_pin_entries()
{
	constexpr std::uint64_t guest_pc = 0x17e'4000;
	const arm64::SharedTranslationCache::CodegenProfile profile{};
	const std::array first_words{0xd400'0001U};
	const std::array second_words{0xd420'0000U};
	arm64::SharedTranslationCache cache{1};
	cache.add_executor();
	cache.add_executor();
	auto owner = cache.insert(profile, guest_pc, first_words,
		make_translation(first_words, 1));
	require(owner != nullptr);
	auto private_words = owner->guest_words;
	const auto publish = [&] {
		return cache.insert(profile, guest_pc, second_words,
			make_translation(second_words, 2));
	};
	require(publish() == nullptr);
	owner.reset();
	require(publish() == nullptr);
	require(private_words[0] == first_words[0]);
	private_words.reset();
	const auto replacement = publish();
	require(replacement != nullptr);
	require(cache.find(profile, guest_pc, first_words) == nullptr);
	require(cache.find(profile, guest_pc, second_words) == replacement);
	require(cache.statistics().evictions == 1);
	cache.remove_executor();
	cache.remove_executor();
}

} // namespace

int main()
{
	fallback_budget_limits_live_executor_leases();
	shared_shards_are_backed_only_by_publication();
	publication_preflight_avoids_definitely_full_candidates();
	retired_peer_preserves_exact_owners_without_new_admission();
	failed_new_entry_growth_reports_retained_accounting();
	malformed_translation_metadata_is_rejected_before_admission();
	early_degraded_refusal_is_one_counter_event();
	cumulative_byte_budget_spans_all_shards();
	full_cache_replaces_unused_translations_without_spending_index_twice();
	private_validation_words_and_materialization_owners_pin_entries();
	capacity_degraded_translation_leaves_the_key_publishable();
	lookup_identity_stops_at_the_compiled_prefix();
	aarch32_translations_are_keyed_and_sized_by_isa();
	codegen_profile_partitions_the_cache_identity();
	concurrent_publication_keeps_one_canonical_owner();
	return 0;
}
