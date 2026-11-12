// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>

#ifndef UML_JIT_RUNTIME_DIAGNOSTICS
#define UML_JIT_RUNTIME_DIAGNOSTICS 0
#endif

namespace uml::cpu::arm64 {

class JitExecutor;

/*
 * How much retained host memory the JIT cache may spend, and how it is
 * divided. This covers every allocation made eagerly for an admitted
 * executor and every canonical entry retained by the process-global shared
 * translation cache. Short-lived compiler results and rejected publication
 * candidates remain transient caller scratch rather than cache-owned storage.
 *
 * The total is chosen and every per-executor structure is derived from it
 * (G135), so retained JIT memory stays bounded however many guest CPUs run.
 *
 * The budget is a whole-process figure. Each expected executor, one per guest
 * CPU, receives a bounded private working-set cache and the process-global
 * shared translation cache receives the remainder. The whole budget defaults
 * to half of guest memory, which the emulator reads from the guest's own
 * `mem=` argument. `--jit-cache-budget=` overrides the whole budget and
 * `--jit-executor-cache-budget=` may lower the private per-executor cap.
 */
struct JitCacheGeometry final {
	std::size_t cache_sets{};
	std::size_t block_links{};
	std::size_t indirect_links{};
	std::size_t arena_bytes{};
	std::size_t tracked_store_arena_bytes{};

	[[nodiscard]] bool operator==(const JitCacheGeometry &) const noexcept =
		default;
	[[nodiscard]] bool valid() const noexcept;
	[[nodiscard]] std::size_t total_bytes() const noexcept;
};

inline constexpr unsigned maximum_jit_cache_executors = 64;

/* Private caches are L1 working sets in front of the process-global canonical
 * translation cache. Their complete retained geometry, not merely the block
 * lookup array, is capped at 256 MiB per executor. The shared tier receives all
 * remaining budget and grows with guest memory.
 */
inline constexpr std::size_t default_jit_cache_budget_per_executor =
	256U * 1024U * 1024U;

/* The host-only override may lower the private working set for diagnosis. It
 * cannot grow one executor beyond the production 256 MiB ceiling; after that
 * ceiling is funded, only the shared tier grows with guest memory.
 */
inline constexpr std::size_t maximum_jit_cache_budget_per_executor =
	256U * 1024U * 1024U;

/* A small fixed accounting reservation covers the lazy shard directory even
 * before the first shard receives an entry.
 */
inline constexpr std::size_t minimum_shared_translation_cache_bytes =
	64U * 1024U;

/* Publishes one indivisible whole-process budget and executor-count pair.
 * Called during startup, before any executor exists. The first valid pair wins,
 * an identical repeat succeeds, and a different later pair fails so a new run
 * cannot silently reuse geometry derived for another configuration. Invalid
 * values fail without publishing anything. Constructing an executor before
 * explicit publication claims the documented fallback pair.
 */
[[nodiscard]] bool set_jit_cache_budget(std::size_t total_bytes,
	unsigned executors,
	std::size_t per_executor_bytes =
		default_jit_cache_budget_per_executor) noexcept;

/* Pure geometry derivation used by the published process configuration and by
 * budget-accounting tests. total_bytes is the whole-process JIT budget.
 */
[[nodiscard]] JitCacheGeometry derive_jit_cache_geometry(
	std::size_t total_bytes, unsigned executors,
	std::size_t per_executor_bytes =
		default_jit_cache_budget_per_executor) noexcept;

/* Process-global share retained after all per-executor minimums have been
 * funded. Zero means the whole-process pair is invalid.
 */
[[nodiscard]] std::size_t derive_shared_translation_cache_budget(
	std::size_t total_bytes, unsigned executors,
	std::size_t per_executor_bytes =
		default_jit_cache_budget_per_executor) noexcept;

/* Smallest accounted per-executor payload that can construct usable cache and
 * arena resources. A whole-process budget must provide at least this much to
 * every executor.
 */
[[nodiscard]] std::size_t
minimum_jit_cache_budget_per_executor() noexcept;

/* Exact minimum whole-process budget, including the shared-cache base. */
[[nodiscard]] std::size_t
minimum_jit_cache_budget(unsigned executors) noexcept;

/* The per-executor geometry derived from the published budget. */
[[nodiscard]] JitCacheGeometry jit_cache_geometry() noexcept;

/* Shared-cache share belonging to the published process configuration. */
[[nodiscard]] std::size_t jit_shared_translation_cache_budget() noexcept;

/* An executor lease is acquired before any cache or arena allocation. At
 * most the configured executor count may be live. Managed emulator runs keep
 * those leases in a session-owned pool and borrow them across lightweight task
 * wrappers. Standalone diagnostic and embedding executors use the public
 * lease path only outside a managed session; the pool uses a private tagged
 * path only while such a session is active. A rejected lease carries invalid
 * geometry and allocates no JIT-owned backing resources.
 */
class JitCacheExecutorLease final {
public:
	JitCacheExecutorLease() noexcept;
	~JitCacheExecutorLease();
	JitCacheExecutorLease(JitCacheExecutorLease &&other) noexcept;

	JitCacheExecutorLease(const JitCacheExecutorLease &) = delete;
	JitCacheExecutorLease &operator=(const JitCacheExecutorLease &) = delete;
	JitCacheExecutorLease &operator=(JitCacheExecutorLease &&) = delete;

	[[nodiscard]] bool admitted() const noexcept { return admitted_; }
	[[nodiscard]] const JitCacheGeometry &geometry() const noexcept
	{
		return geometry_;
	}

private:
	friend class JitExecutor;
	enum class Admission {
		standalone,
		managed_pool,
	};
	explicit JitCacheExecutorLease(Admission admission) noexcept;

	JitCacheGeometry geometry_{};
	bool admitted_{};
};

/* Emulator runs take a process-wide session before entering a JIT-capable
 * kernel. This activates the bounded executor pool and prevents two identical
 * configurations from splitting it and partially starting two guests.
 */
class JitCacheRunSession final {
public:
	JitCacheRunSession() noexcept;
	~JitCacheRunSession();

	JitCacheRunSession(const JitCacheRunSession &) = delete;
	JitCacheRunSession &operator=(const JitCacheRunSession &) = delete;

	[[nodiscard]] bool admitted() const noexcept { return admitted_; }

private:
	bool admitted_{};
};

[[nodiscard]] bool acquire_jit_cache_run_session() noexcept;
void release_jit_cache_run_session() noexcept;

#if UML_JIT_RUNTIME_DIAGNOSTICS
using JitCacheRunSessionReservedHook = void (*)(void *) noexcept;
/* Pauses diagnostic activation after the registry reserves the managed run but
 * before the executor pool publishes its session token. */
[[nodiscard]] bool acquire_jit_cache_run_session_after_reservation_for_test(
	JitCacheRunSessionReservedHook hook, void *context) noexcept;
#endif

/* Parses a byte count with an optional K/M/G suffix, as used by
 * `--jit-cache-budget=`. Returns zero if unparsable.
 */
[[nodiscard]] std::size_t parse_byte_quantity(const char *text) noexcept;

} // namespace uml::cpu::arm64
