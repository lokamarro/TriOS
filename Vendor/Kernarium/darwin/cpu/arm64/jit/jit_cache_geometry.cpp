// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_cache_geometry.hpp"

#include "jit_cache.hpp"
#include "jit_executor_pool.hpp"
#include "jit_linking.hpp"
#include "jit_runtime_contract.hpp"
#include "memory/tlb.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

namespace uml::cpu::arm64 {

namespace {

/* Used when nothing published a budget: unit tests, embedders that never call
 * the setter, and any path that creates an executor before startup has parsed
 * the guest's memory size. Generous enough that a single executor still holds
 * a real working set, small enough that a forgotten setter cannot reach the
 * multi-gigabyte totals this type exists to prevent.
 */
constexpr std::size_t default_total_bytes = 512U * 1024U * 1024U;
constexpr unsigned default_executors = 1;

/* Shares of the per-executor budget. The block cache and its saved guest words
 * decide how much of the working set stays resident, the arena decides how
 * much translated code survives before a reset discards the cache, and the
 * link tables only have to cover the blocks the cache can hold.
 */
constexpr std::size_t share_denominator = 100;
constexpr std::size_t lookup_share = 30;
constexpr std::size_t arena_share = 45;
constexpr std::size_t tracked_store_share = 8;

constexpr std::size_t minimum_cache_sets = 1;
constexpr std::size_t maximum_cache_sets = 131072;
constexpr std::size_t minimum_arena_bytes = 16U * 1024U;
constexpr std::size_t maximum_arena_bytes = 512U * 1024U * 1024U;
constexpr std::size_t minimum_tracked_store_bytes = 16U * 1024U;
constexpr std::size_t maximum_tracked_store_bytes = 32U * 1024U * 1024U;
/* Each registered target consumes a stable data record and reverse-index
 * storage, while a useful linked source also consumes code and alignment.
 * This stride funds both within the private budget. Records are keyed by
 * address space and reclaimed with the whole cache, so a workload that
 * reaches the table capacity first recycles the cache as a full arena does.
 */
constexpr std::size_t direct_link_record_budget_stride = 256U;

/* Darwin supports 4 KiB and 16 KiB host pages. Charging every mapping at the
 * larger granule plus one granule of VM/allocation metadata keeps the policy
 * independent of the host on which pure geometry tests execute.
 */
constexpr std::size_t mapping_granule = 16U * 1024U;
constexpr std::size_t mapping_allocation_allowance = mapping_granule;
constexpr std::size_t allocation_allowance = 64U;
/* Besides non-TLB inline Implementation state, this covers the owning heap
 * block and the admitted executor's share of the small process publication
 * registry backing that is populated during construction.
 */
constexpr std::size_t executor_state_allocation_allowance = 64U * 1024U;
constexpr std::size_t maximum_relocated_link_pointers =
	maximum_block_instructions;

struct JitCacheBudget final {
	std::size_t total_bytes{};
	unsigned executors{};
	std::size_t per_executor_bytes{};

	[[nodiscard]] bool operator==(const JitCacheBudget &) const noexcept =
		default;
};

struct JitCacheBudgetRegistry final {
	std::mutex mutex;
	std::optional<JitCacheBudget> published;
	unsigned live_executors{};
	bool run_session_active{};
	bool run_session_release_pending{};
};

[[nodiscard]] JitCacheBudgetRegistry &budget_registry() noexcept
{
	static JitCacheBudgetRegistry registry;
	return registry;
}

[[nodiscard]] std::size_t share_of(
	std::size_t value, std::size_t numerator) noexcept
{
	return (value / share_denominator) * numerator +
		((value % share_denominator) * numerator) / share_denominator;
}

[[nodiscard]] std::size_t rounded_mapping_bytes(
	std::size_t bytes) noexcept
{
	if (bytes > std::numeric_limits<std::size_t>::max() -
			(mapping_granule - 1U) - mapping_allocation_allowance)
		return std::numeric_limits<std::size_t>::max();
	return ((bytes + mapping_granule - 1U) & ~(mapping_granule - 1U)) +
		mapping_allocation_allowance;
}

bool checked_add(
	std::size_t &total, std::size_t value) noexcept
{
	if (value > std::numeric_limits<std::size_t>::max() - total) {
		total = std::numeric_limits<std::size_t>::max();
		return false;
	}
	total += value;
	return true;
}

bool checked_add_product(
	std::size_t &total, std::size_t count, std::size_t element) noexcept
{
	if (count != 0 &&
	    element > (std::numeric_limits<std::size_t>::max() - total) / count) {
		total = std::numeric_limits<std::size_t>::max();
		return false;
	}
	total += count * element;
	return true;
}

[[nodiscard]] JitCacheBudget published_budget_locked(
	JitCacheBudgetRegistry &registry) noexcept
{
	if (!registry.published)
		registry.published = JitCacheBudget{
			default_total_bytes, default_executors,
			default_jit_cache_budget_per_executor};
	return *registry.published;
}

[[nodiscard]] JitCacheGeometry minimum_geometry() noexcept
{
	return {.cache_sets = minimum_cache_sets,
		.block_links =
			minimum_arena_bytes / direct_link_record_budget_stride,
		.indirect_links = minimum_cache_sets * cache_ways,
		.arena_bytes = minimum_arena_bytes,
		.tracked_store_arena_bytes = minimum_tracked_store_bytes};
}

} // namespace

bool JitCacheGeometry::valid() const noexcept
{
	return cache_sets != 0 && (cache_sets & (cache_sets - 1U)) == 0 &&
		block_links != 0 && indirect_links != 0 && arena_bytes != 0 &&
		tracked_store_arena_bytes != 0;
}

std::size_t JitCacheGeometry::total_bytes() const noexcept
{
	std::size_t total = 0;
	/* JitExecutor::Implementation is itself one eager allocation. Tlb is its
	 * dominant inline payload; the fixed allowance covers every other inline
	 * field, the unique allocation's bookkeeping, and ordinary layout growth.
	 */
	checked_add(total, sizeof(::uml::memory::Tlb));
	checked_add(total, executor_state_allocation_allowance);
	/* BlockCache's owning object and vector backing are distinct allocations. */
	checked_add(total, sizeof(BlockCache) + allocation_allowance);
	checked_add_product(total, cache_sets, sizeof(CacheSet));
	checked_add(total, allocation_allowance);
	/* The saved-word slab has one combined array/control allocation. */
	checked_add_product(total, cache_sets, sizeof(SavedGuestWordSet));
	checked_add(total, saved_guest_word_slab_allocation_allowance);
	/* Direct and indirect records and their indexes occupy writable data.
	 * Activation lists are intrusive; their context heads are inline state.
	 */
	checked_add(total, JitBlockLinkTable::allocation_bytes(block_links));
	checked_add(total,
		JitIndirectLinkTable::allocation_bytes(indirect_links));
	/* Shared materialization retains this bounded pointer scratch capacity. */
	checked_add_product(total, maximum_relocated_link_pointers,
		sizeof(std::uintptr_t));
	checked_add(total, allocation_allowance);
	checked_add(total, rounded_mapping_bytes(arena_bytes));
	checked_add(total, rounded_mapping_bytes(tracked_store_arena_bytes));
	checked_add(total,
		rounded_mapping_bytes(single_instruction_arena_size));
	return total;
}

bool set_jit_cache_budget(std::size_t total_bytes,
	unsigned executors, std::size_t per_executor_bytes) noexcept
{
	if (!derive_jit_cache_geometry(
			total_bytes, executors, per_executor_bytes).valid())
		return false;
	auto &registry = budget_registry();
	std::lock_guard lock{registry.mutex};
	const JitCacheBudget requested{
		total_bytes, executors, per_executor_bytes};
	if (!registry.published) {
		registry.published = requested;
		return true;
	}
	return *registry.published == requested;
}

JitCacheGeometry derive_jit_cache_geometry(std::size_t total_bytes,
	unsigned executors, std::size_t per_executor_bytes) noexcept
{
	if (total_bytes == 0 || executors == 0 ||
	    executors > maximum_jit_cache_executors || per_executor_bytes == 0 ||
	    per_executor_bytes > maximum_jit_cache_budget_per_executor)
		return {};
	const auto minimum = minimum_jit_cache_budget_per_executor();
	if (minimum > per_executor_bytes ||
	    minimum > (total_bytes - std::min(
			total_bytes, minimum_shared_translation_cache_bytes)) /
			executors)
		return {};
	const auto available =
		(total_bytes - minimum_shared_translation_cache_bytes) / executors;
	const auto budget = std::min(per_executor_bytes, available);

	const auto per_set = sizeof(CacheSet) + sizeof(SavedGuestWordSet);
	const auto lookup_bytes = share_of(budget, lookup_share);
	auto sets = std::clamp(
		std::bit_floor(lookup_bytes / per_set),
		minimum_cache_sets, maximum_cache_sets);

	auto arena = std::clamp(share_of(budget, arena_share),
		minimum_arena_bytes, maximum_arena_bytes);
	auto tracked_store = std::clamp(
		share_of(budget, tracked_store_share),
		minimum_tracked_store_bytes, maximum_tracked_store_bytes);

	auto geometry = JitCacheGeometry{
		.cache_sets = sets,
		/* Bound stable records in proportion to code capacity. Exhaustion
		 * recycles the cache like a full arena.
		 */
		.block_links = arena / direct_link_record_budget_stride,
		.indirect_links = sets * cache_ways,
		.arena_bytes = arena,
		.tracked_store_arena_bytes = tracked_store};
	/* Link indexes and mappings are deliberately outside lookup_share. Should
	 * they make a rounded
	 * set-count step exceed the budget, retain a usable smaller cache rather
	 * than publishing an over-budget geometry.
	 */
	while (geometry.total_bytes() > budget && sets > minimum_cache_sets) {
		sets /= 2U;
		geometry.cache_sets = sets;
		geometry.indirect_links = sets * cache_ways;
	}
	if (geometry.total_bytes() > budget && arena > minimum_arena_bytes) {
		const auto excess = geometry.total_bytes() - budget;
		/* total_bytes() charges the mapped, page-rounded size. Reducing the
		 * requested byte count by exactly excess can remain in the same page
		 * and therefore reduce the accounted size by nothing. Include one
		 * granule less of rounding slack so this single adjustment really
		 * removes at least excess whenever the arena has enough room.
		 */
		const auto rounded_excess =
			excess > std::numeric_limits<std::size_t>::max() -
				(mapping_granule - 1U)
			? std::numeric_limits<std::size_t>::max()
			: excess + mapping_granule - 1U;
		arena -= std::min(
			rounded_excess, arena - minimum_arena_bytes);
		geometry.arena_bytes = arena;
		geometry.block_links =
			arena / direct_link_record_budget_stride;
	}
	if (geometry.total_bytes() > budget &&
	    tracked_store > minimum_tracked_store_bytes) {
		const auto excess = geometry.total_bytes() - budget;
		const auto rounded_excess =
			excess > std::numeric_limits<std::size_t>::max() -
				(mapping_granule - 1U)
			? std::numeric_limits<std::size_t>::max()
			: excess + mapping_granule - 1U;
		tracked_store -= std::min(rounded_excess,
			tracked_store - minimum_tracked_store_bytes);
		geometry.tracked_store_arena_bytes = tracked_store;
	}
	if (geometry.total_bytes() > budget)
		return {};

	/* The fixed shares choose the relative lookup and tracked-store sizes.
	 * Give every remaining byte in the selected private cap to the ordinary
	 * code arena and its link records. An early arena reset discards the
	 * complete private block cache.
	 */
	auto low = geometry.arena_bytes;
	auto high = maximum_arena_bytes;
	while (low < high) {
		const auto candidate = low + (high - low + 1U) / 2U;
		auto expanded = geometry;
		expanded.arena_bytes = candidate;
		expanded.block_links =
			candidate / direct_link_record_budget_stride;
		if (expanded.total_bytes() <= budget)
			low = candidate;
		else
			high = candidate - 1U;
	}
	geometry.arena_bytes = low;
	geometry.block_links = low / direct_link_record_budget_stride;
	return geometry;
}

std::size_t derive_shared_translation_cache_budget(
	std::size_t total_bytes, unsigned executors,
	std::size_t per_executor_bytes) noexcept
{
	const auto geometry = derive_jit_cache_geometry(
		total_bytes, executors, per_executor_bytes);
	if (!geometry.valid() ||
	    geometry.total_bytes() > total_bytes / executors)
		return 0;
	return total_bytes - geometry.total_bytes() * executors;
}

std::size_t minimum_jit_cache_budget_per_executor() noexcept
{
	return minimum_geometry().total_bytes();
}

std::size_t minimum_jit_cache_budget(unsigned executors) noexcept
{
	if (executors == 0 || executors > maximum_jit_cache_executors)
		return 0;
	const auto per_executor = minimum_jit_cache_budget_per_executor();
	if (per_executor >
	    (std::numeric_limits<std::size_t>::max() -
	     minimum_shared_translation_cache_bytes) / executors)
		return 0;
	return per_executor * executors +
		minimum_shared_translation_cache_bytes;
}

JitCacheGeometry jit_cache_geometry() noexcept
{
	auto &registry = budget_registry();
	std::lock_guard lock{registry.mutex};
	const auto budget = published_budget_locked(registry);
	return derive_jit_cache_geometry(
		budget.total_bytes, budget.executors, budget.per_executor_bytes);
}

std::size_t jit_shared_translation_cache_budget() noexcept
{
	auto &registry = budget_registry();
	std::lock_guard lock{registry.mutex};
	const auto budget = published_budget_locked(registry);
	return derive_shared_translation_cache_budget(
		budget.total_bytes, budget.executors, budget.per_executor_bytes);
}

JitCacheExecutorLease::JitCacheExecutorLease() noexcept
	: JitCacheExecutorLease{Admission::standalone}
{
}

JitCacheExecutorLease::JitCacheExecutorLease(Admission admission) noexcept
{
	auto &registry = budget_registry();
	{
		std::lock_guard lock{registry.mutex};
		const auto budget = published_budget_locked(registry);
		if (admission == Admission::managed_pool) {
			if (!registry.run_session_active ||
			    registry.run_session_release_pending)
				return;
		} else if (registry.run_session_active ||
			   registry.run_session_release_pending) {
			return;
		}
		if (registry.live_executors >= budget.executors)
			return;
		geometry_ = derive_jit_cache_geometry(
			budget.total_bytes, budget.executors,
			budget.per_executor_bytes);
		if (!geometry_.valid())
			return;
		++registry.live_executors;
		admitted_ = true;
	}
}

JitCacheExecutorLease::~JitCacheExecutorLease()
{
	if (!admitted_)
		return;
	auto &registry = budget_registry();
	std::lock_guard lock{registry.mutex};
	if (registry.live_executors != 0)
		--registry.live_executors;
	/* A composition bug may release the owning run session before its final
	 * executor. Keep admission closed while any executor remains, then recover
	 * automatically instead of poisoning every later run in the process.
	 */
	if (registry.live_executors == 0 &&
	    registry.run_session_release_pending) {
		registry.run_session_release_pending = false;
		registry.run_session_active = false;
	}
}

JitCacheExecutorLease::JitCacheExecutorLease(
	JitCacheExecutorLease &&other) noexcept
	: geometry_{other.geometry_}, admitted_{other.admitted_}
{
	other.geometry_ = {};
	other.admitted_ = false;
}

JitCacheRunSession::JitCacheRunSession() noexcept
	: admitted_{acquire_jit_cache_run_session()}
{
}

JitCacheRunSession::~JitCacheRunSession()
{
	if (admitted_)
		release_jit_cache_run_session();
}

namespace {

[[nodiscard]] bool acquire_jit_cache_run_session_impl(
#if UML_JIT_RUNTIME_DIAGNOSTICS
	JitCacheRunSessionReservedHook reserved_hook,
	void *reserved_hook_context
#endif
	) noexcept
{
	auto &registry = budget_registry();
	unsigned executors = 0;
	{
		std::lock_guard lock{registry.mutex};
		const auto budget = published_budget_locked(registry);
		if (registry.run_session_active ||
		    registry.run_session_release_pending ||
		    registry.live_executors != 0)
			return false;
		registry.run_session_active = true;
		executors = budget.executors;
	}
#if UML_JIT_RUNTIME_DIAGNOSTICS
	if (reserved_hook != nullptr)
		reserved_hook(reserved_hook_context);
#endif
	/* Pool activation allocates no executor, but it has its own lifecycle lock.
	 * Never nest that lock with the budget registry: pooled executor teardown
	 * destroys a lease, whose destructor returns here for the registry lock.
	 */
	if (activate_jit_executor_pool(executors))
		return true;
	std::lock_guard lock{registry.mutex};
	if (registry.live_executors == 0) {
		registry.run_session_active = false;
		registry.run_session_release_pending = false;
	} else {
		/* Pool activation allocates nothing, and its private construction
		 * authority is unreachable before activation. Preserve fail-closed
		 * recovery if that invariant is ever violated instead of reopening
		 * standalone admission around a live managed lease.
		 */
		registry.run_session_release_pending = true;
	}
	return false;
}

} // namespace

bool acquire_jit_cache_run_session() noexcept
{
#if UML_JIT_RUNTIME_DIAGNOSTICS
	return acquire_jit_cache_run_session_impl(nullptr, nullptr);
#else
	return acquire_jit_cache_run_session_impl();
#endif
}

#if UML_JIT_RUNTIME_DIAGNOSTICS
bool acquire_jit_cache_run_session_after_reservation_for_test(
	JitCacheRunSessionReservedHook hook, void *context) noexcept
{
	return acquire_jit_cache_run_session_impl(hook, context);
}
#endif

void release_jit_cache_run_session() noexcept
{
	auto &registry = budget_registry();
	{
		std::lock_guard lock{registry.mutex};
		if (!registry.run_session_active)
			return;
	}
	/* The normal path destroys every idle pooled executor synchronously. An
	 * explicitly misordered active borrow remains owned by its fixed slot and
	 * retires itself after run() returns. Do not hold the registry lock while
	 * either path destroys a lease.
	 */
	drain_jit_executor_pool();
	{
		std::lock_guard lock{registry.mutex};
		if (!registry.run_session_active)
			return;
		if (registry.live_executors == 0) {
			registry.run_session_active = false;
			registry.run_session_release_pending = false;
		} else {
			/* Fail closed until the final old executor lease retires. Publishing a
			 * new session while those mappings remain would recreate the cross-run
			 * multiplication this gate exists to prevent.
			 */
			registry.run_session_release_pending = true;
		}
	}
}

std::size_t parse_byte_quantity(const char *text) noexcept
{
	if (text == nullptr || *text == '\0')
		return 0;
	std::uint64_t value = 0;
	const char *cursor = text;
	for (; *cursor >= '0' && *cursor <= '9'; ++cursor) {
		const auto digit =
			static_cast<std::uint64_t>(*cursor - '0');
		if (value > (~std::uint64_t{} - digit) / 10U)
			return 0;
		value = value * 10U + digit;
	}
	if (cursor == text)
		return 0;
	std::uint64_t multiplier = 1;
	switch (std::tolower(static_cast<unsigned char>(*cursor))) {
	case 'k':
		multiplier = 1024U;
		++cursor;
		break;
	case 'm':
		multiplier = 1024U * 1024U;
		++cursor;
		break;
	case 'g':
		multiplier = 1024U * 1024U * 1024U;
		++cursor;
		break;
	default:
		break;
	}
	if (*cursor != '\0')
		return 0;
	if (value > ~std::uint64_t{} / multiplier)
		return 0;
	return static_cast<std::size_t>(value * multiplier);
}

} // namespace uml::cpu::arm64
